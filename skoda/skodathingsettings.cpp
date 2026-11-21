// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM

#include "skodathingsettings.h"

namespace SkodaThingSettings {

QVariant load(QSettings *storage, const QString &group, const QString &key)
{
    storage->beginGroup(group);
    const QVariant value = storage->value(key);
    storage->endGroup();
    return value;
}

void store(QSettings *storage, const QString &group, const QString &key, const QVariant &value)
{
    storage->beginGroup(group);
    if (storage->value(key) != value) {
        storage->setValue(key, value);
        storage->sync();
    }
    storage->endGroup();
}

}
