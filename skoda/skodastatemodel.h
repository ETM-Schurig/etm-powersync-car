// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM
//
// Turns API responses into the states of the nymea thing (SK-3xx) and writes
// the log of each cycle (SK-4xx). It reads, normalises, timestamps and
// publishes; it decides nothing.

#ifndef SKODASTATEMODEL_H
#define SKODASTATEMODEL_H

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QVariantMap>

#include <functional>

#include "skodaresponse.h"

class SkodaStateModel : public QObject
{
    Q_OBJECT
public:
    using Clock = std::function<QDateTime()>;

    // SK-306
    static constexpr int expiryWarningDays = 14;
    // Below this SoC, batteryCritical (required by the nymea battery interface)
    // is true. 30 %: Patrick's decision of 2026-11-02 (lot car-2).
    static constexpr int batteryCriticalBelowPercent = 30;

    explicit SkodaStateModel(QObject *parent = nullptr);

    void setClock(const Clock &clock);
    void setMaskedApiKey(const QString &maskedApiKey);
    // The VIN is never written to the log: any occurrence in a logged text
    // coming from the API is replaced by "[VIN]".
    void setVin(const QString &vin);

    // Initial values, identical to the defaultValue of integrationpluginskoda.json.
    static QVariantMap defaultStates();

    // Takes the value the thing already has (nymea cached state) without
    // emitting a change, so the model compares against what is published.
    void seedState(const QString &name, const QVariant &value);

    QVariantMap states() const;
    QVariant state(const QString &name) const;

    void setCapacity(double kiloWattHours);
    // Number of AC phases (1 to 3) from the acPhaseCount setting, published
    // under two names: acPhaseCount (electricvehicle interface from nymea
    // 1.16) and phaseCount (electricvehicle interface of nymea 1.15.2, read
    // by the energy manager). Not in the API. A value outside 1-3 is ignored.
    bool setAcPhaseCount(uint phases);
    // Expiry read when the key was validated at pairing (SK-103).
    void setApiKeyExpiresAt(const QDateTime &expiresAt);

    void applyResponse(const SkodaResponse &response);

    // Normalised value of an enumeration of the contract. An unknown or absent
    // value gives "unknown".
    static QString normalizePlugConnectionState(const QString &raw);

    // French message for the user and value of apiStatus (SK-402).
    struct Status {
        QString apiStatus;
        QString message;
        bool connected = false;
    };
    static Status statusForResponse(const SkodaResponse &response);

signals:
    void stateValueChanged(const QString &name, const QVariant &value);
    // Emitted at the end of applyResponse, once every state is set (an
    // action waiting for a reading finishes on it, never before the states).
    void responseApplied(const SkodaResponse &response);

private:
    bool setState(const QString &name, const QVariant &value);
    QString normalizeLogged(const char *field, const QString &raw, QString (*normalize)(const QString &));
    void updateExpiry(const std::optional<QDateTime> &expiresAt, const QDateTime &now);
    void logCycle(const SkodaResponse &response, bool changed);
    void logErrorsList(const SkodaResponse &response);
    QString redacted(QString text) const;

    Clock m_clock;
    QVariantMap m_states;
    QString m_maskedApiKey;
    QString m_vin;
    QDateTime m_apiKeyExpiresAt;
    QSet<QString> m_loggedUnknownValues;
    QList<SkodaVehicleError> m_lastErrors;
    bool m_errorsSeen = false;
};

#endif // SKODASTATEMODEL_H
