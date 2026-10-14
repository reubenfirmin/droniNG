/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firmwaresettingsbackup.h"
#include <uavobjects/uavobjectmanager.h>
#include <manualcontrolsettings.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopedPointer>
#include <QSet>
#include <QUuid>
#include <QtEndian>
#include <cmath>

using namespace uploader;

namespace {
struct Layout { quint32 magic; int arena; };
// Reviewed against flight/targets/<target>/board-info/board_hw_defs.c.
QList<Layout> layouts(const QString &target)
{
    if (target == "revolution") return {{0x99abcedf, 0x20000}};
    if (target == "sparky2") return {{0x99abcedf, 0x4000}, {0x77abcedf, 0x10000}};
    if (target == "brain") return {{0x3bb141cf, 0x4000}};
    if (target == "seppuku") return {{0x3b1b14cf, 0x20000}};
    if (QStringList{"aq32", "quanton", "matek405"}.contains(target)) return {{0x3b1b14cf, 0x4000}};
    if (target == "playuavosd") return {{0x99abcedf, 0x4000}};
    if (QStringList{"lux", "pipxtreme", "pikoblx", "sprf3e", "sparky", "dtfc", "omnibusf3"}.contains(target))
        return {{0x9ae1ee11, 0x2000}};
    return {};
}
quint32 word(const QByteArray &data, int offset)
{
    if (offset < 0 || offset > data.size() - 4) return 0;
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + offset));
}
bool allErased(const QByteArray &data)
{
    for (char byte : data) if (uchar(byte) != 0xff) return false;
    return true;
}
}

FirmwareSettingsInspection FirmwareSettingsBackup::inspect(const QByteArray &partition,
    const QString &target, bool identifiedFirmware, UAVObjectManager *objects)
{
    FirmwareSettingsInspection result;
    result.reason = QObject::tr("The settings format is unknown. Its raw backup can be retained, but automatic preservation is unavailable.");
    if (partition.isEmpty() || partition.size() > 16 * 1024 * 1024) return result;
    if (allErased(partition)) { result.empty = true; result.compatible = true; return result; }
    if (!identifiedFirmware) {
        result.reason = QObject::tr("The source firmware's board identity could not be verified. Automatic settings preservation is unavailable.");
        return result;
    }
    int active = -1;
    Layout selected{};
    for (const auto &layout : layouts(target)) {
        if (partition.size() < layout.arena * 2 || partition.size() % layout.arena) continue;
        for (int offset = 0; offset < partition.size(); offset += layout.arena) {
            if (word(partition, offset) != layout.magic || word(partition, offset + 4) != 0xe6e66666) continue;
            if (active >= 0) {
                result.reason = QObject::tr("The settings partition has multiple active storage areas. Automatic preservation cannot be verified.");
                return result;
            }
            active = offset; selected = layout;
        }
    }
    if (active < 0 || !objects) return result;
    QSet<quint64> identities;
    QDomDocument document;
    auto root = document.createElement("uavobjects"); document.appendChild(root);
    auto settings = document.createElement("settings"); root.appendChild(settings);
    QByteArray converted = partition;
    QStringList conversions;
    bool reachedEmpty = false;
    for (int slot = active + 256; slot < active + selected.arena; slot += 256) {
        const quint32 state = word(partition, slot);
        if (state == 0xffffffff) {
            if (!allErased(partition.mid(slot, 256))) {
                result.reason = QObject::tr("The settings partition contains an incomplete record."); return result;
            }
            reachedEmpty = true; continue;
        }
        if (reachedEmpty || (state != 0 && state != 0xfafaffff && state != 0xfafaaaaa)) {
            result.reason = QObject::tr("The settings partition contains an invalid record state."); return result;
        }
        if (state != 0xfafaaaaa) continue; // Retired and unfinished writes are not live records.
        const quint32 sourceId = word(partition, slot + 4);
        quint32 id = sourceId;
        const auto *header = reinterpret_cast<const uchar *>(partition.constData() + slot);
        const quint16 instance = qFromLittleEndian<quint16>(header + 8);
        const quint16 size = qFromLittleEndian<quint16>(header + 10);
        // Release-20180729, upstream 4df9abe5a7c52ce162c9f40821b075149050c032:
        // ManualControlSettings changed only by appending IBus to RssiType.
        // The 123-byte layout and all nine existing enum values are unchanged.
        // Pin both schema IDs; a future destination change must be reviewed again.
        const bool legacyManualControl = sourceId == 0x884ed0e6
            && ManualControlSettings::OBJID == 0x3c35be0e && size == 123 && instance == 0;
        if (legacyManualControl) id = ManualControlSettings::OBJID;
        auto *object = objects->getObject(id, instance);
        auto *data = qobject_cast<UAVDataObject *>(object);
        const quint64 identity = (quint64(id) << 16) | instance;
        if (size > 244 || !object || size != object->getNumBytes() || identities.contains(identity)
            || (data && !data->isSettings()) || (!data && !(id & 1))) {
            result.reason = QObject::tr("Settings record 0x%1 (instance %2) does not match the packaged definitions. Automatic preservation cannot be verified.")
                .arg(sourceId, 8, 16, QLatin1Char('0')).arg(instance);
            result.records.clear(); return result;
        }
        identities.insert(identity);
        const QByteArray payload = partition.mid(slot + 12, size);
        result.records.append({id, instance, payload});
        if (!data) continue; // Metadata remains in the raw backup and is also verified on reconnect.
        QScopedPointer<UAVDataObject> copy(data->clone(instance));
        copy->unpack(reinterpret_cast<const quint8 *>(payload.constData()));
        if (legacyManualControl) {
            auto *manual = qobject_cast<ManualControlSettings *>(copy.data());
            if (!manual || manual->getData().RssiType > ManualControlSettings::RSSITYPE_TBSCROSSFIRE) {
                result.reason = QObject::tr("The saved receiver RSSI type is invalid for its original settings format.");
                result.records.clear(); return result;
            }
            qToLittleEndian(id, reinterpret_cast<uchar *>(converted.data() + slot + 4));
            conversions.append(QObject::tr("Receiver settings: preserve all values and update the record identifier for the added IBus RSSI option."));
        }
        auto element = document.createElement("object");
        element.setAttribute("name", copy->getName());
        element.setAttribute("id", QString("0x%1").arg(id, 0, 16));
        element.setAttribute("instance", instance);
        for (auto *field : copy->getFields()) {
            auto entry = document.createElement("field"); QStringList values;
            for (int index = 0; index < field->getNumElements(); ++index) {
                const auto value = field->getValue(index);
                if ((field->getType() == UAVObjectField::ENUM && !field->getOptions().contains(value.toString()))
                    || (field->getType() == UAVObjectField::FLOAT32 && !std::isfinite(value.toFloat()))) {
                    result.reason = QObject::tr("%1 contains an invalid value for %2. Automatic preservation cannot be verified.").arg(copy->getName(), field->getName());
                    result.records.clear(); return result;
                }
                values.append(field->getType() == UAVObjectField::FLOAT32
                    ? QString::number(value.toDouble(), 'g', 9) : value.toString());
            }
            entry.setAttribute("name", field->getName()); entry.setAttribute("values", values.join(','));
            element.appendChild(entry);
        }
        settings.appendChild(element);
    }
    result.compatible = true;
    result.empty = result.records.isEmpty();
    result.xml = document.toByteArray(2);
    result.conversions = conversions;
    if (!conversions.isEmpty()) result.convertedPartition = converted;
    result.reason.clear();
    return result;
}

QString FirmwareSettingsBackup::save(const QString &root, const QByteArray &partition,
    const QJsonObject &provenance, const QByteArray &xml, QString *error)
{
    const QString directory = QDir(root).filePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")
        + "-" + QUuid::createUuid().toString(QUuid::Id128));
    if (partition.isEmpty() || !QDir().mkpath(directory)) {
        *error = QObject::tr("Cannot create the settings backup directory: %1").arg(directory); return {};
    }
    QJsonObject manifest = provenance;
    manifest["schemaVersion"] = 1;
    manifest["capturedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    manifest["size"] = partition.size();
    manifest["sha256"] = QString::fromLatin1(QCryptographicHash::hash(partition, QCryptographicHash::Sha256).toHex());
    const QList<QPair<QString, QByteArray>> files{{"settings.bin", partition},
        {"backup.json", QJsonDocument(manifest).toJson()}, {"settings.uav", xml}};
    for (const auto &entry : files) {
        if (entry.second.isEmpty()) continue;
        QSaveFile output(QDir(directory).filePath(entry.first));
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly) || output.write(entry.second) != entry.second.size() || !output.commit()) {
            *error = QObject::tr("Cannot save the settings backup: %1. Check free space and folder permissions, then retry.").arg(output.errorString());
            return {};
        }
    }
    return directory;
}
