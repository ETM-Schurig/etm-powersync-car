// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM

#ifndef SKODALOGGING_H
#define SKODALOGGING_H

#include <QLoggingCategory>

// Same category name as the one nymea generates for the plugin ("Skoda"),
// so one logging filter covers the plugin and its core classes.
Q_DECLARE_LOGGING_CATEGORY(dcSkodaCore)

#endif // SKODALOGGING_H
