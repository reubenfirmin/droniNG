/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UTILS_QTLOGGING_H
#define UTILS_QTLOGGING_H

#include "utils_global.h"
#include <QtCore/qlogging.h>

namespace Utils {
// Keep the Qt warning workaround active when the Debug gadget takes over logging.
QTCREATOR_UTILS_EXPORT void installQtMessageHandler(QtMessageHandler handler = nullptr);
}

#endif
