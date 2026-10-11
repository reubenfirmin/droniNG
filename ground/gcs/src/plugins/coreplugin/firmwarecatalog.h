/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FIRMWARECATALOG_H
#define FIRMWARECATALOG_H

#include "core_global.h"
#include <QList>
#include <QJsonArray>
#include <QStringList>

namespace Core {

struct CORE_EXPORT FirmwareBoardProfile {
    QString id, name, target, mapping, notes, evidence, systemBootloader, bootInstructions, manualUrl;
    QStringList aliases, mspTargets;
    QJsonArray settings;
    int boardType = 0;
    int boardRevision = 0;
    bool isValid() const { return !id.isEmpty(); }
};

class CORE_EXPORT FirmwareCatalog
{
public:
    enum class ImageKind { Application, BootloaderUpdate, FullInstall };
    static const FirmwareCatalog &instance();
    static FirmwareCatalog fromJson(const QByteArray &data);
    QString error() const { return m_error; }
    QString version() const { return m_version; }
    QList<FirmwareBoardProfile> boards() const { return m_boards; }
    FirmwareBoardProfile board(const QString &idOrName) const;
    QList<FirmwareBoardProfile> candidatesForMspTarget(const QString &target) const;
    static QString imageName(const FirmwareBoardProfile &board, ImageKind kind);
    static QString imagePath(const FirmwareBoardProfile &board, ImageKind kind,
                             const QStringList &roots = defaultImageRoots());
    static QStringList defaultImageRoots();
    static QString firmwareName(const QByteArray &descriptor,
                                const QStringList &roots = defaultImageRoots());

private:
    QList<FirmwareBoardProfile> m_boards;
    QString m_version, m_error;
};

} // namespace Core
#endif
