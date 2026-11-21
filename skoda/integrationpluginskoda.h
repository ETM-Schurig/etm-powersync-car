// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM
//
// nymea integration plugin for the MyŠkoda Public API. Read-only: no command
// is sent to the vehicle.

#ifndef INTEGRATIONPLUGINSKODA_H
#define INTEGRATIONPLUGINSKODA_H

#include <integrations/integrationplugin.h>

#include "extern-plugininfo.h"
#include "skodaresponse.h"
#include "skodasettings.h"

#include <QHash>

class SkodaApiClient;
class SkodaPoller;
class SkodaStateModel;

class IntegrationPluginSkoda : public IntegrationPlugin
{
    Q_OBJECT

    Q_PLUGIN_METADATA(IID "io.nymea.IntegrationPlugin" FILE "integrationpluginskoda.json")
    Q_INTERFACES(IntegrationPlugin)

public:
    explicit IntegrationPluginSkoda(QObject *parent = nullptr);
    ~IntegrationPluginSkoda() override;

    void startPairing(ThingPairingInfo *info) override;
    void confirmPairing(ThingPairingInfo *info, const QString &username, const QString &secret) override;
    void setupThing(ThingSetupInfo *info) override;
    void postSetupThing(Thing *thing) override;
    void executeAction(ThingActionInfo *info) override;
    void thingRemoved(Thing *thing) override;

private:
    struct Session {
        SkodaApiClient *client = nullptr;
        SkodaPoller *poller = nullptr;
        SkodaStateModel *model = nullptr;
    };

    SkodaApiClient *createClient(const QString &vin, const QString &apiKey, QObject *parent);
    void storeApiKey(const ThingId &thingId, const QString &apiKey, const QDateTime &expiresAt);
    QString loadApiKey(const ThingId &thingId, QDateTime *expiresAt) const;
    // SK-200: time of the last request for a vehicle, kept across restarts
    // and reconfigurations so that none of them causes a burst.
    void storeLastRequestAt(const ThingId &thingId, const QDateTime &at);
    QDateTime loadLastRequestAt(const ThingId &thingId) const;
    void destroySession(Thing *thing);

    QHash<Thing *, Session> m_sessions;
    // Answer of the pairing validation, used as the first reading by the
    // setupThing that follows (lot car-7).
    QHash<ThingId, SkodaResponse> m_pairingResponses;
};

#endif // INTEGRATIONPLUGINSKODA_H
