// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM
//
// Glue between a thing setting and the plugin storage (lots car-8, car-10).
// Written as templates over the thing type so that the tests drive it with a
// fake thing, without libnymea or nymead; the plugin uses it with Thing.
//
// Order of a reconfiguration through the pairing dialog in nymea 1.15.2
// (libnymea-core/integrations/thingmanagerimplementation.cpp, confirmPairing):
//   l. 779  thing->setSetupStatus(InProgress)        (same Thing object)
//   l. 784-785  thing->setSettings(defaults)         -> settingChanged per setting
//   l. 789  setupThing(thing)                        -> plugin setupThing
//   l. 812  setSetupStatus(Complete)
//   l. 821  storeConfiguredThings()
//   l. 823  postSetupThing(thing)
// The handlers connected by the previous setupThing are still connected at
// l. 785. They must not take nymea's defaults for a user change: a change
// arriving while the setup is in progress is not stored (follow()). The stored
// value is then set again in setupThing (restore()), after l. 785, and nymea
// writes it to its config at l. 821.

#ifndef SKODATHINGSETTINGS_H
#define SKODATHINGSETTINGS_H

#include "skodalogging.h"
#include "skodasettings.h"

#include <QObject>
#include <QSettings>
#include <QUuid>

namespace SkodaThingSettings {

QVariant load(QSettings *storage, const QString &group, const QString &key);
void store(QSettings *storage, const QString &group, const QString &key, const QVariant &value);

// At setupThing: resolves the setting (SkodaSettings::resolve), keeps it in the
// storage and sets it on the thing when it differs. Returns the value in force.
template<typename ThingT>
double restore(ThingT *thing, const QUuid &settingTypeId, QSettings *storage, const QString &key,
               const SkodaSettings::Bounds &bounds, const QVariant &legacy = QVariant())
{
    const QString group = thing->id().toString();
    const QVariant current = thing->setting(settingTypeId);
    const SkodaSettings::Resolution resolution = SkodaSettings::resolve(load(storage, group, key), current, bounds, legacy);
    switch (resolution.source) {
    case SkodaSettings::Resolution::StoredOutOfBounds:
    case SkodaSettings::Resolution::LegacyOutOfBounds:
    case SkodaSettings::Resolution::ThingOutOfBounds:
        qCWarning(dcSkodaCore()) << thing->name() << "setting" << key << ":" << SkodaSettings::sourceName(resolution.source)
                             << "(" << resolution.value << ")";
        break;
    case SkodaSettings::Resolution::Legacy:
        qCInfo(dcSkodaCore()) << thing->name() << "setting" << key << ":" << SkodaSettings::sourceName(resolution.source)
                          << "(" << resolution.value << ")";
        break;
    case SkodaSettings::Resolution::Stored:
        if (current.toDouble() != resolution.value)
            qCInfo(dcSkodaCore()) << thing->name() << "setting" << key << "set again from the plugin storage:" << resolution.value
                              << "(the thing had" << current << ", e.g. after a reconfiguration through the pairing dialog)";
        break;
    case SkodaSettings::Resolution::Thing:
        break;
    }
    store(storage, group, key, resolution.value);
    if (!current.isValid() || current.toDouble() != resolution.value)
        thing->setSettingValue(settingTypeId, resolution.value);
    return resolution.value;
}

// Follows the user's changes (SetThingSettings): each one is stored, then
// applied. A change while the setup is in progress comes from nymea, not from
// the user (defaults put back at l. 785, or restore() itself): not stored.
// checkBounds: ignore a value outside the bounds (nymea already checks them).
template<typename ThingT, typename Apply>
QMetaObject::Connection follow(ThingT *thing, QObject *context, const QUuid &settingTypeId, QSettings *storage,
                               const QString &key, const SkodaSettings::Bounds &bounds, bool checkBounds, Apply apply)
{
    return QObject::connect(thing, &ThingT::settingChanged, context,
                            [thing, settingTypeId, storage, key, bounds, checkBounds, apply](const QUuid &paramTypeId, const QVariant &value) {
        if (paramTypeId != settingTypeId)
            return;
        if (thing->setupStatus() == ThingT::ThingSetupStatusInProgress) {
            qCDebug(dcSkodaCore()) << thing->name() << "setting" << key << "=" << value
                               << "during the setup (set by nymea), not stored";
            return;
        }
        if (checkBounds && !SkodaSettings::inBounds(value, bounds)) {
            qCWarning(dcSkodaCore()) << thing->name() << "setting" << key << value << "outside its bounds, ignored";
            return;
        }
        store(storage, thing->id().toString(), key, value.toDouble());
        apply(value.toDouble());
    });
}

}

#endif // SKODATHINGSETTINGS_H
