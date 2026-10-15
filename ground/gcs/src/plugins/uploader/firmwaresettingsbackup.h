/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FIRMWARESETTINGSBACKUP_H
#define FIRMWARESETTINGSBACKUP_H

#include "uploader_global.h"
#include <QByteArray>
#include <QList>
#include <QJsonObject>
#include <QStringList>

class UAVObjectManager;

namespace uploader {

struct FirmwareSettingRecord {
    quint32 id;
    quint16 instance;
    QByteArray data;
};

struct FirmwareSettingsInspection {
    bool compatible = false;
    bool empty = false;
    QString reason;
    QList<FirmwareSettingRecord> records;
    QByteArray xml;
    QByteArray convertedPartition;
    QStringList conversions;
};

class UPLOADER_EXPORT FirmwareSettingsBackup
{
public:
    // Identify the source board, then validate each record by its schema ID and size.
    static FirmwareSettingsInspection inspect(const QByteArray &partition, const QString &target,
                                              bool identifiedFirmware, UAVObjectManager *objects);
    // Raw data and its provenance are required; XML is an additional, portable backup.
    static QString save(const QString &root, const QByteArray &partition,
                        const QJsonObject &provenance, const QByteArray &xml, QString *error);
};

}
#endif
