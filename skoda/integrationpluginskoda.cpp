// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM

#include "integrationpluginskoda.h"
#include "plugininfo.h"

#include "skodaapiclient.h"
#include "skodapoller.h"
#include "skodastatemodel.h"
#include "skodathingsettings.h"

#include <hardwaremanager.h>
#include <network/networkaccessmanager.h>

#include <QFile>
#include <QSettings>

IntegrationPluginSkoda::IntegrationPluginSkoda(QObject *parent)
    : IntegrationPlugin(parent)
{
}

IntegrationPluginSkoda::~IntegrationPluginSkoda()
{
}

SkodaApiClient *IntegrationPluginSkoda::createClient(const QString &vin, const QString &apiKey, QObject *parent)
{
    NetworkAccessManager *network = hardwareManager()->networkManager();
    auto get = [network](const QNetworkRequest &request) { return network->get(request); };
    // The base URL is a constant: no setting can point the plugin elsewhere (SK-602, SK-604).
    return new SkodaApiClient(get, SkodaApiClient::productionBaseUrl(), vin, apiKey, parent);
}

namespace {

const QString intervalStorageKey = QStringLiteral("pluggedIntervalMinutes");
const QString capacityStorageKey = QStringLiteral("capacity");
const QString acPhaseCountStorageKey = QStringLiteral("acPhaseCount");
// The former thing param batteryCapacity (up to 0.1.4). nymea 1.15.2 still
// loads a param that left the plugin JSON ("legacy param",
// loadConfiguredThings) so that a plugin can take its value over.
const ParamTypeId legacyBatteryCapacityParamTypeId = ParamTypeId(QString::fromLatin1(SkodaSettings::legacyBatteryCapacityParamTypeId));

void applyInterval(Thing *thing, SkodaPoller *poller, int minutes)
{
    if (minutes * 60 == poller->pluggedInterval())
        return;
    if (!poller->setPluggedInterval(minutes)) {
        qCWarning(dcSkoda()) << thing->name() << "reading interval" << minutes << "min refused:" << SkodaPoller::intervalError(minutes);
        return;
    }
    qCInfo(dcSkoda()) << thing->name() << "reading interval while plugged in:" << minutes << "min";
}

}

void IntegrationPluginSkoda::startPairing(ThingPairingInfo *info)
{
    const QString vin = info->params().paramValue(vehicleThingVinParamTypeId).toString().trimmed().toUpper();
    if (!SkodaApiClient::isValidVin(vin)) {
        info->finish(Thing::ThingErrorInvalidParameter,
                     QT_TR_NOOP("Le VIN doit compter 17 caractères (chiffres et lettres majuscules, sans I, O ni Q)."));
        return;
    }
    info->finish(Thing::ThingErrorNoError,
                 QT_TR_NOOP("Saisissez la clé API créée dans l'app MyŠkoda pour ce véhicule."));
}

void IntegrationPluginSkoda::confirmPairing(ThingPairingInfo *info, const QString &username, const QString &secret)
{
    Q_UNUSED(username)
    const QString vin = info->params().paramValue(vehicleThingVinParamTypeId).toString().trimmed().toUpper();
    const QString apiKey = secret.trimmed();
    if (!SkodaApiClient::isValidVin(vin)) {
        info->finish(Thing::ThingErrorInvalidParameter, QT_TR_NOOP("VIN invalide."));
        return;
    }
    if (apiKey.isEmpty()) {
        info->finish(Thing::ThingErrorAuthenticationFailure, QT_TR_NOOP("La clé API est vide."));
        return;
    }

    // SK-103: one validation request at pairing.
    SkodaApiClient *client = createClient(vin, apiKey, info);
    qCInfo(dcSkoda()) << "Validating API key" << client->maskedApiKey() << "for the vehicle";
    connect(client, &SkodaApiClient::vehicleFetched, info, [this, info, apiKey](const SkodaResponse &response) {
        const SkodaStateModel::Status status = SkodaStateModel::statusForResponse(response);
        if (response.isOk()) {
            storeApiKey(info->thingId(), apiKey, response.apiKeyExpiresAt.value_or(QDateTime()));
            // A 200 with the charging part serves as the first reading: the
            // setupThing that follows sends no request of its own.
            if (SkodaPoller::usableAsFirstReading(response))
                m_pairingResponses.insert(info->thingId(), response);
            else
                m_pairingResponses.remove(info->thingId());
            qCInfo(dcSkoda()) << "API key accepted, expires at"
                              << (response.apiKeyExpiresAt ? response.apiKeyExpiresAt->toString(Qt::ISODate) : QStringLiteral("unknown"));
            info->finish(Thing::ThingErrorNoError);
            return;
        }
        qCWarning(dcSkoda()) << "API key validation failed: HTTP" << response.httpStatus << response.problemType;
        if (response.httpStatus == 401) {
            info->finish(Thing::ThingErrorAuthenticationFailure, status.message);
        } else if (response.httpStatus == 403) {
            info->finish(Thing::ThingErrorAuthenticationFailure, status.message);
        } else if (response.httpStatus == 404) {
            info->finish(Thing::ThingErrorInvalidParameter, status.message);
        } else {
            info->finish(Thing::ThingErrorHardwareNotAvailable, status.message);
        }
    });
    if (!client->fetchVehicle()) {
        info->finish(Thing::ThingErrorHardwareFailure, QT_TR_NOOP("La requête de validation n'a pas pu être envoyée."));
        return;
    }
    // The validation request counts for the minimum gap: the first poll after
    // setupThing comes 5 min later, not straight after it (SK-200).
    storeLastRequestAt(info->thingId(), QDateTime::currentDateTimeUtc());
}

void IntegrationPluginSkoda::setupThing(ThingSetupInfo *info)
{
    Thing *thing = info->thing();
    const QString vin = thing->paramValue(vehicleThingVinParamTypeId).toString().trimmed().toUpper();
    if (!SkodaApiClient::isValidVin(vin)) {
        info->finish(Thing::ThingErrorInvalidParameter, QT_TR_NOOP("VIN invalide."));
        return;
    }
    QDateTime expiresAt;
    const QString apiKey = loadApiKey(thing->id(), &expiresAt);
    if (apiKey.isEmpty()) {
        info->finish(Thing::ThingErrorAuthenticationFailure,
                     QT_TR_NOOP("Aucune clé API enregistrée : reconfigurez le véhicule avec une clé de l'app MyŠkoda."));
        return;
    }

    // A reconfiguration calls setupThing again on the same thing.
    destroySession(thing);

    Session session;
    session.client = createClient(vin, apiKey, this);
    session.poller = new SkodaPoller(session.client, this);
    session.poller->setLastRequestAt(loadLastRequestAt(thing->id()));
    // Thing setting (SetThingSettings, bounds checked by nymea): applied at
    // run time, without reconfiguration and without a request. The plugin
    // keeps it too, and sets it again after a reconfiguration through the
    // pairing dialog, which puts every setting back to its default; the
    // defaults nymea puts back are not stored (SkodaThingSettings, lot car-10).
    const int interval = static_cast<int>(SkodaThingSettings::restore(thing, vehicleSettingsPluggedIntervalMinutesParamTypeId,
                                                                      pluginStorage(), intervalStorageKey,
                                                                      SkodaSettings::pluggedIntervalMinutes()));
    applyInterval(thing, session.poller, interval);
    SkodaPoller *poller = session.poller;
    SkodaThingSettings::follow(thing, poller, vehicleSettingsPluggedIntervalMinutesParamTypeId, pluginStorage(),
                               intervalStorageKey, SkodaSettings::pluggedIntervalMinutes(), true,
                               [thing, poller](double minutes) { applyInterval(thing, poller, static_cast<int>(minutes)); });
    const ThingId thingId = thing->id();
    connect(session.poller, &SkodaPoller::requestSent, this, [this, thingId](const QDateTime &at) {
        storeLastRequestAt(thingId, at);
    });
    session.model = new SkodaStateModel(this);
    session.model->setMaskedApiKey(session.client->maskedApiKey());
    session.model->setVin(vin);

    // Start from the values the thing already has (cached states), so that an
    // absent field keeps its last value across restarts (SK-307).
    const QVariantMap defaults = SkodaStateModel::defaultStates();
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it)
        session.model->seedState(it.key(), thing->stateValue(it.key()));
    connect(session.model, &SkodaStateModel::stateValueChanged, thing, [thing](const QString &name, const QVariant &value) {
        thing->setStateValue(name, value);
    });
    connect(session.poller, &SkodaPoller::responseReceived, session.model, &SkodaStateModel::applyResponse);

    // Capacity: a thing setting since 0.1.5, kept by the plugin like the
    // interval. The first time, the value of the former param is taken over.
    session.model->setCapacity(SkodaThingSettings::restore(thing, vehicleSettingsCapacityParamTypeId, pluginStorage(),
                                                           capacityStorageKey, SkodaSettings::capacityKiloWattHours(),
                                                           thing->paramValue(legacyBatteryCapacityParamTypeId)));
    SkodaStateModel *model = session.model;
    // Bounds (0-200 kWh) checked by nymea alone (SetThingSettings, lot car-9).
    SkodaThingSettings::follow(thing, model, vehicleSettingsCapacityParamTypeId, pluginStorage(), capacityStorageKey,
                               SkodaSettings::capacityKiloWattHours(), false, [thing, model](double kiloWattHours) {
        model->setCapacity(kiloWattHours);
        qCInfo(dcSkoda()) << thing->name() << "battery capacity:" << kiloWattHours << "kWh";
    });
    // AC phases (1-3): not in the API, a thing setting kept like the others
    // and published as the states acPhaseCount and phaseCount. A value
    // outside 1-3 is refused by nymea, and ignored here too.
    session.model->setAcPhaseCount(static_cast<uint>(SkodaThingSettings::restore(thing, vehicleSettingsAcPhaseCountParamTypeId,
                                                                                 pluginStorage(), acPhaseCountStorageKey,
                                                                                 SkodaSettings::acPhaseCount())));
    SkodaThingSettings::follow(thing, model, vehicleSettingsAcPhaseCountParamTypeId, pluginStorage(), acPhaseCountStorageKey,
                               SkodaSettings::acPhaseCount(), true, [thing, model](double phases) {
        model->setAcPhaseCount(static_cast<uint>(phases));
        qCInfo(dcSkoda()) << thing->name() << "AC phases:" << static_cast<uint>(phases);
    });
    session.model->setApiKeyExpiresAt(expiresAt);

    // Right after a pairing (new vehicle or reconfiguration with the key):
    // the validation answer is the first reading, if it is recent.
    if (m_pairingResponses.contains(thing->id())) {
        const SkodaResponse response = m_pairingResponses.take(thing->id());
        if (response.receivedAt.isValid()
                && response.receivedAt.secsTo(QDateTime::currentDateTimeUtc()) < SkodaPoller::minimumGapSeconds)
            session.poller->acceptResponse(response);
    }
    m_sessions.insert(thing, session);

    qCInfo(dcSkoda()) << "Set up" << thing->name() << "with API key" << session.client->maskedApiKey()
                      << "- reading every" << session.poller->pluggedInterval() / 60 << "min while plugged in, none while unplugged";
    info->finish(Thing::ThingErrorNoError);
}

void IntegrationPluginSkoda::postSetupThing(Thing *thing)
{
    if (!m_sessions.contains(thing))
        return;
    m_sessions.value(thing).poller->start();
}

void IntegrationPluginSkoda::executeAction(ThingActionInfo *info)
{
    Thing *thing = info->thing();
    if (info->action().actionTypeId() != vehicleRefreshActionTypeId) {
        info->finish(Thing::ThingErrorActionTypeNotFound);
        return;
    }
    if (!m_sessions.contains(thing)) {
        info->finish(Thing::ThingErrorHardwareNotAvailable, QT_TR_NOOP("Le véhicule n'est pas configuré."));
        return;
    }
    const Session session = m_sessions.value(thing);
    QString refusal;
    if (session.poller->refresh(&refusal) == SkodaPoller::RefreshResult::Refused) {
        // No request sent.
        info->finish(Thing::ThingErrorHardwareNotAvailable, refusal);
        return;
    }
    // Sent, or a request already running: finish on its answer, once the
    // model has set every state (contract asked of plugins by the energy manager).
    connect(session.model, &SkodaStateModel::responseApplied, info, [info](const SkodaResponse &response) {
        if (response.isOk()) {
            info->finish(Thing::ThingErrorNoError);
        } else {
            info->finish(Thing::ThingErrorHardwareNotAvailable, SkodaStateModel::statusForResponse(response).message);
        }
    }, Qt::SingleShotConnection);
}

void IntegrationPluginSkoda::thingRemoved(Thing *thing)
{
    destroySession(thing);
    m_pairingResponses.remove(thing->id());
    pluginStorage()->remove(thing->id().toString());
    pluginStorage()->sync();
}

void IntegrationPluginSkoda::destroySession(Thing *thing)
{
    if (!m_sessions.contains(thing))
        return;
    Session session = m_sessions.take(thing);
    // The setting handlers of this session stop here, not at deleteLater().
    QObject::disconnect(thing, nullptr, session.poller, nullptr);
    QObject::disconnect(thing, nullptr, session.model, nullptr);
    session.poller->stop();
    session.poller->deleteLater();
    session.model->deleteLater();
    session.client->deleteLater();
}

void IntegrationPluginSkoda::storeApiKey(const ThingId &thingId, const QString &apiKey, const QDateTime &expiresAt)
{
    // The key is kept in the plugin storage, not in a thing param: params are
    // returned in clear by the JSON-RPC API, the plugin storage is not (SK-601).
    QSettings *storage = pluginStorage();
    storage->beginGroup(thingId.toString());
    storage->setValue("apiKey", apiKey);
    storage->setValue("apiKeyExpiresAt", expiresAt.isValid() ? expiresAt.toString(Qt::ISODate) : QString());
    storage->endGroup();
    storage->sync();
    // Readable by the nymead user only.
    QFile::setPermissions(storage->fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QString IntegrationPluginSkoda::loadApiKey(const ThingId &thingId, QDateTime *expiresAt) const
{
    QSettings *storage = pluginStorage();
    storage->beginGroup(thingId.toString());
    const QString apiKey = storage->value("apiKey").toString();
    const QString expiry = storage->value("apiKeyExpiresAt").toString();
    storage->endGroup();
    if (expiresAt)
        *expiresAt = expiry.isEmpty() ? QDateTime() : QDateTime::fromString(expiry, Qt::ISODate);
    return apiKey;
}

void IntegrationPluginSkoda::storeLastRequestAt(const ThingId &thingId, const QDateTime &at)
{
    QSettings *storage = pluginStorage();
    storage->beginGroup(thingId.toString());
    storage->setValue("lastRequestAt", at.toUTC().toString(Qt::ISODateWithMs));
    storage->endGroup();
    storage->sync();
}

QDateTime IntegrationPluginSkoda::loadLastRequestAt(const ThingId &thingId) const
{
    QSettings *storage = pluginStorage();
    storage->beginGroup(thingId.toString());
    const QString value = storage->value("lastRequestAt").toString();
    storage->endGroup();
    return value.isEmpty() ? QDateTime() : QDateTime::fromString(value, Qt::ISODateWithMs);
}
