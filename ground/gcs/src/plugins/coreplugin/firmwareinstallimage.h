/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FIRMWAREINSTALLIMAGE_H
#define FIRMWAREINSTALLIMAGE_H
#include "firmwarecatalog.h"
#include <QByteArray>
#include <QJsonObject>

namespace Core {
struct CORE_EXPORT FirmwareInstallImage {
    struct Segment { quint32 offset, size; };
    QString error, target;
    QString profileId, layoutId;
    QByteArray profileHash;
    QByteArray bytes, descriptor;
    QList<Segment> segments;
    bool romBoot = false;
    quint32 base = 0;
    static QByteArray gcsUavoHash();
    static QJsonObject importedProfile(const QString &id);
    static QByteArray importedProfileHash(const QString &id = QStringLiteral("radix"));
    static FirmwareInstallImage load(const FirmwareBoardProfile &board, const QString &hexPath);
    bool valid() const { return error.isEmpty() && !bytes.isEmpty(); }
};
}
#endif
