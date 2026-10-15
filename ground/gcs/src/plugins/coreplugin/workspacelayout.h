/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "core_global.h"
class QSettings;

namespace Core::Internal {
// Move the old flight panels into the dashboard while retaining map state.
CORE_EXPORT void migrateFlightWorkspace(QSettings *settings);
}
