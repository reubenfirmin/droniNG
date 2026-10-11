/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firmwareinstallimage.h"
#include "gcsversioninfo.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

using namespace Core;

QByteArray FirmwareInstallImage::gcsUavoHash()
{
    static const unsigned char hash[] = UAVO_HASH;
    return QByteArray(reinterpret_cast<const char *>(hash), sizeof(hash));
}

FirmwareInstallImage FirmwareInstallImage::load(const FirmwareBoardProfile &board, const QString &hexPath)
{
    FirmwareInstallImage result;
    const auto fail = [](const QString &error) {
        FirmwareInstallImage image;
        image.error = error;
        return image;
    };
    if (!board.isValid() || QFileInfo(hexPath).fileName() != FirmwareCatalog::imageName(board, FirmwareCatalog::ImageKind::FullInstall))
        return fail(QObject::tr("Select a catalog board and its full installation image."));
    const QDir directory = QFileInfo(hexPath).absoluteDir();
    QFile manifestFile(directory.filePath(QString("ef_%1.manifest.json").arg(board.target)));
    if (!manifestFile.open(QIODevice::ReadOnly) || manifestFile.size() > 16384)
        return fail(QObject::tr("The installation image has no readable build manifest. Rebuild the firmware installation bundle."));
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(manifestFile.readAll(), &parse);
    const auto manifest = document.object();
    if (parse.error != QJsonParseError::NoError || manifest.value("schemaVersion").toInt() != 1
        || manifest.value("target").toString() != board.target
        || manifest.value("boardType").toInt(-1) != board.boardType
        || manifest.value("boardRevision").toInt(-1) != board.boardRevision)
        return fail(QObject::tr("The installation manifest does not match the selected board."));
    const auto image = manifest.value("image").toObject();
    const auto firmware = manifest.value("firmware").toObject();
    const auto bootloader = manifest.value("bootloader").toObject();
    const QString name = QString("ef_%1.bin").arg(board.target);
    if (image.value("file").toString() != name)
        return fail(QObject::tr("Invalid installation image filename."));
    QFile file(directory.filePath(name));
    if (!file.open(QIODevice::ReadOnly) || file.size() < 1024 || file.size() > 2 * 1024 * 1024)
        return fail(QObject::tr("The full installation binary is missing or has an invalid size."));
    result.bytes = file.readAll();
    result.target = board.target;
    const qint64 base = image.value("base").toVariant().toLongLong();
    const qint64 size = image.value("size").toVariant().toLongLong();
    const qint64 offset = firmware.value("offset").toInt(-1);
    const qint64 fwSize = firmware.value("size").toInt(-1);
    const qint64 desc = firmware.value("descriptorOffset").toInt(-1);
    const qint64 info = bootloader.value("infoOffset").toInt(-1);
    const qint64 blSize = bootloader.value("size").toInt(-1);
    if (base != 0x08000000 || size != result.bytes.size() || size > 2 * 1024 * 1024
        || offset < 8 || fwSize < 8 || offset + fwSize + 100 > desc || desc + 100 != size
        || blSize < 8 || blSize > offset || info < 8 || info + 32 > blSize
        || QCryptographicHash::hash(result.bytes, QCryptographicHash::Sha256).toHex() != image.value("sha256").toString().toLatin1())
        return fail(QObject::tr("Installation image integrity or flash bounds are invalid."));
    result.base = quint32(base);
    result.descriptor = result.bytes.mid(desc, 100);
    const auto hash = QCryptographicHash::hash(result.bytes.mid(offset, fwSize), QCryptographicHash::Sha1);
    if (result.descriptor.left(4) != "TlFw" || quint8(result.descriptor.at(12)) != board.boardType
        || quint8(result.descriptor.at(13)) != board.boardRevision || result.descriptor.mid(40, 20) != hash
        || hash.toHex() != firmware.value("sha1").toString().toLatin1()
        || result.descriptor.mid(60, 20) != gcsUavoHash()
        || gcsUavoHash().toHex() != firmware.value("uavoSha1").toString().toLatin1()
        || result.bytes.mid(offset + fwSize, 100) != result.descriptor)
        return fail(QObject::tr("The firmware descriptor, board identity, or UAVObjects do not match this GCS build."));
    const auto word = [&result](qint64 position) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(result.bytes.constData() + position));
    };
    if (word(info) != 0xbdbdbdbd || quint8(result.bytes.at(info + 4)) != board.boardType
        || quint8(result.bytes.at(info + 5)) != board.boardRevision
        || word(info + 8) != base + offset || word(info + 12) != desc - offset
        || word(info + 16) != base + desc || word(info + 20) != 100)
        return fail(QObject::tr("The bootloader does not describe the selected firmware layout."));
    // Both executable vector tables must point into RAM and their own image regions.
    for (const auto vector : {qint64(0), offset}) {
        const quint32 stack = word(vector), reset = word(vector + 4);
        const qint64 end = vector == 0 ? blSize : offset + fwSize;
        const bool ram = (stack > 0x20000000 && stack <= 0x20040000)
            || (stack > 0x10000000 && stack <= 0x10010000); // STM32 CCM RAM may hold the stack.
        if (!ram || (stack & 3)
            || !(reset & 1) || reset < base + vector || reset >= base + end)
            return fail(QObject::tr("The installation image contains an invalid boot vector."));
    }
    return result;
}
