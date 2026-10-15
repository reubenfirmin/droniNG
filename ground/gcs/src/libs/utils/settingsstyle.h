// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UTILS_SETTINGSSTYLE_H
#define UTILS_SETTINGSSTYLE_H

#include "utils_global.h"
#include <QByteArray>
#include <QIcon>
#include <QString>

class QWidget;

namespace Utils {
QTCREATOR_UTILS_EXPORT QIcon settingsIcon(const QString &name);
QTCREATOR_UTILS_EXPORT void styleSettingsPage(QWidget *page);
QTCREATOR_UTILS_EXPORT QByteArray flatSvgDiagram(const QString &resource);
QTCREATOR_UTILS_EXPORT QByteArray flatSvgDiagram(const QString &resource, bool emphasizeRotation);
}

#endif
