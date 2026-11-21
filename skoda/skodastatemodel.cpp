// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM

#include "skodastatemodel.h"
#include "skodalogging.h"

#include <QStringList>

#include <cmath>

SkodaStateModel::SkodaStateModel(QObject *parent)
    : QObject(parent),
      m_clock([]() { return QDateTime::currentDateTimeUtc(); }),
      m_states(defaultStates())
{
}

void SkodaStateModel::setClock(const Clock &clock)
{
    m_clock = clock;
}

void SkodaStateModel::setMaskedApiKey(const QString &maskedApiKey)
{
    m_maskedApiKey = maskedApiKey;
}

void SkodaStateModel::setVin(const QString &vin)
{
    m_vin = vin;
}

QString SkodaStateModel::redacted(QString text) const
{
    if (!m_vin.isEmpty())
        text.replace(m_vin, QStringLiteral("[VIN]"), Qt::CaseInsensitive);
    return text;
}

QVariantMap SkodaStateModel::defaultStates()
{
    return {
        {QStringLiteral("connected"), false},
        {QStringLiteral("batteryLevel"), 0},
        {QStringLiteral("socKnown"), false},
        {QStringLiteral("batteryCritical"), false},
        {QStringLiteral("capacity"), 0.0},
        {QStringLiteral("acPhaseCount"), 3u},
        {QStringLiteral("phaseCount"), 3u},
        {QStringLiteral("socTimestamp"), 0},
        {QStringLiteral("socTimestampSource"), QStringLiteral("unknown")},
        {QStringLiteral("plugState"), QStringLiteral("unknown")},
        {QStringLiteral("apiKeyExpiresAt"), 0},
        {QStringLiteral("apiKeyExpiringSoon"), false},
        {QStringLiteral("apiStatus"), QStringLiteral("unknown")},
        {QStringLiteral("apiStatusMessage"), QString()},
        {QStringLiteral("rateLimitRemaining"), -1},
    };
}

void SkodaStateModel::seedState(const QString &name, const QVariant &value)
{
    if (m_states.contains(name) && value.isValid())
        m_states.insert(name, value);
}

QVariantMap SkodaStateModel::states() const
{
    return m_states;
}

QVariant SkodaStateModel::state(const QString &name) const
{
    return m_states.value(name);
}

void SkodaStateModel::setCapacity(double kiloWattHours)
{
    setState(QStringLiteral("capacity"), kiloWattHours > 0 ? kiloWattHours : 0.0);
}

bool SkodaStateModel::setAcPhaseCount(uint phases)
{
    if (phases < 1 || phases > 3)
        return false;
    setState(QStringLiteral("acPhaseCount"), phases);
    setState(QStringLiteral("phaseCount"), phases);
    return true;
}

void SkodaStateModel::setApiKeyExpiresAt(const QDateTime &expiresAt)
{
    updateExpiry(expiresAt.isValid() ? std::optional<QDateTime>(expiresAt) : std::nullopt, m_clock());
}

bool SkodaStateModel::setState(const QString &name, const QVariant &value)
{
    if (m_states.value(name) == value && m_states.value(name).typeId() == value.typeId())
        return false;
    m_states.insert(name, value);
    emit stateValueChanged(name, value);
    return true;
}

QString SkodaStateModel::normalizePlugConnectionState(const QString &raw)
{
    if (raw == QLatin1String("CONNECTED"))
        return QStringLiteral("connected");
    if (raw == QLatin1String("DISCONNECTED"))
        return QStringLiteral("disconnected");
    return QStringLiteral("unknown");
}

QString SkodaStateModel::normalizeLogged(const char *field, const QString &raw, QString (*normalize)(const QString &))
{
    const QString normalized = normalize(raw);
    if (normalized == QLatin1String("unknown")) {
        // SK-102: an unknown value is logged once, never an error.
        const QString key = QString::fromLatin1(field) + QLatin1Char('=') + raw;
        if (!m_loggedUnknownValues.contains(key)) {
            m_loggedUnknownValues.insert(key);
            qCInfo(dcSkodaCore()) << "Unknown value" << redacted(raw) << "for" << field << "- published as unknown";
        }
    }
    return normalized;
}

SkodaStateModel::Status SkodaStateModel::statusForResponse(const SkodaResponse &response)
{
    Status status;
    const QString type = response.problemType;

    if (response.kind == SkodaResponse::KindOk) {
        status.apiStatus = QStringLiteral("ok");
        status.connected = true;
        if (!response.charging.present) {
            QStringList types;
            for (const SkodaVehicleError &error : response.errors)
                types.append(error.type);
            status.message = QStringLiteral("Données de charge absentes de la dernière réponse%1 : valeurs précédentes conservées.")
                                 .arg(types.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(types.join(QStringLiteral(", "))));
        }
        return status;
    }

    if (response.kind == SkodaResponse::KindNetworkError) {
        status.apiStatus = QStringLiteral("serviceUnavailable");
        status.message = QStringLiteral("Le serveur MyŠkoda est injoignable. Nouvelle tentative automatique.");
        return status;
    }
    if (response.kind == SkodaResponse::KindRefused) {
        status.apiStatus = QStringLiteral("error");
        status.message = QStringLiteral("Requête refusée par le plugin (adresse non autorisée).");
        return status;
    }

    switch (response.httpStatus) {
    case 200:
        status.apiStatus = QStringLiteral("error");
        status.message = QStringLiteral("Réponse illisible de l'API MyŠkoda. Nouvelle tentative automatique.");
        return status;
    case 401:
        if (type == QLatin1String("api-key-expired")) {
            status.apiStatus = QStringLiteral("keyExpired");
            status.message = QStringLiteral("La clé API a expiré. Créez une nouvelle clé dans l'app MyŠkoda, puis reconfigurez le véhicule.");
        } else {
            status.apiStatus = QStringLiteral("keyInvalid");
            status.message = QStringLiteral("La clé API est refusée (absente ou invalide). Vérifiez-la dans l'app MyŠkoda, puis reconfigurez le véhicule.");
        }
        return status;
    case 403:
        if (type == QLatin1String("operation-not-authorized")) {
            status.apiStatus = QStringLiteral("keyNotAuthorized");
            status.message = QStringLiteral("Le véhicule refuse l'opération pour le compte de cette clé API.");
        } else {
            status.apiStatus = QStringLiteral("keyNotAuthorized");
            status.message = QStringLiteral("Cette clé API ne couvre pas ce VIN (VIN non couvert). Ajoutez le véhicule à la clé dans l'app MyŠkoda, puis reconfigurez le véhicule.");
        }
        return status;
    case 404:
        status.apiStatus = QStringLiteral("vehicleNotFound");
        status.message = QStringLiteral("Aucun véhicule ne correspond à ce VIN. Vérifiez le VIN, puis reconfigurez le véhicule.");
        return status;
    case 429:
        status.connected = true;
        if (type == QLatin1String("vehicle-not-accepting-requests")) {
            status.apiStatus = QStringLiteral("vehicleNotAccepting");
            status.message = QStringLiteral("Le véhicule refuse les requêtes pour le moment. Nouvelle lecture plus tard.");
        } else {
            status.apiStatus = QStringLiteral("rateLimited");
            if (response.retryAfter) {
                const int minutes = static_cast<int>(std::ceil(*response.retryAfter / 60.0));
                status.message = QStringLiteral("Quota de requêtes dépassé. Nouvelle lecture dans %1 min.").arg(minutes);
            } else {
                status.message = QStringLiteral("Quota de requêtes dépassé. Nouvelle lecture plus tard.");
            }
        }
        return status;
    case 422:
        status.apiStatus = QStringLiteral("error");
        status.message = type == QLatin1String("operation-disabled")
                             ? QStringLiteral("La fonction est désactivée pour ce véhicule.")
                             : QStringLiteral("Le véhicule ne prend pas en charge cette opération.");
        return status;
    case 504:
        status.apiStatus = QStringLiteral("vehicleUnavailable");
        status.message = QStringLiteral("Le véhicule n'a pas répondu à temps. Nouvelle tentative automatique.");
        return status;
    default:
        break;
    }

    if (response.httpStatus >= 500) {
        status.apiStatus = QStringLiteral("serviceUnavailable");
        status.message = QStringLiteral("Le service MyŠkoda est indisponible (HTTP %1). Nouvelle tentative automatique.").arg(response.httpStatus);
    } else {
        status.apiStatus = QStringLiteral("error");
        status.message = QStringLiteral("Réponse inattendue de l'API MyŠkoda (HTTP %1). Nouvelle tentative automatique.").arg(response.httpStatus);
    }
    return status;
}

void SkodaStateModel::updateExpiry(const std::optional<QDateTime> &expiresAt, const QDateTime &now)
{
    if (expiresAt && expiresAt->isValid()) {
        m_apiKeyExpiresAt = *expiresAt;
        setState(QStringLiteral("apiKeyExpiresAt"), static_cast<int>(expiresAt->toSecsSinceEpoch()));
    }
    if (m_apiKeyExpiresAt.isValid())
        setState(QStringLiteral("apiKeyExpiringSoon"), now.secsTo(m_apiKeyExpiresAt) < qint64(expiryWarningDays) * 24 * 3600);
}

void SkodaStateModel::applyResponse(const SkodaResponse &response)
{
    const QDateTime now = response.receivedAt.isValid() ? response.receivedAt : m_clock();
    const Status status = statusForResponse(response);

    setState(QStringLiteral("connected"), status.connected);
    setState(QStringLiteral("apiStatus"), status.apiStatus);
    setState(QStringLiteral("apiStatusMessage"), status.message);
    if (response.rateLimitRemaining)
        setState(QStringLiteral("rateLimitRemaining"), *response.rateLimitRemaining);
    updateExpiry(response.apiKeyExpiresAt, now);

    bool changed = false;
    if (response.isOk()) {
        logErrorsList(response);
        const SkodaChargingReading &charging = response.charging;
        if (charging.present) {
            if (charging.stateOfChargeInPercent) {
                const int soc = *charging.stateOfChargeInPercent;
                // SK-301: the vehicle's timestamp when the contract gives one.
                const bool fromVehicle = charging.carCapturedTimestamp.has_value();
                const QDateTime timestamp = fromVehicle ? *charging.carCapturedTimestamp : now;
                changed |= setState(QStringLiteral("batteryLevel"), soc);
                // batteryLevel cannot be published without a value in nymea 1.15.2
                // (Thing::setStateValue discards an invalid QVariant): 0 until a
                // first reading, and socKnown tells the two apart. Cached state,
                // so it survives a restart together with batteryLevel.
                changed |= setState(QStringLiteral("socKnown"), true);
                changed |= setState(QStringLiteral("batteryCritical"), soc < batteryCriticalBelowPercent);
                changed |= setState(QStringLiteral("socTimestamp"), static_cast<int>(timestamp.toSecsSinceEpoch()));
                changed |= setState(QStringLiteral("socTimestampSource"),
                                    fromVehicle ? QStringLiteral("vehicle") : QStringLiteral("reception"));
            }
            // SK-302, SK-307: an absent plug state keeps its last value.
            if (charging.plugConnectionState)
                changed |= setState(QStringLiteral("plugState"),
                                    normalizeLogged("plugConnectionState", *charging.plugConnectionState, &normalizePlugConnectionState));
        }
    }
    logCycle(response, changed);
    emit responseApplied(response);
}

void SkodaStateModel::logErrorsList(const SkodaResponse &response)
{
    // SK-403: info level, only when the content changes.
    if (m_errorsSeen && response.errors == m_lastErrors)
        return;
    const bool firstAndEmpty = !m_errorsSeen && response.errors.isEmpty();
    m_errorsSeen = true;
    m_lastErrors = response.errors;
    if (firstAndEmpty)
        return;
    if (response.errors.isEmpty()) {
        qCInfo(dcSkodaCore()) << "API errors list is now empty";
        return;
    }
    QStringList entries;
    for (const SkodaVehicleError &error : response.errors)
        entries.append(error.description.isEmpty() ? error.type : error.type + QStringLiteral(" (") + error.description + QLatin1Char(')'));
    qCInfo(dcSkodaCore()).noquote() << "API errors list:" << redacted(entries.join(QStringLiteral("; ")));
}

void SkodaStateModel::logCycle(const SkodaResponse &response, bool changed)
{
    // SK-400: one line per cycle, including "nothing changed".
    QStringList parts;
    if (response.isOk()) {
        parts.append(changed ? QStringLiteral("Cycle HTTP 200: states updated") : QStringLiteral("Cycle HTTP 200: nothing changed"));
        const SkodaChargingReading &c = response.charging;
        if (!c.present) {
            parts.append(QStringLiteral("charging part absent"));
        } else {
            QStringList read;
            read.append(c.stateOfChargeInPercent ? QStringLiteral("SoC %1 %").arg(*c.stateOfChargeInPercent) : QStringLiteral("SoC absent"));
            read.append(c.carCapturedTimestamp ? QStringLiteral("vehicle time %1").arg(c.carCapturedTimestamp->toString(Qt::ISODate))
                                               : QStringLiteral("no vehicle time"));
            read.append(QStringLiteral("plug %1").arg(c.plugConnectionState.value_or(QStringLiteral("absent"))));
            parts.append(QStringLiteral("read: ") + read.join(QStringLiteral(", ")));
        }
    } else if (response.kind == SkodaResponse::KindNetworkError) {
        parts.append(QStringLiteral("Cycle failed: network error (%1)").arg(response.errorString));
    } else {
        parts.append(QStringLiteral("Cycle failed: HTTP %1 %2").arg(response.httpStatus).arg(response.problemType));
        if (!response.errorString.isEmpty())
            parts.append(response.errorString);
    }

    if (response.rateLimitRemaining) {
        parts.append(QStringLiteral("quota %1/%2, reset in %3 s")
                         .arg(*response.rateLimitRemaining)
                         .arg(response.rateLimitLimit ? QString::number(*response.rateLimitLimit) : QStringLiteral("?"))
                         .arg(response.rateLimitReset ? QString::number(*response.rateLimitReset) : QStringLiteral("?")));
    } else {
        parts.append(QStringLiteral("quota unknown"));
    }

    if (m_apiKeyExpiresAt.isValid()) {
        const qint64 days = (response.receivedAt.isValid() ? response.receivedAt : m_clock()).secsTo(m_apiKeyExpiresAt) / 86400;
        parts.append(QStringLiteral("key %1 expires in %2 days").arg(m_maskedApiKey).arg(days));
    } else {
        parts.append(QStringLiteral("key %1 expiry unknown").arg(m_maskedApiKey));
    }

    qCInfo(dcSkodaCore()).noquote() << redacted(parts.join(QStringLiteral(" | ")));
}
