/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firmwareinstallimage.h"
#include "gcsversioninfo.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtEndian>

using namespace Core;

QByteArray FirmwareInstallImage::gcsUavoHash()
{
    static const unsigned char hash[] = UAVO_HASH;
    return QByteArray(reinterpret_cast<const char *>(hash), sizeof(hash));
}

QJsonObject FirmwareInstallImage::importedProfile(const QString &id)
{
    if (id != "radix" && id != "omnibusf4sd") return {};
    QFile file(QString(":/core/firmware/profiles/%1.json").arg(id));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto profile = QJsonDocument::fromJson(file.readAll()).object();
    const int flash = id == "radix" ? 524288 : 1048576;
    const QString layout = id == "radix" ? "radix-f446-rom-v1" : "omnibusf4sd-f405-rom-v1";
    if (profile.value("schemaVersion").toInt() != 1 || profile.value("id").toString() != id
        || profile.value("boardType").toInt() != (id == "radix" ? 224 : 225)
        || profile.value("boardRevision").toInt() != 1 || profile.value("flashBytes").toInt() != flash
        || profile.value("ramBytes").toInt() != 131072 || profile.value("layoutId").toString() != layout)
        return {};
    return profile;
}

QByteArray FirmwareInstallImage::importedProfileHash(const QString &id)
{
    if (importedProfile(id).isEmpty()) return {};
    QByteArray inputs;
    for (const auto &path : {QString(":/core/firmware/profiles/%1.json").arg(id), QString(":/core/firmware/%1-source-manifest.json").arg(id)}) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        inputs += file.readAll();
    }
    return QCryptographicHash::hash(inputs, QCryptographicHash::Sha256);
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
    if (!board.layoutId.isEmpty()) {
        const auto profile = importedProfile(board.id);
        const int flashSize = profile.value("flashBytes").toInt();
        const int descriptorOffset = flashSize - 100;
        const QList<Segment> ranges{{0, 16384}, {49152, 16384}, {131072, quint32(qMax(0, flashSize - 131072))}};
        const auto hash = importedProfileHash(board.id);
        if (profile.isEmpty() || board.layoutId != profile.value("layoutId").toString()
            || board.boardType != profile.value("boardType").toInt() || board.boardRevision != 1
            || manifest.value("layoutId").toString() != board.layoutId
            || manifest.value("profileId").toString() != board.id
            || manifest.value("supportLevel").toString() != "experimental"
            || manifest.value("profileSha256").toString().toLatin1() != hash.toHex() || hash.size() != 32
            || base != 0x08000000 || size != flashSize || size != result.bytes.size()
            || offset != 0 || fwSize < 131080 || fwSize > descriptorOffset || desc != descriptorOffset
            || manifest.value("identityOffset").toInt() != 49152
            || QCryptographicHash::hash(result.bytes, QCryptographicHash::Sha256).toHex() != image.value("sha256").toString().toLatin1())
            return fail(QObject::tr("The imported firmware profile, layout or image integrity does not match this GCS build."));
        const auto segments = manifest.value("programSegments").toArray();
        if (segments.size() != ranges.size()) return fail(QObject::tr("Invalid imported firmware write regions."));
        for (int i = 0; i < ranges.size(); ++i) {
            const auto region = segments.at(i).toObject();
            if (region.value("offset").toInt(-1) != int(ranges[i].offset)
                || region.value("size").toInt(-1) != int(ranges[i].size))
                return fail(QObject::tr("The image would overwrite reserved storage. No firmware was written."));
        }
        const auto word = [&result](int position) {
            return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(result.bytes.constData() + position));
        };
        const auto rawHash = QCryptographicHash::hash(result.bytes.left(fwSize), QCryptographicHash::Sha1);
        result.descriptor = result.bytes.mid(desc, 100);
        if (result.descriptor.left(4) != "TlFw" || quint8(result.descriptor.at(12)) != board.boardType
            || quint8(result.descriptor.at(13)) != board.boardRevision
            || result.descriptor.mid(40, 20) != rawHash || rawHash.toHex() != firmware.value("sha1").toString().toLatin1()
            || result.descriptor.mid(60, 20) != gcsUavoHash()
            || firmware.value("uavoSha1").toString().toLatin1() != gcsUavoHash().toHex()
            || word(49152) != 0xbdbdbdbd || quint8(result.bytes.at(49156)) != board.boardType
            || quint8(result.bytes.at(49157)) != 1 || result.bytes.at(49158) != 0 || result.bytes.at(49159) != 1
            || word(49160) != base || word(49164) != quint32(descriptorOffset) || word(49168) != base + desc
            || word(49172) != 100 || word(49176) != 0x08004000 || word(49180) != 32768
            || result.bytes.mid(49184, 4) != "BPrf" || word(49188) != 1
            || result.bytes.mid(49192, 32) != board.id.toLatin1().leftJustified(32, '\0')
            || result.bytes.mid(49224, 32) != board.layoutId.toLatin1().leftJustified(32, '\0')
            || result.bytes.mid(49256, 32) != hash
            || word(0) <= 0x20000000 || word(0) > 0x20020000 || (word(0) & 7)
            || !(word(4) & 1) || word(4) < 0x08020000 || word(4) >= base + fwSize)
            return fail(QObject::tr("The imported image has an invalid board identity, protocol or direct-boot vector."));
        result.base = quint32(base); result.segments = ranges; result.romBoot = true;
        result.profileId = board.id; result.layoutId = board.layoutId; result.profileHash = hash;
        return result;
    }
    if (manifest.contains("layoutId") || manifest.contains("programSegments"))
        return fail(QObject::tr("This board does not accept an imported firmware layout."));
    if (base != 0x08000000 || size != result.bytes.size() || size > 2 * 1024 * 1024
        || offset < 8 || fwSize < 8 || offset + fwSize + 100 > desc || desc + 100 != size
        || blSize < 8 || blSize > offset || info < 8 || info + 32 > blSize
        || QCryptographicHash::hash(result.bytes, QCryptographicHash::Sha256).toHex() != image.value("sha256").toString().toLatin1())
        return fail(QObject::tr("Installation image integrity or flash bounds are invalid."));
    result.base = quint32(base);
    result.segments = {{0, quint32(size)}};
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
