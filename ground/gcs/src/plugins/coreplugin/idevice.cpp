/**
 ******************************************************************************
 *
 * @file       idevice.cpp
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2012-2013
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup CorePlugin Core Plugin
 * @{
 * @brief The Core GCS plugin
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */
#include "idevice.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include "firmwarecatalog.h"

bool Core::IDevice::plausibleControllerUsb(quint16 vendor, quint16 product, const QString &description)
{
    // A candidate is eligible for a read-only firmware probe, not an image mapping.
    // STM32 CDC is shared by Betaflight/Cleanflight controllers and development boards.
    if (vendor == 0x0483 && product == 0x5740) return true;
    if ((vendor == 0x20a0 && (product == 0x4250 || product == 0x415b || product == 0x415d
            || product == 0x415e || product == 0x4242 || product == 0x41d0 || product == 0x4284))
        || (vendor == 0x0fda && product == 0x0100) || (vendor == 0x1209 && product == 0xf3fc)
        || (vendor == 0x16d0 && product == 0xd7fc)) return true;
    if (!vendor || !product) return false;
    static const QRegularExpression family("\\b(?:betaflight|cleanflight|inav|droning?|openpilot|taulabs|flight[ -]?controller|omnibus[[:alnum:]]*)\\b",
                                           QRegularExpression::CaseInsensitiveOption);
    if (family.match(description).hasMatch()) return true;
    for (const auto &board : FirmwareCatalog::instance().boards()) {
        QStringList names = board.aliases; names.prepend(board.name);
        for (const auto &name : names) {
            const QRegularExpression model("(?:^|\\W)" + QRegularExpression::escape(name) + "(?:$|\\W)",
                                           QRegularExpression::CaseInsensitiveOption);
            if (model.match(description).hasMatch()) return true;
        }
    }
    return false;
}

QString Core::IDevice::usbTopologyForDeviceNode(const QString &node)
{
#ifdef Q_OS_LINUX
    const QString name = QFileInfo(node).fileName();
    const QString subsystem = name.startsWith("hidraw") ? "hidraw" : "tty";
    QDir device(QFileInfo(QString("/sys/class/%1/%2/device").arg(subsystem, name)).canonicalFilePath());
    if (device.path().isEmpty() || device.path() == ".") return {};
    for (int i = 0; i < 12; ++i) {
        if (QFileInfo::exists(device.filePath("idVendor")) && QFileInfo::exists(device.filePath("idProduct"))
            && QRegularExpression("^[0-9]+-[0-9]+(?:\\.[0-9]+)*$").match(device.dirName()).hasMatch())
            return device.dirName();
        if (!device.cdUp()) break;
    }
#else
    Q_UNUSED(node);
#endif
    return {};
}
