// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM

#include "skodasettings.h"

#include <cmath>

namespace SkodaSettings {

Bounds pluggedIntervalMinutes()
{
    return {5, 120, 30};
}

Bounds capacityKiloWattHours()
{
    return {0, 200, 0};
}

Bounds acPhaseCount()
{
    return {1, 3, 3, true};
}

bool inBounds(const QVariant &value, const Bounds &bounds)
{
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!value.isValid() || !ok)
        return false;
    if (bounds.wholeNumber && number != std::floor(number))
        return false;
    return number >= bounds.minimum && number <= bounds.maximum;
}

Resolution resolve(const QVariant &stored, const QVariant &current, const Bounds &bounds, const QVariant &legacy)
{
    if (stored.isValid()) {
        if (inBounds(stored, bounds))
            return {stored.toDouble(), Resolution::Stored};
        return {bounds.defaultValue, Resolution::StoredOutOfBounds};
    }
    if (legacy.isValid()) {
        if (inBounds(legacy, bounds))
            return {legacy.toDouble(), Resolution::Legacy};
        return {bounds.defaultValue, Resolution::LegacyOutOfBounds};
    }
    if (inBounds(current, bounds))
        return {current.toDouble(), Resolution::Thing};
    return {bounds.defaultValue, Resolution::ThingOutOfBounds};
}

QString sourceName(Resolution::Source source)
{
    switch (source) {
    case Resolution::Stored:
        return QStringLiteral("plugin storage");
    case Resolution::StoredOutOfBounds:
        return QStringLiteral("plugin storage out of bounds, default used");
    case Resolution::Legacy:
        return QStringLiteral("former thing param, taken over");
    case Resolution::LegacyOutOfBounds:
        return QStringLiteral("former thing param out of bounds, default used");
    case Resolution::Thing:
        return QStringLiteral("thing setting");
    case Resolution::ThingOutOfBounds:
        return QStringLiteral("thing setting out of bounds, default used");
    }
    return QString();
}

}
