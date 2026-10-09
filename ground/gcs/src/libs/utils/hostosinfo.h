/**************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#ifndef HOSTOSINFO_H
#define HOSTOSINFO_H

#include "utils_global.h"

#include <QString>

namespace Utils {

class QTCREATOR_UTILS_EXPORT HostOsInfo
{
public:
    // Add more as needed.
    enum HostOs { HostOsLinux };
    static inline HostOs hostOs();

    enum HostArchitecture { HostArchitectureX86, HostArchitectureAMD64, HostArchitectureItanium,
                            HostArchitectureArm, HostArchitectureUnknown };
    static HostArchitecture hostArchitecture();

    static bool isLinuxHost() { return hostOs() == HostOsLinux; }
    static inline bool isAnyUnixHost();

    static QString appendExecutableSuffix(const QString &executable)
    {
        return executable;
    }

    static Qt::CaseSensitivity fileNameCaseSensitivity()
    {
        return Qt::CaseSensitive;
    }

    static QChar pathListSeparator()
    {
        return QLatin1Char(':');
    }

    static Qt::KeyboardModifier controlModifier()
    {
        return Qt::ControlModifier;
    }

};

HostOsInfo::HostOs HostOsInfo::hostOs()
{
    return HostOsLinux;
}

bool HostOsInfo::isAnyUnixHost()
{
    return true;
}

} // namespace Utils

#endif // HOSTOSINFO_H
