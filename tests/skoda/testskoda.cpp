// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM
//
// Tests of the Škoda plugin core against a local HTTP server replaying
// responses built on the structure of the MyŠkoda contract 1.1.0. The two
// contract tests need the contract itself, which is not in this repository:
// they are skipped without it (see docs/skoda/README.md and SKODA_OPENAPI).
// Every exchange goes through the event loop (QSignalSpy::wait), so a test
// cannot pass on the state from before the response.

#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>

#include <integrations/pluginmetadata.h>

#include "fakeskodaserver.h"
#include "skodaapiclient.h"
#include "skodapoller.h"
#include "skodastatemodel.h"
#include "skodasettings.h"
#include "skodathingsettings.h"

#include <QSettings>
#include <QTemporaryDir>

namespace {

// Fictitious VIN of our own (ISO 3779 characters, no maker WMI) and a fake key (SK-600).
const QString kVin = QStringLiteral("ETM0SK0DA00000001");
const QString kApiKey = QStringLiteral("fake-test-key-0123456789-WXYZ");
const QString kNewApiKey = QStringLiteral("fake-test-key-9876543210-ABCD");

QStringList g_log;
QtMessageHandler g_previousHandler = nullptr;

void captureMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    g_log.append(QStringLiteral("%1 %2").arg(QString::fromLatin1(context.category ? context.category : "")).arg(message));
    if (qEnvironmentVariableIsSet("SKODA_TEST_VERBOSE") && g_previousHandler)
        g_previousHandler(type, context, message);
}

int countLog(const QString &needle)
{
    int count = 0;
    for (const QString &line : std::as_const(g_log))
        if (line.contains(needle))
            count++;
    return count;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Cannot open %s", qPrintable(path));
    return file.readAll();
}

QByteArray fixture(const QString &name)
{
    return readFile(QStringLiteral(SOURCE_ROOT "/tests/skoda/fixtures/") + name);
}

// The contract belongs to Škoda and is not in this repository (docs/skoda/README.md
// says how to download it). Path: $SKODA_OPENAPI when set, else docs/skoda/.
QString contractPath()
{
    const QString fromEnvironment = qEnvironmentVariable("SKODA_OPENAPI");
    return fromEnvironment.isEmpty() ? QStringLiteral(SOURCE_ROOT "/docs/skoda/openapi-1.1.0.json") : fromEnvironment;
}

// Empty object when the contract is absent or unreadable.
QJsonObject contract()
{
    QFile file(contractPath());
    if (!file.open(QIODevice::ReadOnly))
        return QJsonObject();
    return QJsonDocument::fromJson(file.readAll()).object();
}

QByteArray missingContractMessage()
{
    return QStringLiteral("Škoda contract not found at %1. It is not distributed with this repository: download "
                          "https://public.api.connect.skoda-auto.cz/docs/myskoda-public-api.yaml, check its SHA-256 "
                          "(fb0514bf3e289e4bfb634f13dd5b7dfd24eb06cbab829ccf6a6fbb831ecf4cfc) and convert it to JSON "
                          "as docs/skoda/README.md shows, then put it at docs/skoda/openapi-1.1.0.json or set "
                          "SKODA_OPENAPI to its path.")
        .arg(contractPath())
        .toUtf8();
}

QJsonObject schema(const QJsonObject &contract, const QString &name)
{
    return contract.value("components").toObject().value("schemas").toObject().value(name).toObject();
}

QJsonObject resolve(const QJsonObject &contract, QJsonObject object)
{
    while (object.contains("$ref")) {
        const QString ref = object.value("$ref").toString();
        object = schema(contract, ref.section('/', -1));
    }
    return object;
}

// Every key of a fixture exists in the schema (no invented field name).
void checkAgainstSchema(const QJsonObject &contract, const QJsonValue &value, QJsonObject schemaObject,
                        const QString &path, QStringList *problems)
{
    schemaObject = resolve(contract, schemaObject);
    if (value.isObject()) {
        const QJsonObject properties = schemaObject.value("properties").toObject();
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!properties.contains(it.key())) {
                problems->append(path + "." + it.key());
                continue;
            }
            checkAgainstSchema(contract, it.value(), properties.value(it.key()).toObject(), path + "." + it.key(), problems);
        }
    } else if (value.isArray()) {
        const QJsonObject items = schemaObject.value("items").toObject();
        for (const QJsonValue &item : value.toArray())
            checkAgainstSchema(contract, item, items, path + "[]", problems);
    }
}

}

// Stand-in for nymea's Thing (libnymea/integrations/thing.cpp, tag 1.15.2),
// whose constructor is private: the members SkodaThingSettings uses, with the
// same behaviour. setSettings() emits settingChanged for every setting, even
// unchanged (l. 255-261); setSettingValue() only when the value changes
// (l. 279-302).
class FakeThing : public QObject
{
    Q_OBJECT
public:
    enum ThingSetupStatus { ThingSetupStatusNone, ThingSetupStatusInProgress, ThingSetupStatusComplete, ThingSetupStatusFailed };

    explicit FakeThing(const QUuid &id, QObject *parent = nullptr) : QObject(parent), m_id(id) {}
    QUuid id() const { return m_id; }
    QString name() const { return QStringLiteral("Enyaq"); }
    ThingSetupStatus setupStatus() const { return m_status; }
    void setSetupStatus(ThingSetupStatus status) { m_status = status; }
    QVariant setting(const QUuid &id) const { return m_settings.value(id); }
    QHash<QUuid, QVariant> settings() const { return m_settings; }
    void setSettings(const QList<QPair<QUuid, QVariant>> &settings)
    {
        m_settings.clear();
        for (const auto &setting : settings)
            m_settings.insert(setting.first, setting.second);
        for (const auto &setting : settings)
            emit settingChanged(setting.first, setting.second);
    }
    void setSettingValue(const QUuid &id, const QVariant &value)
    {
        if (!m_settings.contains(id) || m_settings.value(id) == value)
            return;
        m_settings.insert(id, value);
        emit settingChanged(id, value);
    }

signals:
    void settingChanged(const QUuid &paramTypeId, const QVariant &value);

private:
    QUuid m_id;
    ThingSetupStatus m_status = ThingSetupStatusNone;
    QHash<QUuid, QVariant> m_settings;
};

class TestSkoda : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // SK-101: contract, fixtures and plugin description.
    void contractDescribesEveryFieldRead();
    void fixturesFollowTheContract();
    void pluginJsonIsValidForNymea1152();

    // SK-100, SK-204, SK-602, SK-603, SK-604
    void requestAsksForTheChargingPartOnly();
    void onlyHttpsOrLoopback();
    void vinValidation();
    void keyIsMasked();

    // SK-3xx
    void full200FillsEveryState();
    void onlyTheListedStatesArePublished();
    void unreadAndUnknownFieldsAreIgnored();
    void missingChargingPartKeepsStates();
    void missingSocAndPlugKeepLastValues();
    void unknownEnumValueIsUnknownAndLoggedOnce();
    void receptionTimestampWithoutVehicleTimestamp();
    void sameVehicleTimestampDoesNotAdvance();
    void socUnknownUntilFirstReading();
    void socKnownKeptAcrossRestart();
    void batteryCriticalBelow30();
    void batteryCriticalBelow30_data();
    void keyExpiringSoon();
    void keyExpiringSoon_data();

    // SK-2xx
    void adaptiveInterval();
    void unpluggedNoRequestFor24Hours();
    void unpluggedFailureIsRetried();
    void refreshRefusedWithinGapAcceptedAfter();
    void refreshIgnoresThePeriodicPlan();
    void refreshRefusedDuringBackoff();
    void refreshRefusedDuring429();
    void refreshRefusedDuringQuotaPause();
    void refreshRefusedWhileHalted();
    void refreshJoinsARunningRequest();
    void statesAreSetBeforeTheReadingEnds();
    void pairingAnswerIsTheFirstReading();
    void pairingAnswerWithoutChargingIsNotUsed();
    void quotaReservePauses();
    void quotaAtReserveDoesNotPause();
    void rateLimited429WaitsRetryAfter();
    void vehicleNotAccepting429();
    void serverErrorBackoff();
    void serverErrorRetryAfterLongerThanBackoff();
    void networkErrorBacksOff();
    void gatewayTimeoutIsVehicleUnavailable();
    void unreadable200BacksOff();
    void startPollsImmediately();
    void restartDoesNotBurst();
    void intervalParamsMatchTheBounds();
    void intervalsOutsideBoundsAreRefused();
    void intervalsOutsideBoundsAreRefused_data();
    void configuredIntervalsAreUsed();
    void intervalChangeMovesThePlanWithoutRequest();
    void intervalSettingResolution();
    void intervalSettingResolution_data();
    void intervalSurvivesAReconfigurationThroughPairing();
    void capacitySettingResolution();
    void capacitySettingResolution_data();
    void capacityStateFollowsTheSetting();
    void acPhaseCountSettingResolution();
    void acPhaseCountSettingResolution_data();
    void phaseStatesFollowTheSetting();
    void phaseSettingOutsideOneToThreeIsRefused();
    void phaseSettingOutsideOneToThreeIsRefused_data();
    void settingsSurviveTheNymeaPairingOrder();
    void firstReadAtOnceAfterRestartIsAccepted();
    void newKeyRespectsMinimumGap();
    void lastRequestInTheFutureCountsAsNow();

    // SK-103, SK-402
    void key401ExpiredHaltsUntilNewKey();
    void key401InvalidHalts();
    void key403NotAuthorizedHalts();
    void vehicle404Halts();

    // SK-400, SK-401
    void everyCycleIsLogged();
    void apiKeyNeverInLogs();
    void vinNeverInLogs();

private:
    void createSession(const QString &apiKey = kApiKey);
    void destroySession();
    SkodaResponse cycle(); // one request, event loop until the answer
    void advance(int seconds) { m_now = m_now.addSecs(seconds); }
    // Action "refresh": returns the result; on Sent, waits for the answer.
    SkodaPoller::RefreshResult refreshNow(QString *refusal = nullptr);
    static QString localHHmm(const QDateTime &time)
    {
        const qint64 seconds = (time.toSecsSinceEpoch() + 59) / 60 * 60;
        return QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC).toLocalTime().toString("HH:mm");
    }
    // To the earliest time the poller allows the next request.
    void advanceToNext() { if (m_poller->notBefore().isValid()) m_now = std::max(m_now, m_poller->notBefore()); }
    qint64 waitSeconds() const { return m_now.secsTo(m_poller->notBefore()); }

    static QList<QPair<QByteArray, QByteArray>> okHeaders(int remaining = 17, int reset = 1800, int expiresInDays = 120);
    FakeResponse ok(const QString &fixtureName, const QList<QPair<QByteArray, QByteArray>> &headers = {}) const;
    // vehicle-200-full.json with another stateOfChargeInPercent.
    FakeResponse okWithSoc(int soc) const;
    FakeResponse problem(int status, const QString &fixtureName, const QList<QPair<QByteArray, QByteArray>> &headers = {}) const;

    static QDateTime s_start;
    QDateTime m_now;
    FakeSkodaServer *m_server = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    SkodaApiClient *m_client = nullptr;
    SkodaPoller *m_poller = nullptr;
    SkodaStateModel *m_model = nullptr;
};

QDateTime TestSkoda::s_start = QDateTime(QDate(2026, 11, 2), QTime(8, 0), QTimeZone::UTC);

void TestSkoda::initTestCase()
{
    QLoggingCategory::setFilterRules(QStringLiteral("Skoda.debug=true"));
    g_previousHandler = qInstallMessageHandler(captureMessage);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
}

void TestSkoda::init()
{
    g_log.clear();
    m_now = s_start;
    m_server = new FakeSkodaServer(this);
    QVERIFY(m_server->start());
    m_network = new QNetworkAccessManager(this);
    createSession();
}

void TestSkoda::cleanup()
{
    destroySession();
    delete m_network;
    m_network = nullptr;
    delete m_server;
    m_server = nullptr;
}

void TestSkoda::createSession(const QString &apiKey)
{
    QNetworkAccessManager *network = m_network;
    auto get = [network](const QNetworkRequest &request) { return network->get(request); };
    auto clock = [this]() { return m_now; };
    m_client = new SkodaApiClient(get, m_server->baseUrl(), kVin, apiKey, this);
    m_client->setClock(clock);
    m_poller = new SkodaPoller(m_client, this);
    m_poller->setClock(clock);
    m_model = new SkodaStateModel(this);
    m_model->setClock(clock);
    m_model->setMaskedApiKey(m_client->maskedApiKey());
    m_model->setVin(kVin);
    connect(m_poller, &SkodaPoller::responseReceived, m_model, &SkodaStateModel::applyResponse);
}

void TestSkoda::destroySession()
{
    delete m_poller;
    delete m_model;
    delete m_client;
    m_poller = nullptr;
    m_model = nullptr;
    m_client = nullptr;
}

SkodaResponse TestSkoda::cycle()
{
    QSignalSpy spy(m_poller, &SkodaPoller::responseReceived);
    if (!m_poller->pollNow()) {
        qWarning() << "pollNow() refused the request";
        return SkodaResponse();
    }
    if (!spy.wait(5000)) {
        qWarning() << "no response within 5 s";
        return SkodaResponse();
    }
    return spy.first().first().value<SkodaResponse>();
}

SkodaPoller::RefreshResult TestSkoda::refreshNow(QString *refusal)
{
    QSignalSpy spy(m_model, &SkodaStateModel::responseApplied);
    const SkodaPoller::RefreshResult result = m_poller->refresh(refusal);
    if (result == SkodaPoller::RefreshResult::Sent && !spy.wait(5000))
        qWarning() << "no response within 5 s";
    return result;
}

QList<QPair<QByteArray, QByteArray>> TestSkoda::okHeaders(int remaining, int reset, int expiresInDays)
{
    return {
        {"X-API-Key-Expires-At", s_start.addDays(expiresInDays).toString(Qt::ISODate).toLatin1()},
        {"RateLimit-Limit", "20"},
        {"RateLimit-Remaining", QByteArray::number(remaining)},
        {"RateLimit-Reset", QByteArray::number(reset)},
    };
}

FakeResponse TestSkoda::ok(const QString &fixtureName, const QList<QPair<QByteArray, QByteArray>> &headers) const
{
    FakeResponse response;
    response.status = 200;
    response.headers = headers.isEmpty() ? okHeaders() : headers;
    response.body = fixture(fixtureName);
    return response;
}

FakeResponse TestSkoda::okWithSoc(int soc) const
{
    FakeResponse response = ok("vehicle-200-full.json");
    QJsonObject root = QJsonDocument::fromJson(response.body).object();
    QJsonObject vehicle = root.value("vehicle").toObject();
    QJsonObject charging = vehicle.value("charging").toObject();
    QJsonObject status = charging.value("status").toObject();
    QJsonObject battery = status.value("battery").toObject();
    battery.insert("stateOfChargeInPercent", soc);
    status.insert("battery", battery);
    charging.insert("status", status);
    vehicle.insert("charging", charging);
    root.insert("vehicle", vehicle);
    response.body = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return response;
}

FakeResponse TestSkoda::problem(int status, const QString &fixtureName, const QList<QPair<QByteArray, QByteArray>> &headers) const
{
    FakeResponse response;
    response.status = status;
    response.headers = headers;
    response.body = fixture(fixtureName);
    return response;
}

// ---------------------------------------------------------------------------
// SK-101

void TestSkoda::contractDescribesEveryFieldRead()
{
    const QJsonObject api = contract();
    if (api.isEmpty())
        QSKIP(missingContractMessage().constData());
    QCOMPARE(api.value("info").toObject().value("version").toString(), QStringLiteral("1.1.0"));
    QVERIFY(api.value("paths").toObject().contains("/api/v1/vehicles/{vin}"));

    QVERIFY(schema(api, "VehiclePart").value("enum").toArray().contains(QJsonValue("charging")));
    QVERIFY(schema(api, "VehicleResponse").value("properties").toObject().contains("errors"));
    QVERIFY(schema(api, "Vehicle").value("properties").toObject().contains("charging"));

    const QJsonObject charging = schema(api, "Charging").value("properties").toObject();
    for (const char *name : {"status", "settings", "carCapturedTimestamp"})
        QVERIFY2(charging.contains(name), name);
    const QJsonObject status = schema(api, "ChargingStatus").value("properties").toObject();
    for (const char *name : {"plugConnectionState", "battery"})
        QVERIFY2(status.contains(name), name);
    const QJsonObject battery = schema(api, "BatteryStatus").value("properties").toObject();
    QVERIFY(battery.contains("stateOfChargeInPercent"));
    QVERIFY(schema(api, "VehicleError").value("properties").toObject().contains("type"));

    const QJsonObject headers = api.value("components").toObject().value("headers").toObject();
    for (const char *name : {"ApiKeyExpiresAt", "RateLimitLimit", "RateLimitRemaining", "RateLimitReset", "RetryAfter"})
        QVERIFY2(headers.contains(name), name);
    const QJsonObject security = api.value("components").toObject().value("securitySchemes").toObject().value("apiKeyAuth").toObject();
    QCOMPARE(security.value("name").toString(), QStringLiteral("X-API-Key"));

    // Every value of the enumeration the plugin normalises is published in the contract.
    const QString plugDescription = status.value("plugConnectionState").toObject().value("description").toString();
    for (const char *value : {"CONNECTED", "DISCONNECTED"}) {
        QVERIFY2(plugDescription.contains(QStringLiteral("* ") + value), value);
        QVERIFY(SkodaStateModel::normalizePlugConnectionState(value) != QLatin1String("unknown"));
    }
}

void TestSkoda::fixturesFollowTheContract()
{
    const QDir dir(QStringLiteral(SOURCE_ROOT "/tests/skoda/fixtures"));
    const QStringList files = dir.entryList({"*.json"}, QDir::Files);
    QVERIFY(files.size() >= 10);

    // Checked with or without the contract: valid JSON, and no VIN but the test one (SK-600).
    for (const QString &file : files) {
        QJsonParseError error;
        QJsonDocument::fromJson(fixture(file), &error);
        QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(file));
        const QString text = QString::fromUtf8(fixture(file));
        static const QRegularExpression vinPattern(QStringLiteral("[A-HJ-NPR-Z0-9]{17}"));
        auto it = vinPattern.globalMatch(text);
        while (it.hasNext())
            QCOMPARE(it.next().captured(), kVin);
    }

    const QJsonObject api = contract();
    if (api.isEmpty())
        QSKIP(missingContractMessage().constData());
    QJsonObject vehicleResponse;
    vehicleResponse.insert("$ref", "#/components/schemas/VehicleResponse");
    QJsonObject problemDetail;
    problemDetail.insert("$ref", "#/components/schemas/ProblemDetail");
    for (const QString &file : files) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(fixture(file), &error);
        QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(file));
        QStringList problems;
        checkAgainstSchema(api, document.object(), file.startsWith("vehicle") ? vehicleResponse : problemDetail, file, &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(file + ": fields not in the contract: " + problems.join(", ")));
    }
}

void TestSkoda::pluginJsonIsValidForNymea1152()
{
    const QJsonObject json = QJsonDocument::fromJson(readFile(QStringLiteral(SOURCE_ROOT "/skoda/integrationpluginskoda.json"))).object();
    PluginMetadata metadata(json);
    QVERIFY2(metadata.isValid(), qPrintable(metadata.validationErrors().join("\n")));

    QCOMPARE(metadata.thingClasses().count(), 1);
    const ThingClass thingClass = metadata.thingClasses().first();
    QVERIFY(thingClass.interfaces().contains("electricvehicle"));
    QVERIFY(thingClass.interfaces().contains("battery"));
    QVERIFY(thingClass.interfaces().contains("connectable"));
    QCOMPARE(thingClass.setupMethod(), ThingClass::SetupMethodEnterPin);
    // SK-601: the key is not a param (params are returned by the JSON-RPC API).
    QCOMPARE(thingClass.paramTypes().count(), 1);
    QVERIFY(thingClass.paramTypes().findByName("vin").isValid());
    // The reading interval (lot car-7) and the capacity (lot car-8) are
    // settings: libnymea reads the bounds it checks on SetThingSettings.
    QCOMPARE(thingClass.settingsTypes().count(), 3);
    const ParamType capacity = thingClass.settingsTypes().findByName("capacity");
    QVERIFY(capacity.isValid());
    QCOMPARE(capacity.type(), QMetaType::Double);
    QCOMPARE(capacity.unit(), Types::UnitKiloWattHour);
    QCOMPARE(capacity.minValue().toDouble(), SkodaSettings::capacityKiloWattHours().minimum);
    QCOMPARE(capacity.maxValue().toDouble(), SkodaSettings::capacityKiloWattHours().maximum);
    QCOMPARE(capacity.defaultValue().toDouble(), SkodaSettings::capacityKiloWattHours().defaultValue);
    // The id of the former param is not reused.
    const ParamTypeId legacy(QString::fromLatin1(SkodaSettings::legacyBatteryCapacityParamTypeId));
    QVERIFY(!legacy.isNull());
    QVERIFY(!thingClass.paramTypes().findById(legacy).isValid());
    QVERIFY(!thingClass.settingsTypes().findById(legacy).isValid());
    const ParamType plugged = thingClass.settingsTypes().findByName("pluggedIntervalMinutes");
    QVERIFY(plugged.isValid());
    QCOMPARE(plugged.minValue().toInt(), SkodaPoller::pluggedIntervalMinMinutes);
    QCOMPARE(plugged.maxValue().toInt(), SkodaPoller::pluggedIntervalMaxMinutes);
    QCOMPARE(plugged.defaultValue().toInt(), SkodaPoller::pluggedIntervalSeconds / 60);
    // AC phases (lot skoda-phases): a setting, whole numbers 1-3, default 3.
    const ParamType phases = thingClass.settingsTypes().findByName("acPhaseCount");
    QVERIFY(phases.isValid());
    QCOMPARE(phases.type(), QMetaType::UInt);
    QCOMPARE(phases.minValue().toDouble(), SkodaSettings::acPhaseCount().minimum);
    QCOMPARE(phases.maxValue().toDouble(), SkodaSettings::acPhaseCount().maximum);
    QCOMPARE(phases.defaultValue().toDouble(), SkodaSettings::acPhaseCount().defaultValue);
    QCOMPARE(phases.defaultValue().toUInt(), 3u);
    // Published as acPhaseCount (electricvehicle from nymea 1.16) and
    // phaseCount (electricvehicle of nymea 1.15.2, read by the energy
    // manager), both with the type and bounds of the interfaces: uint 1-3.
    for (const QString &name : {QStringLiteral("acPhaseCount"), QStringLiteral("phaseCount")}) {
        const StateType state = thingClass.stateTypes().findByName(name);
        QVERIFY2(state.isValid(), qPrintable(name));
        QCOMPARE(state.type(), QMetaType::UInt);
        QCOMPARE(state.minValue().toUInt(), 1u);
        QCOMPARE(state.maxValue().toUInt(), 3u);
        QCOMPARE(state.defaultValue().toUInt(), 3u);
    }

    // The model starts from the same states and defaults as the plugin description.
    const QVariantMap defaults = SkodaStateModel::defaultStates();
    QCOMPARE(thingClass.stateTypes().count(), defaults.count());
    for (const StateType &stateType : thingClass.stateTypes()) {
        QVERIFY2(defaults.contains(stateType.name()), qPrintable(stateType.name()));
        QCOMPARE(defaults.value(stateType.name()), stateType.defaultValue());
    }
    // SK-302: plugState is never a boolean.
    QCOMPARE(thingClass.stateTypes().findByName("plugState").possibleValues().count(), 3);
}

// ---------------------------------------------------------------------------
// SK-100, SK-204, SK-602, SK-603, SK-604

void TestSkoda::requestAsksForTheChargingPartOnly()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_server->requestCount(), 1);
    const RecordedRequest request = m_server->requests().first();
    QCOMPARE(request.method, QByteArray("GET"));
    QCOMPARE(request.target, QByteArray("/api/v1/vehicles/ETM0SK0DA00000001?include=charging"));
    QCOMPARE(request.headers.value("x-api-key"), kApiKey.toUtf8());
    QVERIFY(!request.target.contains("parkingPosition"));
    QCOMPARE(SkodaApiClient::productionBaseUrl().toString(), QStringLiteral("https://public.api.connect.skoda-auto.cz"));
}

void TestSkoda::onlyHttpsOrLoopback()
{
    QVERIFY(SkodaApiClient::isAllowedBaseUrl(QUrl("https://public.api.connect.skoda-auto.cz")));
    QVERIFY(SkodaApiClient::isAllowedBaseUrl(QUrl("http://127.0.0.1:8080")));
    QVERIFY(SkodaApiClient::isAllowedBaseUrl(QUrl("http://localhost:8080")));
    QVERIFY(!SkodaApiClient::isAllowedBaseUrl(QUrl("http://public.api.connect.skoda-auto.cz")));
    QVERIFY(!SkodaApiClient::isAllowedBaseUrl(QUrl("http://172.16.0.10")));
    QVERIFY(!SkodaApiClient::isAllowedBaseUrl(QUrl("ftp://127.0.0.1")));

    // A client on plain http towards another host sends nothing.
    QNetworkAccessManager *network = m_network;
    int calls = 0;
    SkodaApiClient client([network, &calls](const QNetworkRequest &request) { calls++; return network->get(request); },
                          QUrl("http://example.invalid"), kVin, kApiKey);
    QVERIFY(!client.fetchVehicle());
    QCOMPARE(calls, 0);
}

void TestSkoda::vinValidation()
{
    QVERIFY(SkodaApiClient::isValidVin(kVin));
    QVERIFY(!SkodaApiClient::isValidVin("ETM0SK0DA0000000"));    // 16
    QVERIFY(!SkodaApiClient::isValidVin("ETM0SK0DA000000001"));  // 18
    QVERIFY(!SkodaApiClient::isValidVin("ETM0SK0DA0000000O"));   // O
    QVERIFY(!SkodaApiClient::isValidVin("etm0sk0da00000001"));   // lower case
    QVERIFY(!SkodaApiClient::isValidVin(""));
}

void TestSkoda::keyIsMasked()
{
    QCOMPARE(SkodaApiClient::maskApiKey(kApiKey), QStringLiteral("…WXYZ"));
    QCOMPARE(SkodaApiClient::maskApiKey("short"), QStringLiteral("…"));
    QCOMPARE(SkodaApiClient::maskApiKey(""), QStringLiteral("…"));
}

// ---------------------------------------------------------------------------
// SK-3xx

void TestSkoda::full200FillsEveryState()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    const SkodaResponse response = cycle();
    QVERIFY(response.isOk());

    QCOMPARE(m_model->state("connected").toBool(), true);
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("ok"));
    QCOMPARE(m_model->state("apiStatusMessage").toString(), QString());
    QCOMPARE(m_model->state("batteryLevel").toInt(), 58);
    QCOMPARE(m_model->state("socKnown").toBool(), true);
    QCOMPARE(m_model->state("batteryCritical").toBool(), false);
    QCOMPARE(m_model->state("socTimestampSource").toString(), QStringLiteral("vehicle"));
    QCOMPARE(m_model->state("socTimestamp").toLongLong(),
             QDateTime(QDate(2026, 10, 28), QTime(17, 42), QTimeZone::UTC).toSecsSinceEpoch());
    QCOMPARE(m_model->state("plugState").toString(), QStringLiteral("connected"));
    QCOMPARE(m_model->state("rateLimitRemaining").toInt(), 17);
    QCOMPARE(m_model->state("apiKeyExpiresAt").toLongLong(), s_start.addDays(120).toSecsSinceEpoch());
    QCOMPARE(m_model->state("apiKeyExpiringSoon").toBool(), false);
    // Capacity is not in the API: it stays what the user entered (0 = not entered).
    QCOMPARE(m_model->state("capacity").toDouble(), 0.0);
}

void TestSkoda::onlyTheListedStatesArePublished()
{
    // Lot car-5 (Patrick, 2026-11-02): the SoC, the plug state and the API
    // status only. The vehicle's charge targets are a HEMS setting, not read.
    const QStringList expected = {
        "connected", "batteryLevel", "socKnown", "batteryCritical", "capacity", "acPhaseCount", "phaseCount", "socTimestamp",
        "socTimestampSource", "plugState", "apiKeyExpiresAt", "apiKeyExpiringSoon", "apiStatus",
        "apiStatusMessage", "rateLimitRemaining"};
    QStringList model = SkodaStateModel::defaultStates().keys();
    QStringList sortedExpected = expected;
    model.sort();
    sortedExpected.sort();
    QCOMPARE(model, sortedExpected);

    QFile file(QStringLiteral(SOURCE_ROOT "/skoda/integrationpluginskoda.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject thingClass = QJsonDocument::fromJson(file.readAll()).object()
                                       .value("vendors").toArray().first().toObject()
                                       .value("thingClasses").toArray().first().toObject();
    QStringList published;
    for (const QJsonValue &state : thingClass.value("stateTypes").toArray())
        published.append(state.toObject().value("name").toString());
    published.sort();
    QCOMPARE(published, sortedExpected);

    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QStringList after = m_model->states().keys();
    after.sort();
    QCOMPARE(after, sortedExpected);
}

void TestSkoda::unreadAndUnknownFieldsAreIgnored()
{
    // A member the contract does not know yet, next to the members the plugin
    // does not read (range, charge limit, battery care, charging state...).
    QJsonObject root = QJsonDocument::fromJson(fixture("vehicle-200-full.json")).object();
    root.insert("someFutureMember", QJsonObject{{"value", 1}});
    QJsonObject vehicle = root.value("vehicle").toObject();
    QJsonObject charging = vehicle.value("charging").toObject();
    charging.insert("someFutureMember", "x");
    QJsonObject status = charging.value("status").toObject();
    status.insert("someFutureMember", 42);
    QJsonObject battery = status.value("battery").toObject();
    battery.insert("someFutureMember", true);
    status.insert("battery", battery);
    charging.insert("status", status);
    vehicle.insert("charging", charging);
    root.insert("vehicle", vehicle);
    FakeResponse response = ok("vehicle-200-full.json");
    response.body = QJsonDocument(root).toJson(QJsonDocument::Compact);

    m_server->enqueue(response);
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("ok"));
    QCOMPARE(m_model->state("batteryLevel").toInt(), 58);
    QCOMPARE(m_model->state("plugState").toString(), QStringLiteral("connected"));
    QCOMPARE(countLog("someFutureMember"), 0);
    QCOMPARE(countLog("treated as unknown"), 0);
    // Unread members are not logged either.
    for (const char *unread : {"READY_FOR_CHARGING", "LOCKED", "286500", "DEACTIVATED", "target", "range"})
        QVERIFY2(countLog(unread) == 0, unread);
}

void TestSkoda::missingChargingPartKeepsStates()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    const QVariantMap before = m_model->states();
    QCOMPARE(countLog("API errors list"), 0);

    advanceToNext();
    m_server->enqueue(ok("vehicle-200-no-charging.json"));
    QVERIFY(cycle().isOk());
    for (const char *name : {"batteryLevel", "socKnown", "batteryCritical", "socTimestamp", "socTimestampSource", "plugState"})
        QVERIFY2(m_model->state(name) == before.value(name), name);
    QCOMPARE(m_model->state("connected").toBool(), true);
    QVERIFY(m_model->state("apiStatusMessage").toString().contains("CHARGING_UNAVAILABLE"));
    QCOMPARE(countLog("API errors list: CHARGING_UNAVAILABLE"), 1);
    QCOMPARE(countLog("Skoda API errors list"), 1); // logged in the plugin's category

    // Same list again: no new line (SK-403).
    advanceToNext();
    m_server->enqueue(ok("vehicle-200-no-charging.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(countLog("API errors list"), 1);

    // The list empties: one line.
    advanceToNext();
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(countLog("API errors list is now empty"), 1);
}

void TestSkoda::missingSocAndPlugKeepLastValues()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    const qint64 socTimestamp = m_model->state("socTimestamp").toLongLong();

    advanceToNext();
    m_server->enqueue(ok("vehicle-200-no-plug-no-soc.json"));
    QVERIFY(cycle().isOk());
    // SK-307: last value kept, timestamp frozen, never 0.
    QCOMPARE(m_model->state("batteryLevel").toInt(), 58);
    QCOMPARE(m_model->state("socTimestamp").toLongLong(), socTimestamp);
    QCOMPARE(m_model->state("socTimestampSource").toString(), QStringLiteral("vehicle"));
    QCOMPARE(m_model->state("plugState").toString(), QStringLiteral("connected"));
}

void TestSkoda::unknownEnumValueIsUnknownAndLoggedOnce()
{
    m_server->enqueue(ok("vehicle-200-unknown-enum.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("plugState").toString(), QStringLiteral("unknown"));
    QCOMPARE(m_model->state("batteryLevel").toInt(), 58);
    QCOMPARE(countLog("Unknown value \"SOME_NEW_PLUG_STATE\" for plugConnectionState"), 1);
    QCOMPARE(countLog("Unknown value"), 1);
    // charging.status.state is not read any more (SK-603): its value is neither
    // published nor logged.
    QCOMPARE(countLog("SOME_NEW_STATE"), 0);

    advance(SkodaPoller::pluggedIntervalSeconds);
    m_server->enqueue(ok("vehicle-200-unknown-enum.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(countLog("Unknown value"), 1);
    // An unknown plug state is polled like a plugged-in vehicle (SK-200): 30 min.
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::pluggedIntervalSeconds));
}

void TestSkoda::receptionTimestampWithoutVehicleTimestamp()
{
    m_server->enqueue(ok("vehicle-200-no-timestamp.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("batteryLevel").toInt(), 47);
    QCOMPARE(m_model->state("socTimestampSource").toString(), QStringLiteral("reception"));
    QCOMPARE(m_model->state("socTimestamp").toLongLong(), m_now.toSecsSinceEpoch());
    QCOMPARE(m_model->state("plugState").toString(), QStringLiteral("disconnected"));
    // Absent fields stay unknown, never 0.
    QCOMPARE(m_model->state("rateLimitRemaining").toInt(), 17);
}

void TestSkoda::sameVehicleTimestampDoesNotAdvance()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    const qint64 first = m_model->state("socTimestamp").toLongLong();
    advance(3600);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    // SK-205: the API does not wake the car; an old reading keeps its old time.
    QCOMPARE(m_model->state("socTimestamp").toLongLong(), first);
}

void TestSkoda::socUnknownUntilFirstReading()
{
    // Before any reading: 0 is a placeholder, flagged as such.
    QCOMPARE(m_model->state("socKnown").toBool(), false);
    QCOMPARE(m_model->state("batteryLevel").toInt(), 0);
    QCOMPARE(m_model->state("socTimestamp").toInt(), 0);
    QCOMPARE(m_model->state("socTimestampSource").toString(), QStringLiteral("unknown"));

    // A 200 without SoC, then a failure: still unknown, never critical.
    m_server->enqueue(ok("vehicle-200-no-plug-no-soc.json"));
    QVERIFY(cycle().isOk());
    advanceToNext();
    m_server->enqueue(problem(503, "problem-503.json"));
    QCOMPARE(cycle().httpStatus, 503);
    QCOMPARE(m_model->state("socKnown").toBool(), false);
    QCOMPARE(m_model->state("batteryLevel").toInt(), 0);
    QCOMPARE(m_model->state("batteryCritical").toBool(), false);
    QCOMPARE(m_model->state("socTimestampSource").toString(), QStringLiteral("unknown"));

    // First SoC read: known from then on, even when a later answer omits it.
    advanceToNext();
    QSignalSpy changes(m_model, &SkodaStateModel::stateValueChanged);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("socKnown").toBool(), true);
    bool emitted = false;
    for (const QList<QVariant> &change : std::as_const(changes))
        emitted |= change.at(0).toString() == QLatin1String("socKnown") && change.at(1).toBool();
    QVERIFY(emitted);

    advanceToNext();
    m_server->enqueue(ok("vehicle-200-no-plug-no-soc.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("socKnown").toBool(), true);
    QCOMPARE(m_model->state("batteryLevel").toInt(), 58);
}

void TestSkoda::socKnownKeptAcrossRestart()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    const QVariantMap cached = m_model->states();

    // Restart: the plugin seeds the new model with the thing's cached states.
    destroySession();
    createSession();
    QCOMPARE(m_model->state("socKnown").toBool(), false);
    for (auto it = cached.constBegin(); it != cached.constEnd(); ++it)
        m_model->seedState(it.key(), it.value());
    QCOMPARE(m_model->state("socKnown").toBool(), true);
    QCOMPARE(m_model->state("batteryLevel").toInt(), 58);

    // A first answer without SoC after the restart does not forget it.
    m_server->enqueue(ok("vehicle-200-no-plug-no-soc.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("socKnown").toBool(), true);
    QCOMPARE(m_model->state("batteryLevel").toInt(), 58);
}

void TestSkoda::batteryCriticalBelow30_data()
{
    QTest::addColumn<int>("soc");
    QTest::addColumn<bool>("critical");
    QTest::newRow("0 %") << 0 << true;
    QTest::newRow("10 %") << 10 << true;
    QTest::newRow("29 %") << 29 << true;
    QTest::newRow("30 %") << 30 << false;
    QTest::newRow("58 %") << 58 << false;
}

void TestSkoda::batteryCriticalBelow30()
{
    QFETCH(int, soc);
    QFETCH(bool, critical);
    QCOMPARE(SkodaStateModel::batteryCriticalBelowPercent, 30);
    m_server->enqueue(okWithSoc(soc));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("batteryLevel").toInt(), soc);
    QCOMPARE(m_model->state("batteryCritical").toBool(), critical);
}

void TestSkoda::keyExpiringSoon_data()
{
    QTest::addColumn<int>("days");
    QTest::addColumn<bool>("soon");
    QTest::newRow("10 days") << 10 << true;
    QTest::newRow("13 days") << 13 << true;
    QTest::newRow("14 days") << 14 << false;
    QTest::newRow("20 days") << 20 << false;
}

void TestSkoda::keyExpiringSoon()
{
    QFETCH(int, days);
    QFETCH(bool, soon);
    m_server->enqueue(ok("vehicle-200-full.json", okHeaders(17, 1800, days)));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("apiKeyExpiringSoon").toBool(), soon);
    QCOMPARE(m_model->state("apiKeyExpiresAt").toLongLong(), s_start.addDays(days).toSecsSinceEpoch());
    QCOMPARE(countLog(QStringLiteral("expires in %1 days").arg(days)), 1);
}

// ---------------------------------------------------------------------------
// SK-2xx

void TestSkoda::adaptiveInterval()
{
    QCOMPARE(SkodaPoller::pluggedIntervalSeconds, 30 * 60);
    QCOMPARE(SkodaPoller::minimumGapSeconds, 5 * 60);

    m_server->enqueue(ok("vehicle-200-full.json")); // CONNECTED
    QVERIFY(cycle().isOk());
    QCOMPARE(waitSeconds(), qint64(1800));

    advance(1800);
    m_server->enqueue(ok("vehicle-200-no-plug-no-soc.json")); // plug absent: last known (plugged)
    QVERIFY(cycle().isOk());
    QCOMPARE(waitSeconds(), qint64(1800));

    // Not before the interval.
    advance(1799);
    QVERIFY(!m_poller->pollNow());
    QCOMPARE(m_server->requestCount(), 2);

    // A value the contract does not list: unknown, polled like plugged in.
    advance(1);
    m_server->enqueue(ok("vehicle-200-unknown-enum.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(waitSeconds(), qint64(1800));
    QVERIFY(m_poller->scheduleReason().contains("plug state unknown, 30 min"));

    // Unplugged: no request planned (SK-200, lot car-7).
    advance(1800);
    m_server->enqueue(ok("vehicle-200-no-timestamp.json")); // DISCONNECTED
    QVERIFY(cycle().isOk());
    QVERIFY(!m_poller->hasPlannedRequest());
    QVERIFY(!m_poller->notBefore().isValid());
    QVERIFY(m_poller->scheduleReason().contains("unplugged, no periodic reading"));
    advance(7 * 24 * 3600);
    QVERIFY(!m_poller->pollNow());
    QCOMPARE(m_server->requestCount(), 4);
}
void TestSkoda::unpluggedNoRequestFor24Hours()
{
    // Started like in the plugin, with the real timer.
    m_server->enqueue(ok("vehicle-200-no-timestamp.json")); // DISCONNECTED
    QSignalSpy spy(m_poller, &SkodaPoller::responseReceived);
    m_poller->start();
    QVERIFY(spy.wait(5000));
    QCOMPARE(m_server->requestCount(), 1);
    QVERIFY(!m_poller->hasPlannedRequest());
    QVERIFY(!m_poller->timerActive());
    QCOMPARE(countLog("No request planned - \"vehicle unplugged"), 1);

    // 24 h in 5-min steps: no request.
    for (int step = 0; step < 24 * 12; ++step) {
        advance(300);
        QVERIFY(!m_poller->pollNow());
    }
    QTest::qWait(1200);
    QCOMPARE(m_server->requestCount(), 1);
    QVERIFY(!m_poller->timerActive());

    // Even with a low quota in the last answer: the pause plans nothing.
    destroySession();
    createSession();
    m_server->enqueue(ok("vehicle-200-no-timestamp.json", okHeaders(2, 1200)));
    QVERIFY(cycle().isOk());
    QVERIFY(!m_poller->hasPlannedRequest());
    m_poller->stop();
}

void TestSkoda::unpluggedFailureIsRetried()
{
    // Decision of 2026-11-02 (lot car-8): kept on purpose. Without the retry,
    // an API failure when the vehicle is unplugged would leave plugState
    // "connected" indefinitely.
    m_server->enqueue(ok("vehicle-200-full.json")); // CONNECTED
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("plugState").toString(), QStringLiteral("connected"));

    // The vehicle is unplugged; the next reading fails (5xx), then gets a 429.
    advance(SkodaPoller::pluggedIntervalSeconds);
    m_server->enqueue(problem(503, "problem-503.json"));
    QCOMPARE(cycle().httpStatus, 503);
    QVERIFY(m_poller->hasPlannedRequest());
    QCOMPARE(waitSeconds(), qint64(300));
    advance(300);
    m_server->enqueue(problem(429, "problem-429.json", {{"Retry-After", "600"}}));
    QCOMPARE(cycle().httpStatus, 429);
    QVERIFY(m_poller->hasPlannedRequest());
    QCOMPARE(waitSeconds(), qint64(600));

    // The retry that succeeds learns the plug state, then reading stops.
    advance(600);
    m_server->enqueue(ok("vehicle-200-no-timestamp.json")); // DISCONNECTED
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("plugState").toString(), QStringLiteral("disconnected"));
    QVERIFY(!m_poller->hasPlannedRequest());

    // A reading asked while unplugged (refresh) that fails is retried too.
    advance(300);
    m_server->enqueue(problem(503, "problem-503.json"));
    QCOMPARE(refreshNow(), SkodaPoller::RefreshResult::Sent);
    QVERIFY(m_poller->hasPlannedRequest());
    QCOMPARE(waitSeconds(), qint64(300));
    QCOMPARE(m_server->requestCount(), 5);
}
void TestSkoda::refreshRefusedWithinGapAcceptedAfter()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    const QDateTime first = m_now;

    // 1 min later: refused, no request, the time of the next possible reading.
    advance(60);
    QString refusal;
    QCOMPARE(refreshNow(&refusal), SkodaPoller::RefreshResult::Refused);
    QVERIFY2(refusal.contains("moins de 5 min depuis la dernière requête"), qPrintable(refusal));
    QVERIFY2(refusal.contains("Prochaine lecture possible à " + localHHmm(first.addSecs(300))), qPrintable(refusal));
    QTest::qWait(50);
    QCOMPARE(m_server->requestCount(), 1);

    // 5 min after the first request: accepted.
    advance(240);
    m_server->enqueue(okWithSoc(61));
    QCOMPARE(refreshNow(), SkodaPoller::RefreshResult::Sent);
    QCOMPARE(m_server->requestCount(), 2);
    QCOMPARE(m_model->state("batteryLevel").toInt(), 61);
    // The plan restarts from that reading.
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::pluggedIntervalSeconds));
}

void TestSkoda::refreshIgnoresThePeriodicPlan()
{
    // Unplugged: no plan at all, a refresh still reads.
    m_server->enqueue(ok("vehicle-200-no-timestamp.json")); // DISCONNECTED
    QVERIFY(cycle().isOk());
    QVERIFY(!m_poller->hasPlannedRequest());
    advance(300);
    m_server->enqueue(ok("vehicle-200-full.json")); // now plugged in
    QCOMPARE(refreshNow(), SkodaPoller::RefreshResult::Sent);
    QVERIFY(m_poller->hasPlannedRequest());
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::pluggedIntervalSeconds));

    // Plugged in, 10 min into a 30-min interval: a refresh reads.
    advance(600);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QCOMPARE(refreshNow(), SkodaPoller::RefreshResult::Sent);
    QCOMPARE(m_server->requestCount(), 3);
}

void TestSkoda::refreshRefusedDuringBackoff()
{
    // Two failures: back-off 10 min after the second.
    m_server->enqueue(problem(503, "problem-503.json"));
    QCOMPARE(cycle().httpStatus, 503);
    advance(300);
    m_server->enqueue(problem(503, "problem-503.json"));
    QCOMPARE(cycle().httpStatus, 503);
    const QDateTime retry = m_now.addSecs(600);

    advance(300); // past the 5-min gap, inside the back-off
    QString refusal;
    QCOMPARE(refreshNow(&refusal), SkodaPoller::RefreshResult::Refused);
    QVERIFY2(refusal.contains("vient d'échouer"), qPrintable(refusal));
    QVERIFY2(refusal.contains("Prochaine lecture possible à " + localHHmm(retry)), qPrintable(refusal));
    QCOMPARE(m_server->requestCount(), 2);

    advance(300);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QCOMPARE(refreshNow(), SkodaPoller::RefreshResult::Sent);
}

void TestSkoda::refreshRefusedDuring429()
{
    m_server->enqueue(problem(429, "problem-429.json", {{"Retry-After", "900"}}));
    QCOMPARE(cycle().httpStatus, 429);
    const QDateTime retry = m_now.addSecs(900);

    advance(600);
    QString refusal;
    QCOMPARE(refreshNow(&refusal), SkodaPoller::RefreshResult::Refused);
    QVERIFY2(refusal.contains("HTTP 429"), qPrintable(refusal));
    QVERIFY2(refusal.contains("Prochaine lecture possible à " + localHHmm(retry)), qPrintable(refusal));
    QCOMPARE(m_server->requestCount(), 1);
}

void TestSkoda::refreshRefusedDuringQuotaPause()
{
    // Unplugged with 2 requests left, reset in 40 min: no plan, refresh refused.
    m_server->enqueue(ok("vehicle-200-no-timestamp.json", okHeaders(2, 2400)));
    QVERIFY(cycle().isOk());
    QVERIFY(!m_poller->hasPlannedRequest());
    advance(600);
    QString refusal;
    QCOMPARE(refreshNow(&refusal), SkodaPoller::RefreshResult::Refused);
    QVERIFY2(refusal.contains("réserve du quota"), qPrintable(refusal));
    QCOMPARE(m_server->requestCount(), 1);
    advance(1800);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QCOMPARE(refreshNow(), SkodaPoller::RefreshResult::Sent);
}

void TestSkoda::refreshRefusedWhileHalted()
{
    m_server->enqueue(problem(401, "problem-401-expired.json"));
    QCOMPARE(cycle().httpStatus, 401);
    advance(3600);
    QString refusal;
    QCOMPARE(refreshNow(&refusal), SkodaPoller::RefreshResult::Refused);
    QVERIFY2(refusal.contains("nouvelle clé API"), qPrintable(refusal));
    QCOMPARE(m_server->requestCount(), 1);
}

void TestSkoda::refreshJoinsARunningRequest()
{
    QSignalSpy applied(m_model, &SkodaStateModel::responseApplied);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(m_poller->pollNow()); // request on its way
    QCOMPARE(m_poller->refresh(nullptr), SkodaPoller::RefreshResult::Busy);
    QVERIFY(applied.wait(5000));
    QCOMPARE(m_server->requestCount(), 1);
}

void TestSkoda::statesAreSetBeforeTheReadingEnds()
{
    // The plugin finishes the action on responseApplied: by then every state is set.
    QVariantMap seen;
    connect(m_model, &SkodaStateModel::responseApplied, this, [this, &seen]() {
        seen = m_model->states();
    });
    m_server->enqueue(ok("vehicle-200-full.json"));
    QCOMPARE(refreshNow(), SkodaPoller::RefreshResult::Sent);
    QCOMPARE(seen.value("batteryLevel").toInt(), 58);
    QCOMPARE(seen.value("socKnown").toBool(), true);
    QCOMPARE(seen.value("plugState").toString(), QStringLiteral("connected"));
    QCOMPARE(seen.value("apiStatus").toString(), QStringLiteral("ok"));
    QCOMPARE(seen.value("rateLimitRemaining").toInt(), 17);
}

void TestSkoda::pairingAnswerIsTheFirstReading()
{
    // confirmPairing: the validation request, with its own client.
    QNetworkAccessManager *network = m_network;
    SkodaApiClient validation([network](const QNetworkRequest &request) { return network->get(request); },
                              m_server->baseUrl(), kVin, kApiKey);
    validation.setClock([this]() { return m_now; });
    QSignalSpy fetched(&validation, &SkodaApiClient::vehicleFetched);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(validation.fetchVehicle());
    QVERIFY(fetched.wait(5000));
    const SkodaResponse answer = fetched.first().first().value<SkodaResponse>();
    QVERIFY(SkodaPoller::usableAsFirstReading(answer));

    // setupThing a few seconds later, then postSetupThing: as in the plugin.
    advance(3);
    m_poller->setLastRequestAt(answer.receivedAt);
    m_poller->acceptResponse(answer);
    m_poller->start();
    QTest::qWait(100);
    // 1 request in all, and the states come from the validation answer.
    QCOMPARE(m_server->requestCount(), 1);
    QCOMPARE(m_model->state("batteryLevel").toInt(), 58);
    QCOMPARE(m_model->state("socKnown").toBool(), true);
    QCOMPARE(m_model->state("plugState").toString(), QStringLiteral("connected"));
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("ok"));
    QCOMPARE(m_poller->notBefore(), m_now.addSecs(SkodaPoller::pluggedIntervalSeconds));
    QCOMPARE(countLog("Using the pairing validation answer as the first reading"), 1);
    m_poller->stop();
}

void TestSkoda::pairingAnswerWithoutChargingIsNotUsed()
{
    SkodaResponse answer;
    answer.kind = SkodaResponse::KindOk;
    answer.httpStatus = 200;
    QVERIFY(!SkodaPoller::usableAsFirstReading(answer)); // 200 without the charging part
    answer.charging.present = true;
    QVERIFY(SkodaPoller::usableAsFirstReading(answer));
    answer.kind = SkodaResponse::KindHttpError;
    answer.httpStatus = 503;
    QVERIFY(!SkodaPoller::usableAsFirstReading(answer));

    // Not used: the first reading follows 5 min after the validation request.
    m_poller->setLastRequestAt(m_now);
    advance(3);
    m_server->enqueue(ok("vehicle-200-full.json"));
    m_poller->start();
    QTest::qWait(100);
    QCOMPARE(m_server->requestCount(), 0);
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::minimumGapSeconds - 3));
    m_poller->stop();
}

void TestSkoda::quotaReservePauses()
{
    // The pause only lengthens the wait: RateLimit-Reset 3000 s > 30 min.
    m_server->enqueue(ok("vehicle-200-full.json", okHeaders(3, 3000)));
    QVERIFY(cycle().isOk());
    QCOMPARE(waitSeconds(), qint64(3000));
    QVERIFY(m_poller->scheduleReason().contains("RateLimit-Remaining 3"));

    advance(2999);
    QVERIFY(!m_poller->pollNow());
    QCOMPARE(m_server->requestCount(), 1);
    advance(1);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_server->requestCount(), 2);
}

void TestSkoda::quotaAtReserveDoesNotPause()
{
    m_server->enqueue(ok("vehicle-200-full.json", okHeaders(4, 1200)));
    QVERIFY(cycle().isOk());
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::pluggedIntervalSeconds));

    // Below the reserve but a reset shorter than the interval: the interval wins.
    advance(SkodaPoller::pluggedIntervalSeconds);
    m_server->enqueue(ok("vehicle-200-full.json", okHeaders(3, 1200)));
    QVERIFY(cycle().isOk());
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::pluggedIntervalSeconds));
}

void TestSkoda::rateLimited429WaitsRetryAfter()
{
    const QList<QPair<QByteArray, QByteArray>> headers = {
        {"Retry-After", "600"}, {"RateLimit-Limit", "20"}, {"RateLimit-Remaining", "0"}, {"RateLimit-Reset", "600"}};
    m_server->enqueue(problem(429, "problem-429.json", headers));
    const SkodaResponse response = cycle();
    QCOMPARE(response.httpStatus, 429);
    QCOMPARE(response.problemType, QStringLiteral("rate-limit-exceeded"));
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("rateLimited"));
    QVERIFY(m_model->state("apiStatusMessage").toString().contains("10 min"));
    QCOMPARE(waitSeconds(), qint64(600));

    // No request for 600 s.
    for (int elapsed : {60, 300, 599}) {
        m_now = s_start.addSecs(elapsed);
        QVERIFY(!m_poller->pollNow());
    }
    QTest::qWait(50);
    QCOMPARE(m_server->requestCount(), 1);

    // Second 429 of the same episode: logged once only (SK-202).
    m_now = s_start.addSecs(600);
    m_server->enqueue(problem(429, "problem-429.json", headers));
    QCOMPARE(cycle().httpStatus, 429);
    QCOMPARE(m_server->requestCount(), 2);
    QCOMPARE(countLog("Rate limit episode started"), 1);
    QCOMPARE(countLog("Cycle failed: HTTP 429"), 2); // but every cycle is logged (SK-400)

    // The episode ends with a success.
    advance(600);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(countLog("Rate limit episode over"), 1);
}

void TestSkoda::vehicleNotAccepting429()
{
    m_server->enqueue(problem(429, "problem-429-vehicle.json", {{"Retry-After", "60"}}));
    QCOMPARE(cycle().httpStatus, 429);
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("vehicleNotAccepting"));
    // Never less than 5 minutes between two requests (SK-200).
    QCOMPARE(waitSeconds(), qint64(300));
}

void TestSkoda::serverErrorBackoff()
{
    const QList<qint64> expected = {300, 600, 1200, 1800, 1800};
    for (qint64 wait : expected) {
        m_server->enqueue(problem(503, "problem-503.json", {{"RateLimit-Remaining", "15"}, {"RateLimit-Reset", "3000"}}));
        const SkodaResponse response = cycle();
        QCOMPARE(response.httpStatus, 503);
        QCOMPARE(waitSeconds(), wait);
        // No immediate retry: 5xx count against the quota.
        QVERIFY(!m_poller->pollNow());
        advance(static_cast<int>(wait));
    }
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("serviceUnavailable"));
    QCOMPARE(m_model->state("connected").toBool(), false);

    // A success resets the back-off.
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::pluggedIntervalSeconds));
    QCOMPARE(m_model->state("connected").toBool(), true);
    QCOMPARE(m_server->requestCount(), 6);
}

void TestSkoda::serverErrorRetryAfterLongerThanBackoff()
{
    m_server->enqueue(problem(503, "problem-503.json", {{"Retry-After", "900"}}));
    QCOMPARE(cycle().httpStatus, 503);
    QCOMPARE(waitSeconds(), qint64(900));
}

void TestSkoda::networkErrorBacksOff()
{
    FakeResponse drop;
    drop.dropConnection = true;
    m_server->enqueue(drop);
    const SkodaResponse response = cycle();
    QCOMPARE(response.kind, SkodaResponse::KindNetworkError);
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("serviceUnavailable"));
    QCOMPARE(m_model->state("connected").toBool(), false);
    QCOMPARE(waitSeconds(), qint64(300));
}

void TestSkoda::gatewayTimeoutIsVehicleUnavailable()
{
    m_server->enqueue(problem(504, "problem-504.json"));
    QCOMPARE(cycle().httpStatus, 504);
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("vehicleUnavailable"));
    QVERIFY(m_model->state("apiStatusMessage").toString().contains("véhicule"));
    QCOMPARE(waitSeconds(), qint64(300));
}

void TestSkoda::unreadable200BacksOff()
{
    FakeResponse response = ok("vehicle-200-full.json");
    response.body = "not json";
    m_server->enqueue(response);
    const SkodaResponse answer = cycle();
    QCOMPARE(answer.httpStatus, 200);
    QVERIFY(!answer.isOk());
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("error"));
    QCOMPARE(m_model->state("batteryLevel").toInt(), 0);
    QCOMPARE(m_model->state("socTimestampSource").toString(), QStringLiteral("unknown"));
    QCOMPARE(waitSeconds(), qint64(300));
}

void TestSkoda::startPollsImmediately()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    QSignalSpy spy(m_poller, &SkodaPoller::responseReceived);
    m_poller->start();
    QVERIFY(spy.wait(5000));
    QCOMPARE(m_server->requestCount(), 1);
    // The next request waits for the interval, not for the event loop.
    QTest::qWait(200);
    QCOMPARE(m_server->requestCount(), 1);
    m_poller->stop();
}

void TestSkoda::restartDoesNotBurst()
{
    // Before the restart: one request, its time kept by the plugin.
    QSignalSpy sent(m_poller, &SkodaPoller::requestSent);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(sent.count(), 1);
    const QDateTime lastRequest = sent.first().first().toDateTime();
    QCOMPARE(lastRequest, m_now);

    // nymead restarts 2 min later (or the vehicle is reconfigured): new session,
    // seeded with the stored time.
    destroySession();
    createSession();
    m_poller->setLastRequestAt(lastRequest);
    advance(120);
    m_server->enqueue(ok("vehicle-200-full.json"));
    m_poller->start();
    QTest::qWait(100);
    QCOMPARE(m_server->requestCount(), 1);
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::minimumGapSeconds - 120));
    QVERIFY(!m_poller->pollNow());

    // 5 min after the last request: allowed.
    advance(SkodaPoller::minimumGapSeconds - 120);
    QVERIFY(cycle().isOk());
    QCOMPARE(m_server->requestCount(), 2);
    m_poller->stop();
}

void TestSkoda::intervalParamsMatchTheBounds()
{
    QFile file(QStringLiteral(SOURCE_ROOT "/skoda/integrationpluginskoda.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject thingClass = QJsonDocument::fromJson(file.readAll()).object()
                                       .value("vendors").toArray().first().toObject()
                                       .value("thingClasses").toArray().first().toObject();
    QHash<QString, QJsonObject> params;
    for (const QJsonValue &param : thingClass.value("paramTypes").toArray())
        params.insert(param.toObject().value("name").toString(), param.toObject());
    QHash<QString, QJsonObject> settings;
    for (const QJsonValue &setting : thingClass.value("settingsTypes").toArray())
        settings.insert(setting.toObject().value("name").toString(), setting.toObject());

    // A setting, not a param (lot car-7): changed at run time.
    QVERIFY(!params.contains("pluggedIntervalMinutes"));
    const QJsonObject plugged = settings.value("pluggedIntervalMinutes");
    QCOMPARE(plugged.value("type").toString(), QStringLiteral("int"));
    QCOMPARE(plugged.value("minValue").toInt(), SkodaPoller::pluggedIntervalMinMinutes);
    QCOMPARE(plugged.value("maxValue").toInt(), SkodaPoller::pluggedIntervalMaxMinutes);
    QCOMPARE(plugged.value("defaultValue").toInt(), SkodaPoller::pluggedIntervalSeconds / 60);
    // No interval for an unplugged vehicle any more (lot car-7).
    QVERIFY(!params.contains("unpluggedIntervalMinutes"));

    QCOMPARE(SkodaPoller::pluggedIntervalMinMinutes, 5);
    QCOMPARE(SkodaPoller::pluggedIntervalMaxMinutes, 120);
    // The shortest interval is never below the minimum gap.
    QVERIFY(SkodaPoller::pluggedIntervalMinMinutes * 60 >= SkodaPoller::minimumGapSeconds);
}
void TestSkoda::intervalsOutsideBoundsAreRefused_data()
{
    QTest::addColumn<int>("minutes");
    QTest::addColumn<bool>("valid");
    QTest::newRow("default") << 30 << true;
    QTest::newRow("5") << 5 << true;
    QTest::newRow("120") << 120 << true;
    QTest::newRow("4") << 4 << false;
    QTest::newRow("121") << 121 << false;
    QTest::newRow("0") << 0 << false;
    QTest::newRow("-1") << -1 << false;
}
void TestSkoda::intervalsOutsideBoundsAreRefused()
{
    QFETCH(int, minutes);
    QFETCH(bool, valid);
    const QString error = SkodaPoller::intervalError(minutes);
    QCOMPARE(error.isEmpty(), valid);
    if (!valid)
        QVERIFY(error.contains("entre 5 et 120 min"));
    QCOMPARE(m_poller->setPluggedInterval(minutes), valid);
    // A refused value leaves the interval as it was.
    QCOMPARE(m_poller->pluggedInterval(), valid ? minutes * 60 : SkodaPoller::pluggedIntervalSeconds);
}
void TestSkoda::configuredIntervalsAreUsed()
{
    QVERIFY(m_poller->setPluggedInterval(5));
    m_server->enqueue(ok("vehicle-200-full.json")); // CONNECTED
    QVERIFY(cycle().isOk());
    QCOMPARE(waitSeconds(), qint64(5 * 60));
    QVERIFY(m_poller->scheduleReason().contains("plugged in, 5 min"));

    // 120 min: longer than the timer's one-hour cap, which re-arms until then.
    QVERIFY(m_poller->setPluggedInterval(120));
    QCOMPARE(waitSeconds(), qint64(120 * 60));
    advance(119 * 60);
    QVERIFY(!m_poller->pollNow());
    advance(60);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(waitSeconds(), qint64(120 * 60));
    QCOMPARE(m_server->requestCount(), 2);
}
void TestSkoda::intervalChangeMovesThePlanWithoutRequest()
{
    m_server->enqueue(ok("vehicle-200-full.json")); // CONNECTED
    QVERIFY(cycle().isOk());
    const QDateTime read = m_now;
    QCOMPARE(waitSeconds(), qint64(1800));

    // 10 min later the setting goes to 60 min: plan moves to read + 60 min.
    advance(600);
    QVERIFY(m_poller->setPluggedInterval(60));
    QCOMPARE(m_poller->notBefore(), read.addSecs(3600));
    QVERIFY(m_poller->scheduleReason().contains("setting changed"));
    // Then to 5 min: read + 5 min is already past, so the next timer tick reads.
    QVERIFY(m_poller->setPluggedInterval(5));
    QCOMPARE(m_poller->notBefore(), read.addSecs(300));
    QVERIFY(m_poller->notBefore() < m_now);
    QTest::qWait(50);
    QCOMPARE(m_server->requestCount(), 1); // the change itself sends nothing

    // A retry after a failure is not moved by the setting.
    m_server->enqueue(problem(503, "problem-503.json"));
    QCOMPARE(cycle().httpStatus, 503);
    const QDateTime retry = m_poller->notBefore();
    QVERIFY(m_poller->setPluggedInterval(120));
    QCOMPARE(m_poller->notBefore(), retry);
    // Out of bounds: refused, nothing changes.
    QVERIFY(!m_poller->setPluggedInterval(121));
    QCOMPARE(m_poller->pluggedInterval(), 120 * 60);
}

void TestSkoda::intervalSettingResolution_data()
{
    QTest::addColumn<QVariant>("stored");
    QTest::addColumn<QVariant>("current");
    QTest::addColumn<double>("value");
    QTest::addColumn<int>("source");
    using R = SkodaSettings::Resolution;
    QTest::newRow("first setup") << QVariant() << QVariant(30) << 30.0 << int(R::Thing);
    QTest::newRow("first setup, 45") << QVariant() << QVariant(45) << 45.0 << int(R::Thing);
    QTest::newRow("restart") << QVariant(45) << QVariant(45) << 45.0 << int(R::Stored);
    QTest::newRow("after pairing reset") << QVariant(45) << QVariant(30) << 45.0 << int(R::Stored);
    QTest::newRow("stored as text (INI)") << QVariant(QStringLiteral("45")) << QVariant(30) << 45.0 << int(R::Stored);
    QTest::newRow("stored 4") << QVariant(4) << QVariant(30) << 30.0 << int(R::StoredOutOfBounds);
    QTest::newRow("stored 121") << QVariant(121) << QVariant(45) << 30.0 << int(R::StoredOutOfBounds);
    QTest::newRow("stored garbage") << QVariant(QStringLiteral("x")) << QVariant(45) << 30.0 << int(R::StoredOutOfBounds);
    QTest::newRow("thing out of bounds") << QVariant() << QVariant(500) << 30.0 << int(R::ThingOutOfBounds);
    QTest::newRow("thing without setting") << QVariant() << QVariant() << 30.0 << int(R::ThingOutOfBounds);
}

void TestSkoda::intervalSettingResolution()
{
    QFETCH(QVariant, stored);
    QFETCH(QVariant, current);
    QFETCH(double, value);
    QFETCH(int, source);
    const SkodaSettings::Bounds bounds = SkodaSettings::pluggedIntervalMinutes();
    // The same bounds as the poller and the plugin JSON (see intervalParamsMatchTheBounds).
    QCOMPARE(bounds.minimum, double(SkodaPoller::pluggedIntervalMinMinutes));
    QCOMPARE(bounds.maximum, double(SkodaPoller::pluggedIntervalMaxMinutes));
    QCOMPARE(bounds.defaultValue, double(SkodaPoller::pluggedIntervalSeconds / 60));
    const SkodaSettings::Resolution resolution = SkodaSettings::resolve(stored, current, bounds);
    QCOMPARE(resolution.value, value);
    QCOMPARE(int(resolution.source), source);
}

void TestSkoda::intervalSurvivesAReconfigurationThroughPairing()
{
    // The plugin storage is a QSettings INI file. (Within one process Qt caches
    // the file, so a value may come back with its type; after a restart it
    // comes back as text: see the "stored as text (INI)" row above.)
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath("skoda.conf");
    const QString group = QStringLiteral("{c0ffee00-0000-4000-8000-000000000001}");
    const SkodaSettings::Bounds bounds = SkodaSettings::pluggedIntervalMinutes();
    auto stored = [&]() {
        QSettings storage(file, QSettings::IniFormat);
        return storage.value(group + "/pluggedIntervalMinutes");
    };
    auto store = [&](double value) {
        QSettings storage(file, QSettings::IniFormat);
        storage.setValue(group + "/pluggedIntervalMinutes", value);
    };

    // First setup: nothing stored, the thing has the default; kept.
    SkodaSettings::Resolution r = SkodaSettings::resolve(stored(), 30, bounds);
    QCOMPARE(r.value, 30.0);
    store(r.value);
    // The user sets 45 (settingChanged): kept.
    store(45);
    // Reconfiguration through the pairing dialog: nymea puts 30 back.
    r = SkodaSettings::resolve(stored(), 30, bounds);
    QCOMPARE(r.value, 45.0);
    QCOMPARE(r.source, SkodaSettings::Resolution::Stored);
    // A broken value in the storage: default, reported as such.
    store(999);
    r = SkodaSettings::resolve(stored(), 45, bounds);
    QCOMPARE(r.value, 30.0);
    QCOMPARE(r.source, SkodaSettings::Resolution::StoredOutOfBounds);
    QVERIFY(SkodaSettings::sourceName(r.source).contains("out of bounds"));
}

void TestSkoda::capacitySettingResolution_data()
{
    QTest::addColumn<QVariant>("stored");
    QTest::addColumn<QVariant>("current");
    QTest::addColumn<QVariant>("legacy");
    QTest::addColumn<double>("value");
    QTest::addColumn<int>("source");
    using R = SkodaSettings::Resolution;
    // Box in 0.1.2 with batteryCapacity = 52: first setupThing in 0.1.5.
    QTest::newRow("migration 52") << QVariant() << QVariant(0.0) << QVariant(52.0) << 52.0 << int(R::Legacy);
    QTest::newRow("migration 52 as text") << QVariant() << QVariant(0.0) << QVariant(QStringLiteral("52")) << 52.0 << int(R::Legacy);
    QTest::newRow("migration 0 (not entered)") << QVariant() << QVariant(0.0) << QVariant(0.0) << 0.0 << int(R::Legacy);
    QTest::newRow("migration 230 (old max 250)") << QVariant() << QVariant(0.0) << QVariant(230.0) << 0.0 << int(R::LegacyOutOfBounds);
    QTest::newRow("migration 5") << QVariant() << QVariant(0.0) << QVariant(5.0) << 5.0 << int(R::Legacy);
    // Once stored, the legacy param no longer counts.
    QTest::newRow("stored wins over legacy") << QVariant(77.0) << QVariant(77.0) << QVariant(52.0) << 77.0 << int(R::Stored);
    QTest::newRow("after pairing reset") << QVariant(52.0) << QVariant(0.0) << QVariant() << 52.0 << int(R::Stored);
    QTest::newRow("stored 0") << QVariant(0.0) << QVariant(0.0) << QVariant() << 0.0 << int(R::Stored);
    QTest::newRow("stored 9.5") << QVariant(9.5) << QVariant(0.0) << QVariant() << 9.5 << int(R::Stored);
    QTest::newRow("stored -1") << QVariant(-1.0) << QVariant(0.0) << QVariant() << 0.0 << int(R::StoredOutOfBounds);
    QTest::newRow("stored 201") << QVariant(201.0) << QVariant(0.0) << QVariant() << 0.0 << int(R::StoredOutOfBounds);
    // New thing in 0.1.5: no legacy param, nothing stored.
    QTest::newRow("new thing") << QVariant() << QVariant(0.0) << QVariant() << 0.0 << int(R::Thing);
    QTest::newRow("new thing, 82") << QVariant() << QVariant(82.0) << QVariant() << 82.0 << int(R::Thing);
}

void TestSkoda::capacitySettingResolution()
{
    QFETCH(QVariant, stored);
    QFETCH(QVariant, current);
    QFETCH(QVariant, legacy);
    QFETCH(double, value);
    QFETCH(int, source);
    const SkodaSettings::Bounds bounds = SkodaSettings::capacityKiloWattHours();
    // Lot car-9: 0-200 kWh without a gap, the bounds of the plugin JSON.
    QCOMPARE(bounds.minimum, 0.0);
    QCOMPARE(bounds.maximum, 200.0);
    QCOMPARE(bounds.defaultValue, 0.0);
    const SkodaSettings::Resolution resolution = SkodaSettings::resolve(stored, current, bounds, legacy);
    QCOMPARE(resolution.value, value);
    QCOMPARE(int(resolution.source), source);
}

void TestSkoda::capacityStateFollowsTheSetting()
{
    QCOMPARE(m_model->state("capacity").toDouble(), 0.0);
    QSignalSpy changes(m_model, &SkodaStateModel::stateValueChanged);
    m_model->setCapacity(52);
    QCOMPARE(m_model->state("capacity").toDouble(), 52.0);
    QCOMPARE(changes.count(), 1);
    m_model->setCapacity(52);
    QCOMPARE(changes.count(), 1); // unchanged: no signal
    m_model->setCapacity(0);
    QCOMPARE(m_model->state("capacity").toDouble(), 0.0);
    // 0-200 without a gap (lot car-9): small values are accepted.
    m_model->setCapacity(5.5);
    QCOMPARE(m_model->state("capacity").toDouble(), 5.5);
    for (double outside : {200.5, -1.0})
        QVERIFY2(!SkodaSettings::inBounds(outside, SkodaSettings::capacityKiloWattHours()), qPrintable(QString::number(outside)));
    for (double accepted : {0.0, 0.5, 9.9, 10.0, 52.0, 200.0})
        QVERIFY(SkodaSettings::inBounds(accepted, SkodaSettings::capacityKiloWattHours()));
    QCOMPARE(m_server->requestCount(), 0); // no request for a setting
}

void TestSkoda::acPhaseCountSettingResolution_data()
{
    QTest::addColumn<QVariant>("stored");
    QTest::addColumn<QVariant>("current");
    QTest::addColumn<double>("value");
    QTest::addColumn<int>("source");
    using R = SkodaSettings::Resolution;
    // First start of 0.1.8: nothing stored, nymea gives the default 3.
    QTest::newRow("first setup") << QVariant() << QVariant(3u) << 3.0 << int(R::Thing);
    QTest::newRow("first setup, 1") << QVariant() << QVariant(1u) << 1.0 << int(R::Thing);
    QTest::newRow("restart") << QVariant(1.0) << QVariant(1u) << 1.0 << int(R::Stored);
    QTest::newRow("after pairing reset") << QVariant(1.0) << QVariant(3u) << 1.0 << int(R::Stored);
    QTest::newRow("after pairing reset, 2") << QVariant(2.0) << QVariant(3u) << 2.0 << int(R::Stored);
    QTest::newRow("stored as text (INI)") << QVariant(QStringLiteral("1")) << QVariant(3u) << 1.0 << int(R::Stored);
    QTest::newRow("stored 0") << QVariant(0.0) << QVariant(1u) << 3.0 << int(R::StoredOutOfBounds);
    QTest::newRow("stored 4") << QVariant(4.0) << QVariant(1u) << 3.0 << int(R::StoredOutOfBounds);
    QTest::newRow("stored 1.5") << QVariant(1.5) << QVariant(1u) << 3.0 << int(R::StoredOutOfBounds);
    QTest::newRow("stored garbage") << QVariant(QStringLiteral("x")) << QVariant(1u) << 3.0 << int(R::StoredOutOfBounds);
    QTest::newRow("thing 0") << QVariant() << QVariant(0u) << 3.0 << int(R::ThingOutOfBounds);
    QTest::newRow("thing without setting") << QVariant() << QVariant() << 3.0 << int(R::ThingOutOfBounds);
}

void TestSkoda::acPhaseCountSettingResolution()
{
    QFETCH(QVariant, stored);
    QFETCH(QVariant, current);
    QFETCH(double, value);
    QFETCH(int, source);
    const SkodaSettings::Bounds bounds = SkodaSettings::acPhaseCount();
    QCOMPARE(bounds.minimum, 1.0);
    QCOMPARE(bounds.maximum, 3.0);
    QCOMPARE(bounds.defaultValue, 3.0);
    QVERIFY(bounds.wholeNumber);
    const SkodaSettings::Resolution resolution = SkodaSettings::resolve(stored, current, bounds);
    QCOMPARE(resolution.value, value);
    QCOMPARE(int(resolution.source), source);
}

void TestSkoda::phaseStatesFollowTheSetting()
{
    // Default: three phases under both names, before any setting is applied.
    QCOMPARE(m_model->state("acPhaseCount"), QVariant(3u));
    QCOMPARE(m_model->state("phaseCount"), QVariant(3u));
    QSignalSpy changes(m_model, &SkodaStateModel::stateValueChanged);
    QVERIFY(m_model->setAcPhaseCount(1));
    QCOMPARE(m_model->state("acPhaseCount"), QVariant(1u));
    QCOMPARE(m_model->state("phaseCount"), QVariant(1u));
    QCOMPARE(changes.count(), 2);
    QCOMPARE(changes.at(0).at(0).toString(), QStringLiteral("acPhaseCount"));
    QCOMPARE(changes.at(1).at(0).toString(), QStringLiteral("phaseCount"));
    QCOMPARE(changes.at(1).at(1).typeId(), int(QMetaType::UInt)); // the type of the state
    QVERIFY(m_model->setAcPhaseCount(1));
    QCOMPARE(changes.count(), 2); // unchanged: no signal
    QVERIFY(m_model->setAcPhaseCount(2));
    QCOMPARE(m_model->state("phaseCount"), QVariant(2u));
    QCOMPARE(changes.count(), 4);
    // Outside 1-3: refused, the states keep their value.
    QVERIFY(!m_model->setAcPhaseCount(0));
    QVERIFY(!m_model->setAcPhaseCount(4));
    QCOMPARE(m_model->state("acPhaseCount"), QVariant(2u));
    QCOMPARE(m_model->state("phaseCount"), QVariant(2u));
    QCOMPARE(changes.count(), 4);

    // A reading does not touch them: the API gives no phase count.
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_model->state("acPhaseCount"), QVariant(2u));
    QCOMPARE(m_model->state("phaseCount"), QVariant(2u));
    QCOMPARE(m_server->requestCount(), 1); // the reading only, none for the setting

    // Seeded from nymea's cached states at start-up: no change emitted.
    SkodaStateModel restarted;
    restarted.seedState("acPhaseCount", 1u);
    restarted.seedState("phaseCount", 1u);
    QSignalSpy restartedChanges(&restarted, &SkodaStateModel::stateValueChanged);
    QVERIFY(restarted.setAcPhaseCount(1));
    QCOMPARE(restartedChanges.count(), 0);
}

void TestSkoda::phaseSettingOutsideOneToThreeIsRefused_data()
{
    QTest::addColumn<QVariant>("value");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("1") << QVariant(1u) << true;
    QTest::newRow("2") << QVariant(2u) << true;
    QTest::newRow("3") << QVariant(3u) << true;
    QTest::newRow("0") << QVariant(0u) << false;
    QTest::newRow("4") << QVariant(4u) << false;
    QTest::newRow("-1") << QVariant(-1) << false;
    QTest::newRow("2.5") << QVariant(2.5) << false;
    QTest::newRow("text") << QVariant(QStringLiteral("three")) << false;
    QTest::newRow("invalid") << QVariant() << false;
}

void TestSkoda::phaseSettingOutsideOneToThreeIsRefused()
{
    // nymea refuses a value outside the bounds of the plugin JSON on
    // SetThingSettings; the plugin ignores one too (follow, checkBounds).
    QFETCH(QVariant, value);
    QFETCH(bool, accepted);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings storage(dir.filePath("skoda.conf"), QSettings::IniFormat);
    const QUuid phasesId = QUuid::createUuid();
    FakeThing thing(QUuid(QStringLiteral("{c0ffee00-0000-4000-8000-000000000001}")));
    thing.setSettings({{phasesId, 3u}});
    thing.setSetupStatus(FakeThing::ThingSetupStatusInProgress);
    SkodaStateModel model;
    QObject session;
    model.setAcPhaseCount(static_cast<uint>(SkodaThingSettings::restore(&thing, phasesId, &storage, "acPhaseCount",
                                                                        SkodaSettings::acPhaseCount())));
    SkodaThingSettings::follow(&thing, &session, phasesId, &storage, "acPhaseCount", SkodaSettings::acPhaseCount(), true,
                               [&model](double phases) { model.setAcPhaseCount(static_cast<uint>(phases)); });
    thing.setSetupStatus(FakeThing::ThingSetupStatusComplete);
    QCOMPARE(SkodaThingSettings::load(&storage, thing.id().toString(), "acPhaseCount").toDouble(), 3.0);

    thing.setSettings({{phasesId, value}});
    const double stored = SkodaThingSettings::load(&storage, thing.id().toString(), "acPhaseCount").toDouble();
    if (accepted) {
        QCOMPARE(stored, value.toDouble());
        QCOMPARE(model.state("phaseCount"), QVariant(value.toUInt()));
        QCOMPARE(model.state("acPhaseCount"), QVariant(value.toUInt()));
        QCOMPARE(countLog("outside its bounds"), 0);
    } else {
        QCOMPARE(stored, 3.0);
        QCOMPARE(model.state("phaseCount"), QVariant(3u));
        QCOMPARE(model.state("acPhaseCount"), QVariant(3u));
        QCOMPARE(countLog("outside its bounds, ignored"), 1);
    }
}

void TestSkoda::settingsSurviveTheNymeaPairingOrder()
{
    // Replays what nymea 1.15.2 does around the plugin (lot car-10, defect seen
    // on the box with 0.1.6: interval 45 and capacity 52 back to 30 and 0 after
    // a reconfiguration through the pairing dialog). The plugin part is the
    // one of IntegrationPluginSkoda::setupThing / destroySession.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings storage(dir.filePath("skoda.conf"), QSettings::IniFormat);
    const QUuid intervalId = QUuid::createUuid();
    const QUuid capacityId = QUuid::createUuid();
    const QUuid phasesId = QUuid::createUuid();
    const QVariant legacyCapacity = 52; // former param batteryCapacity of the 0.1.2 thing
    const QList<QPair<QUuid, QVariant>> defaults = {{intervalId, 30}, {capacityId, 0.0}, {phasesId, 3u}};

    struct Applied { double interval = 0; double capacity = 0; double phases = 0; };
    Applied applied;
    QObject *session = nullptr; // receiver of the setting handlers (poller, model)

    auto pluginSetupThing = [&](FakeThing *thing) {
        if (session) { // destroySession
            QObject::disconnect(thing, nullptr, session, nullptr);
            session->deleteLater();
        }
        session = new QObject(this);
        applied.interval = SkodaThingSettings::restore(thing, intervalId, &storage, "pluggedIntervalMinutes",
                                                       SkodaSettings::pluggedIntervalMinutes());
        SkodaThingSettings::follow(thing, session, intervalId, &storage, "pluggedIntervalMinutes",
                                   SkodaSettings::pluggedIntervalMinutes(), true,
                                   [&applied](double minutes) { applied.interval = minutes; });
        applied.capacity = SkodaThingSettings::restore(thing, capacityId, &storage, "capacity",
                                                       SkodaSettings::capacityKiloWattHours(), legacyCapacity);
        SkodaThingSettings::follow(thing, session, capacityId, &storage, "capacity",
                                   SkodaSettings::capacityKiloWattHours(), false,
                                   [&applied](double kiloWattHours) { applied.capacity = kiloWattHours; });
        applied.phases = SkodaThingSettings::restore(thing, phasesId, &storage, "acPhaseCount", SkodaSettings::acPhaseCount());
        SkodaThingSettings::follow(thing, session, phasesId, &storage, "acPhaseCount", SkodaSettings::acPhaseCount(), true,
                                   [&applied](double phases) { applied.phases = phases; });
    };
    auto stored = [&](const QString &key) {
        return SkodaThingSettings::load(&storage, QStringLiteral("{c0ffee00-0000-4000-8000-000000000001}"), key).toDouble();
    };
    // What nymea writes to its config (storeConfiguredThings): the thing's settings.
    QHash<QUuid, QVariant> nymeaConfig;

    FakeThing thing(QUuid(QStringLiteral("{c0ffee00-0000-4000-8000-000000000001}")));

    // Start-up with 0.1.6 on the 0.1.2 thing (loadConfiguredThings): no
    // setting in the config, defaults; then trySetupThing (l. 2477).
    thing.setSettings(defaults);
    thing.setSetupStatus(FakeThing::ThingSetupStatusInProgress);
    pluginSetupThing(&thing);
    thing.setSetupStatus(FakeThing::ThingSetupStatusComplete);
    nymeaConfig = thing.settings();
    QCOMPARE(applied.capacity, 52.0);
    QCOMPARE(stored("capacity"), 52.0);
    QCOMPARE(nymeaConfig.value(capacityId).toDouble(), 52.0);
    QCOMPARE(applied.interval, 30.0);
    QCOMPARE(applied.phases, 3.0);
    QCOMPARE(stored("acPhaseCount"), 3.0);

    // SetThingSettings pluggedIntervalMinutes = 45 (setThingSettings,
    // l. 524-525: setSettings with the new and the previous settings).
    thing.setSettings({{intervalId, 45}, {capacityId, thing.setting(capacityId)}, {phasesId, thing.setting(phasesId)}});
    nymeaConfig = thing.settings();
    QCOMPARE(applied.interval, 45.0);
    QCOMPARE(stored("pluggedIntervalMinutes"), 45.0);
    QCOMPARE(stored("capacity"), 52.0);
    // SetThingSettings acPhaseCount = 1 (a single-phase vehicle).
    thing.setSettings({{intervalId, thing.setting(intervalId)}, {capacityId, thing.setting(capacityId)}, {phasesId, 1u}});
    nymeaConfig = thing.settings();
    QCOMPARE(applied.phases, 1.0);
    QCOMPARE(stored("acPhaseCount"), 1.0);
    QCOMPARE(stored("pluggedIntervalMinutes"), 45.0);

    // Reconfiguration through the pairing dialog (confirmPairing), same Thing:
    thing.setSetupStatus(FakeThing::ThingSetupStatusInProgress); // l. 779
    thing.setSettings(defaults);                                  // l. 784-785, old handlers still connected
    QCOMPARE(stored("pluggedIntervalMinutes"), 45.0);             // nymea's defaults are not stored
    QCOMPARE(stored("capacity"), 52.0);
    QCOMPARE(stored("acPhaseCount"), 1.0);
    pluginSetupThing(&thing);                                     // l. 789
    thing.setSetupStatus(FakeThing::ThingSetupStatusComplete);    // l. 812
    nymeaConfig = thing.settings();                               // l. 821
    QCOMPARE(nymeaConfig.value(intervalId).toDouble(), 45.0);
    QCOMPARE(nymeaConfig.value(capacityId).toDouble(), 52.0);
    QCOMPARE(nymeaConfig.value(phasesId).toDouble(), 1.0);
    QCOMPARE(applied.interval, 45.0);
    QCOMPARE(applied.capacity, 52.0);
    QCOMPARE(applied.phases, 1.0);
    QCOMPARE(countLog("set again from the plugin storage"), 3);

    // The user's changes still count after the reconfiguration.
    thing.setSettings({{intervalId, thing.setting(intervalId)}, {capacityId, 60.0}, {phasesId, 2u}});
    QCOMPARE(applied.capacity, 60.0);
    QCOMPARE(stored("capacity"), 60.0);
    QCOMPARE(applied.phases, 2.0);
    QCOMPARE(stored("acPhaseCount"), 2.0);
    QCOMPARE(stored("pluggedIntervalMinutes"), 45.0);

    // A second reconfiguration through pairing, then a restart with what
    // nymea wrote: nothing changes.
    thing.setSetupStatus(FakeThing::ThingSetupStatusInProgress);
    thing.setSettings(defaults);
    pluginSetupThing(&thing);
    thing.setSetupStatus(FakeThing::ThingSetupStatusComplete);
    nymeaConfig = thing.settings();
    QCOMPARE(nymeaConfig.value(intervalId).toDouble(), 45.0);
    QCOMPARE(nymeaConfig.value(capacityId).toDouble(), 60.0);
    QCOMPARE(nymeaConfig.value(phasesId).toDouble(), 2.0);

    delete session;
    session = nullptr;
    FakeThing restarted(thing.id());
    restarted.setSettings({{intervalId, nymeaConfig.value(intervalId)}, {capacityId, nymeaConfig.value(capacityId)},
                           {phasesId, nymeaConfig.value(phasesId)}});
    restarted.setSetupStatus(FakeThing::ThingSetupStatusInProgress);
    QSignalSpy changes(&restarted, &FakeThing::settingChanged);
    pluginSetupThing(&restarted);
    QCOMPARE(changes.count(), 0);
    QCOMPARE(applied.interval, 45.0);
    QCOMPARE(applied.capacity, 60.0);
    QCOMPARE(applied.phases, 2.0);
    delete session;
}

void TestSkoda::firstReadAtOnceAfterRestartIsAccepted()
{
    // No stored time (first start), then a stored time older than 5 min.
    for (int ago : {-1, SkodaPoller::minimumGapSeconds}) {
        destroySession();
        createSession();
        if (ago >= 0)
            m_poller->setLastRequestAt(m_now.addSecs(-ago));
        m_server->enqueue(ok("vehicle-200-full.json"));
        QSignalSpy spy(m_poller, &SkodaPoller::responseReceived);
        m_poller->start();
        QVERIFY(spy.wait(5000));
        m_poller->stop();
        advance(SkodaPoller::pluggedIntervalSeconds);
    }
    QCOMPARE(m_server->requestCount(), 2);
}

void TestSkoda::newKeyRespectsMinimumGap()
{
    m_server->enqueue(problem(401, "problem-401-expired.json"));
    QCOMPARE(cycle().httpStatus, 401);
    QVERIFY(m_poller->halted());

    // A new key entered 1 min later: resumed, but no request before 5 min.
    advance(60);
    m_client->setApiKey(kNewApiKey);
    m_poller->resume();
    QVERIFY(!m_poller->halted());
    QVERIFY(!m_poller->pollNow());
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::minimumGapSeconds - 60));
    advance(SkodaPoller::minimumGapSeconds - 60);
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    QCOMPARE(m_server->requestCount(), 2);
}

void TestSkoda::lastRequestInTheFutureCountsAsNow()
{
    // Clock set back since the stored time: wait 5 min, not until that time.
    m_poller->setLastRequestAt(m_now.addDays(2));
    QCOMPARE(m_poller->lastRequestAt(), m_now);
    QCOMPARE(waitSeconds(), qint64(SkodaPoller::minimumGapSeconds));
}

// ---------------------------------------------------------------------------
// SK-103, SK-402

void TestSkoda::key401ExpiredHaltsUntilNewKey()
{
    QSignalSpy halted(m_poller, &SkodaPoller::haltedChanged);
    m_server->enqueue(problem(401, "problem-401-expired.json", {{"X-API-Key-Expires-At", "2026-09-01T00:00:00Z"}}));
    const SkodaResponse response = cycle();
    QCOMPARE(response.httpStatus, 401);
    QCOMPARE(response.problemType, QStringLiteral("api-key-expired"));
    QVERIFY(m_poller->halted());
    QCOMPARE(halted.count(), 1);
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("keyExpired"));
    QCOMPARE(m_model->state("connected").toBool(), false);
    QVERIFY(m_model->state("apiStatusMessage").toString().contains("a expiré"));
    QCOMPARE(m_model->state("apiKeyExpiringSoon").toBool(), true);

    // No request at all, however long we wait.
    advance(24 * 3600);
    QVERIFY(!m_poller->pollNow());
    QTest::qWait(50);
    QCOMPARE(m_server->requestCount(), 1);

    // New key: polling resumes.
    m_client->setApiKey(kNewApiKey);
    m_server->enqueue(ok("vehicle-200-full.json"));
    m_poller->resume();
    QVERIFY(!m_poller->halted());
    QVERIFY(cycle().isOk());
    QCOMPARE(m_server->requestCount(), 2);
    QCOMPARE(m_server->requests().last().headers.value("x-api-key"), kNewApiKey.toUtf8());
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("ok"));
}

void TestSkoda::key401InvalidHalts()
{
    m_server->enqueue(problem(401, "problem-401-blank.json"));
    QCOMPARE(cycle().httpStatus, 401);
    QVERIFY(m_poller->halted());
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("keyInvalid"));
}

void TestSkoda::key403NotAuthorizedHalts()
{
    m_server->enqueue(problem(403, "problem-403.json"));
    const SkodaResponse response = cycle();
    QCOMPARE(response.problemType, QStringLiteral("api-key-not-authorized"));
    QVERIFY(m_poller->halted());
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("keyNotAuthorized"));
    QVERIFY(m_model->state("apiStatusMessage").toString().contains("VIN non couvert"));
    advance(3600);
    QVERIFY(!m_poller->pollNow());
    QCOMPARE(m_server->requestCount(), 1);
}

void TestSkoda::vehicle404Halts()
{
    m_server->enqueue(problem(404, "problem-404.json"));
    QCOMPARE(cycle().httpStatus, 404);
    QVERIFY(m_poller->halted());
    QCOMPARE(m_model->state("apiStatus").toString(), QStringLiteral("vehicleNotFound"));
}

// ---------------------------------------------------------------------------
// SK-400, SK-401

void TestSkoda::everyCycleIsLogged()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    advanceToNext();
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    advanceToNext();
    m_server->enqueue(problem(503, "problem-503.json"));
    QCOMPARE(cycle().httpStatus, 503);

    QCOMPARE(countLog("Cycle HTTP 200: states updated"), 1);
    QCOMPARE(countLog("Cycle HTTP 200: nothing changed"), 1);
    QCOMPARE(countLog("Cycle failed: HTTP 503"), 1);
    QCOMPARE(countLog("quota 17/20, reset in 1800 s"), 2);
    QCOMPARE(countLog("key …WXYZ expires in"), 3);
    QCOMPARE(countLog("expires in 120 days"), 1); // then 119 days, 5 and 10 minutes later
    QCOMPARE(countLog("SoC 58 %"), 2);
}

void TestSkoda::apiKeyNeverInLogs()
{
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    advance(300);
    m_server->enqueue(problem(503, "problem-503.json"));
    cycle();
    advance(300);
    m_server->enqueue(problem(429, "problem-429.json", {{"Retry-After", "600"}}));
    cycle();
    advance(600);
    m_server->enqueue(problem(401, "problem-401-expired.json"));
    cycle();
    m_poller->pollNow();

    QVERIFY(!g_log.isEmpty());
    const QString all = g_log.join('\n');
    QVERIFY2(!all.contains(kApiKey), "the API key appears in the log");
    // Not even a long part of it.
    QVERIFY(!all.contains(kApiKey.left(12)));
    QVERIFY(!all.contains(kApiKey.mid(8, 12)));
    QVERIFY(all.contains(QStringLiteral("…WXYZ")));
}

void TestSkoda::vinNeverInLogs()
{
    // 200 (the body carries the VIN), then server errors.
    m_server->enqueue(ok("vehicle-200-full.json"));
    QVERIFY(cycle().isOk());
    advanceToNext();
    m_server->enqueue(problem(503, "problem-503.json"));
    cycle();
    advanceToNext();
    m_server->enqueue(problem(404, "problem-404.json"));
    cycle();

    // Network failure: connection closed without an answer.
    destroySession();
    createSession();
    FakeResponse drop;
    drop.dropConnection = true;
    m_server->enqueue(drop);
    const SkodaResponse dropped = cycle();
    QCOMPARE(dropped.kind, SkodaResponse::KindNetworkError);
    QCOMPARE(dropped.networkError, int(QNetworkReply::RemoteHostClosedError));
    QCOMPARE(dropped.errorString, QStringLiteral("RemoteHostClosedError (2)"));

    // Network failure: nothing listens on the port. A model without the VIN,
    // so the test does not rely on the redaction alone.
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    QUrl closedUrl(QStringLiteral("http://127.0.0.1"));
    closedUrl.setPort(closed.serverPort());
    closed.close();
    QNetworkAccessManager *network = m_network;
    SkodaApiClient refusedClient([network](const QNetworkRequest &request) { return network->get(request); },
                                 closedUrl, kVin, kApiKey);
    SkodaStateModel bareModel;
    connect(&refusedClient, &SkodaApiClient::vehicleFetched, &bareModel, &SkodaStateModel::applyResponse);
    QSignalSpy refusedSpy(&refusedClient, &SkodaApiClient::vehicleFetched);
    QVERIFY(refusedClient.fetchVehicle());
    QVERIFY(refusedSpy.wait(5000));
    const SkodaResponse refused = refusedSpy.first().first().value<SkodaResponse>();
    QCOMPARE(refused.kind, SkodaResponse::KindNetworkError);
    QCOMPARE(refused.networkError, int(QNetworkReply::ConnectionRefusedError));
    QVERIFY(!refused.errorString.contains(kVin));
    QCOMPARE(countLog("Cycle failed: network error (ConnectionRefusedError (1))"), 1);

    // Free text from the API that quotes the VIN (errors list, unknown value).
    QJsonObject root = QJsonDocument::fromJson(fixture("vehicle-200-full.json")).object();
    root.insert("errors", QJsonArray{QJsonObject{{"type", "CHARGING_UNAVAILABLE"},
                                                 {"description", "No charging data for " + kVin}}});
    QJsonObject vehicle = root.value("vehicle").toObject();
    QJsonObject charging = vehicle.value("charging").toObject();
    QJsonObject status = charging.value("status").toObject();
    status.insert("plugConnectionState", "PLUG_" + kVin);
    charging.insert("status", status);
    vehicle.insert("charging", charging);
    root.insert("vehicle", vehicle);
    FakeResponse quoting = ok("vehicle-200-full.json");
    quoting.body = QJsonDocument(root).toJson(QJsonDocument::Compact);
    advanceToNext();
    m_server->enqueue(quoting);
    QVERIFY(cycle().isOk());
    QCOMPARE(countLog("No charging data for [VIN]"), 1);
    QCOMPARE(countLog("PLUG_[VIN]"), 2); // unknown value, then the cycle line

    QVERIFY(!g_log.isEmpty());
    const QString all = g_log.join('\n');
    QVERIFY2(!all.contains(kVin, Qt::CaseInsensitive), "the VIN appears in the log");
    QVERIFY2(!all.contains(QStringLiteral("/api/v1/vehicles")), "a request URL appears in the log");
    QVERIFY(!all.contains(kVin.left(11)));
    QVERIFY(!m_model->state("apiStatusMessage").toString().contains(kVin));
    QVERIFY(!bareModel.state("apiStatusMessage").toString().contains(kVin));
}

QTEST_GUILESS_MAIN(TestSkoda)

#include "testskoda.moc"
