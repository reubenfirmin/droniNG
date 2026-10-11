/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FIRMWAREINSTALLIMAGE_H
#define FIRMWAREINSTALLIMAGE_H
#include "firmwarecatalog.h"
#include <QByteArray>

namespace Core {
struct CORE_EXPORT FirmwareInstallImage {
    QString error, target;
    QByteArray bytes, descriptor;
    quint32 base = 0;
    static QByteArray gcsUavoHash();
    static FirmwareInstallImage load(const FirmwareBoardProfile &board, const QString &hexPath);
    bool valid() const { return error.isEmpty() && !bytes.isEmpty(); }
};
}
#endif
