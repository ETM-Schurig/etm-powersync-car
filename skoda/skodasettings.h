// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM
//
// Thing settings that the plugin also keeps in its own storage (lot car-8).
// nymea 1.15.2 puts every setting back to its default when a thing is
// reconfigured through the pairing dialog (ThingManagerImplementation::
// confirmPairing); the plugin then sets the stored value again.

#ifndef SKODASETTINGS_H
#define SKODASETTINGS_H

#include <QString>
#include <QVariant>

namespace SkodaSettings {

struct Bounds {
    double minimum = 0;
    double maximum = 0;
    double defaultValue = 0;
    // A value with a fractional part is outside the bounds (phase count).
    bool wholeNumber = false;
};

// Reading interval while plugged in, in minutes: 5 to 120, default 30.
Bounds pluggedIntervalMinutes();
// Battery capacity in kWh: 0 to 200, default 0 (0 = unknown). The same
// bounds as the plugin JSON, which nymea alone checks on SetThingSettings
// (lot car-9); used here only for stored and former values.
Bounds capacityKiloWattHours();
// Number of AC phases the vehicle charges on: 1 to 3, whole numbers, default 3
// (on-board charger of the current electric range, three-phase 11 kW). The
// API does not give it (contract 1.1.0): it is the user's setting.
Bounds acPhaseCount();

// Id of the former thing param batteryCapacity (up to 0.1.4), whose value is
// taken over once. Never to be used again for another param or setting.
inline constexpr const char *legacyBatteryCapacityParamTypeId = "{19bb7fe8-e91e-45b1-a2c6-483834e8ff8b}";

bool inBounds(const QVariant &value, const Bounds &bounds);

struct Resolution {
    enum Source {
        Stored,              // the value kept in the plugin storage
        StoredOutOfBounds,   // stored but outside the bounds: default
        Legacy,              // taken over from a former thing param
        LegacyOutOfBounds,   // former param outside the bounds: default
        Thing,               // the thing's setting as nymea gives it
        ThingOutOfBounds     // thing setting outside the bounds: default
    };
    double value = 0;
    Source source = Thing;
};

// The value a setting must have when the thing is set up. stored: the value
// in the plugin storage (invalid: never stored); current: the thing setting;
// legacy: a former thing param to take over once (invalid: none). The stored
// value wins; without one, the legacy param; else the thing setting.
Resolution resolve(const QVariant &stored, const QVariant &current, const Bounds &bounds,
                   const QVariant &legacy = QVariant());

QString sourceName(Resolution::Source source);

}

#endif // SKODASETTINGS_H
