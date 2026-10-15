// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UTILS_DARKTHEME_H
#define UTILS_DARKTHEME_H

#include "utils_global.h"
#include <QColor>

namespace Utils {
QTCREATOR_UTILS_EXPORT void applyDarkTheme();
QTCREATOR_UTILS_EXPORT QColor warningColor();
QTCREATOR_UTILS_EXPORT QColor errorColor();
}

#endif
