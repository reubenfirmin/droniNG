/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "romuart.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtEndian>

using namespace Core;
namespace {
constexpr quint32 base = 0x08000000;
constexpr int flashSize = 262144, pageSize = 2048;
QByteArray octets(std::initializer_list<int> values)
{
    QByteArray bytes;
    for (int value : values) bytes.append(char(value));
    return bytes;
}
QByteArray checksum(QByteArray bytes)
{
    quint8 value = 0;
    for (auto byte : bytes) value ^= quint8(byte);
    bytes.append(char(value));
    return bytes;
}
QByteArray sha(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
bool save(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
}
bool RomUart::transmit(const QByteArray &bytes)
{
    return send(bytes) || fail(QObject::tr("The serial write failed. Keep BOOT connected, reconnect USB to the same port and retry. Any saved backup remains available."));
}
bool RomUart::synchronize()
{
    error.clear(); identified = false;
    return transmit(QByteArray(1, char(0x7f))) && ack(700);
}
bool RomUart::ack(int timeout)
{
    const auto data = receive(1, timeout);
    if (data == QByteArray(1, char(0x79))) return true;
    return fail(data == QByteArray(1, char(0x1f))
        ? QObject::tr("The STM32 bootloader rejected the command. Keep BOOT connected, power-cycle USB and retry. Flash protection is left unchanged so existing data can be preserved.")
        : QObject::tr("The STM32 bootloader stopped responding. Keep BOOT connected, reconnect USB to the same port and retry."));
}
bool RomUart::command(quint8 value)
{
    return transmit(octets({value, value ^ 0xff})) && ack();
}
bool RomUart::address(quint32 value)
{
    QByteArray data(4, char(0)); qToBigEndian(value, data.data());
    return transmit(checksum(data)) && ack();
}
bool RomUart::read(quint32 value, int size, QByteArray &data)
{
    if (size < 1 || size > 256) return fail(QObject::tr("Invalid STM32 read size."));
    if (!command(0x11) || !address(value)
        || !transmit(octets({size - 1, (size - 1) ^ 0xff})) || !ack()) return false;
    data = receive(size, 2000);
    return data.size() == size || fail(QObject::tr("The flash read was incomplete. Reconnect in BOOT mode and retry."));
}
bool RomUart::identify()
{
    identified = false;
    if (!command(0x00)) return false;
    auto count = receive(1, 1500);
    if (count.size() != 1 || quint8(count[0]) < 3 || quint8(count[0]) > 32)
        return fail(QObject::tr("Invalid STM32 bootloader command list."));
    auto commands = receive(quint8(count[0]) + 1, 1500);
    if (commands.size() != quint8(count[0]) + 1) return fail(QObject::tr("The STM32 command list was incomplete. Reconnect in BOOT mode and retry."));
    if (!ack()) return false;
    commands.remove(0, 1); // bootloader version
    extendedErase = commands.contains(char(0x44));
    for (int required : {0x02, 0x11, 0x31})
        if (!commands.contains(char(required))) return fail(QObject::tr("The STM32 bootloader lacks a required command."));
    if (!extendedErase && !commands.contains(char(0x43))) return fail(QObject::tr("The bootloader cannot erase individual flash pages."));
    if (!command(0x02)) return false;
    count = receive(1, 1500);
    if (count != QByteArray(1, char(1))) return fail(QObject::tr("Unexpected STM32 device identifier length."));
    auto pid = receive(2, 1500);
    if (!ack()) return false;
    if (pid != QByteArray::fromHex("0422")) return fail(QObject::tr("This is not the STM32F303xB/C required by SP Racing F3. No flash was changed."));
    QByteArray geometry;
    if (!read(0x1ffff7cc, 2, geometry) || !read(0x1ffff7ac, 12, uid)) return false;
    if (qFromLittleEndian<quint16>(geometry.constData()) != 256
        || uid == QByteArray(12, char(0)) || uid == QByteArray(12, char(0xff)))
        return fail(QObject::tr("The MCU flash size or unique identity does not match the reviewed 256 KiB SP Racing F3. No flash was changed."));
    identified = true;
    return true;
}
bool RomUart::erase(int page)
{
    if (!identified || page < 0 || page >= flashSize / pageSize)
        return fail(QObject::tr("Flash page is outside the reviewed layout."));
    QByteArray data;
    if (extendedErase) data = octets({0, 0, page >> 8, page});
    else data = octets({0, page});
    return command(extendedErase ? 0x44 : 0x43) && transmit(checksum(data)) && ack(10000);
}
bool RomUart::write(quint32 value, const QByteArray &data)
{
    if (!identified || value < base || quint64(value) + data.size() > base + flashSize
        || value % 4 || data.isEmpty() || data.size() > 256 || data.size() % 4)
        return fail(QObject::tr("Flash write is outside the reviewed layout."));
    return command(0x31) && address(value)
        && transmit(checksum(QByteArray(1, char(data.size() - 1)) + data)) && ack(3000);
}
bool RomUart::snapshot(const QString &root, const Progress &progress)
{
    QByteArray bytes, block;
    for (int pass = 0; pass < 2; ++pass) {
        for (int offset = 0; offset < flashSize; offset += 256) {
            if (!read(base + offset, 256, block)) return false;
            if (!pass) bytes.append(block);
            else if (bytes.mid(offset, 256) != block)
                return fail(QObject::tr("Two backup reads differ. No flash was changed. Reconnect in BOOT mode and retry."));
            progress(QObject::tr("Backing up existing firmware and settings"), (pass * flashSize + offset + 256) * 100 / (2 * flashSize));
        }
    }
    const auto directory = QDir(root).filePath(QString::fromLatin1(uid.toHex()) + '/'
        + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz"));
    if (root.isEmpty() || !QDir().mkpath(directory)) return fail(QObject::tr("Cannot create the firmware backup folder. Free disk space and retry; no flash was changed."));
    const auto path = QDir(directory).filePath("flash.bin");
    const QJsonObject metadata{{"schemaVersion", 1}, {"profile", "spracingf3"}, {"uid", QString::fromLatin1(uid.toHex())},
        {"pid", 0x422}, {"flashBytes", flashSize}, {"sha256", QString::fromLatin1(sha(bytes))}};
    if (!save(path, bytes) || !save(path + ".json", QJsonDocument(metadata).toJson()))
        return fail(QObject::tr("Cannot save the complete backup. Free disk space and retry; no flash was changed."));
    QFile check(path), checkMetadata(path + ".json");
    if (!check.open(QIODevice::ReadOnly) || check.readAll() != bytes
        || !checkMetadata.open(QIODevice::ReadOnly)
        || QJsonDocument::fromJson(checkMetadata.readAll()).object() != metadata)
        return fail(QObject::tr("The saved backup could not be verified. No flash was changed."));
    backupPath = path;
    progress(QObject::tr("Backup saved and verified"), 100);
    return true;
}
bool RomUart::program(const QByteArray &bytes, const QList<FirmwareInstallImage::Segment> &segments, const Progress &progress)
{
    // Erase/write application and identity first, reset vectors last. Each page is
    // read back before continuing. No mass erase, unprotect or option-byte command.
    int total = 0, done = 0;
    for (auto segment : segments) {
        if (!segment.size || segment.offset % pageSize || segment.size % pageSize
            || quint64(segment.offset) + segment.size > flashSize || bytes.size() != flashSize)
            return fail(QObject::tr("Invalid flash programming range."));
        total += segment.size;
    }
    for (auto segment : segments) {
        for (quint32 page = segment.offset; page < segment.offset + segment.size; page += pageSize) {
            if (!erase(page / pageSize)) return false;
            const auto expected = bytes.mid(page, pageSize);
            for (int i = 0; i < pageSize; i += 256)
                if (expected.mid(i, 256) != QByteArray(256, char(0xff))
                    && !write(base + page + i, expected.mid(i, 256))) return false;
            QByteArray actual, block;
            for (int i = 0; i < pageSize; i += 256) {
                if (!read(base + page + i, 256, block)) return false;
                actual.append(block);
            }
            if (actual != expected) return fail(QObject::tr("Flash readback differs from the image. Keep BOOT connected and retry installation. Your saved backup is available below."));
            done += pageSize;
            progress(QObject::tr("Writing and verifying firmware"), done * 100 / total);
        }
    }
    return true;
}
bool RomUart::install(const FirmwareInstallImage &image, const QString &root, const Progress &progress)
{
    const QList<FirmwareInstallImage::Segment> expected{{0, 2048}, {2048, 2048}, {36864, 225280}};
    if (!identified || !image.valid() || image.profileId != "spracingf3" || image.base != base
        || image.bytes.size() != flashSize || image.segments.size() != expected.size())
        return fail(QObject::tr("The image is not a validated SP Racing F3 installation."));
    for (int i = 0; i < expected.size(); ++i)
        if (image.segments[i].offset != expected[i].offset || image.segments[i].size != expected[i].size)
            return fail(QObject::tr("The image would overwrite reserved settings."));
    if (!snapshot(root, progress)) return false;
    return program(image.bytes, {expected[2], expected[1], expected[0]}, progress);
}
bool RomUart::restore(const QString &path, const QString &root, const Progress &progress)
{
    QFile file(path), metadata(path + ".json");
    if (!identified || !file.open(QIODevice::ReadOnly) || file.size() != flashSize || !metadata.open(QIODevice::ReadOnly))
        return fail(QObject::tr("Select a complete flash.bin backup with its flash.bin.json identity file."));
    const auto bytes = file.readAll();
    const auto info = QJsonDocument::fromJson(metadata.readAll()).object();
    if (info.value("schemaVersion").toInt() != 1 || info.value("profile").toString() != "spracingf3"
        || info.value("pid").toInt() != 0x422 || info.value("flashBytes").toInt() != flashSize
        || info.value("uid").toString().toLatin1() != uid.toHex() || info.value("sha256").toString().toLatin1() != sha(bytes))
        return fail(QObject::tr("This backup is damaged or belongs to a different controller. No flash was changed."));
    if (!snapshot(root, progress)) return false;
    return program(bytes, {{2048, flashSize - 2048}, {0, 2048}}, progress);
}
