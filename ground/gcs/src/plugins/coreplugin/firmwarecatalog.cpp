/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firmwarecatalog.h"
#include "coreconstants.h"
#include "firmwareinstallimage.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QtEndian>

using namespace Core;

const FirmwareCatalog &FirmwareCatalog::instance()
{
    static const FirmwareCatalog catalog = [] {
        QFile file(":/core/firmware/board-catalog.json");
        if (!file.open(QIODevice::ReadOnly)) {
            FirmwareCatalog result;
            result.m_error = QStringLiteral("Cannot read the firmware catalog: %1").arg(file.errorString());
            return result;
        }
        return fromJson(file.readAll());
    }();
    return catalog;
}

FirmwareCatalog FirmwareCatalog::fromJson(const QByteArray &data)
{
    FirmwareCatalog result;
    const auto fail = [](const QString &error) {
        FirmwareCatalog invalid;
        invalid.m_error = error;
        return invalid;
    };
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("Invalid firmware catalog JSON: %1").arg(parseError.errorString()));
    const auto root = document.object();
    if (root.value("schemaVersion").toInt() != 1)
        return fail(QStringLiteral("Unsupported firmware catalog schema"));
    result.m_version = root.value("catalogVersion").toString();
    if (result.m_version.isEmpty() || !root.value("boards").isArray() || root.value("boards").toArray().isEmpty())
        return fail(QStringLiteral("Firmware catalog has no version or boards"));
    QSet<QString> ids, names, nativeTargets;
    const QRegularExpression identifier("^[a-z0-9][a-z0-9-]*$");
    const auto strings = [](const QJsonValue &value, QStringList *out) {
        if (value.isUndefined())
            return true;
        if (!value.isArray())
            return false;
        for (const auto &item : value.toArray()) {
            if (!item.isString() || item.toString().trimmed().isEmpty())
                return false;
            out->append(item.toString().trimmed());
        }
        return true;
    };
    for (const auto &value : root.value("boards").toArray()) {
        const auto object = value.toObject();
        FirmwareBoardProfile board;
        board.id = object.value("id").toString();
        board.name = object.value("name").toString();
        board.target = object.value("target").toString();
        board.mapping = object.value("mapping").toString();
        board.notes = object.value("notes").toString();
        board.evidence = object.value("evidence").toString();
        board.systemBootloader = object.value("systemBootloader").toString();
        board.bootInstructions = object.value("bootInstructions").toString();
        board.manualUrl = object.value("manualUrl").toString();
        board.settings = object.value("settings").toArray();
        board.boardType = object.value("boardType").toInt(-1);
        board.boardRevision = object.value("boardRevision").toInt(-1);
        if (!identifier.match(board.id).hasMatch() || !identifier.match(board.target).hasMatch()
            || board.name.trimmed().isEmpty() || board.evidence.isEmpty()
            || board.boardType < 1 || board.boardType > 255 || board.boardRevision < 0 || board.boardRevision > 255
            || (board.mapping != "native" && board.mapping != "compatible")
            || (board.mapping == "native" && board.id != board.target)
            || !strings(object.value("aliases"), &board.aliases)
            || !strings(object.value("mspTargets"), &board.mspTargets)
            || (object.contains("settings") && !object.value("settings").isArray())
            || (object.contains("systemBootloader") && (!object.value("systemBootloader").isString()
                || board.systemBootloader.trimmed().isEmpty()))
            || (object.contains("bootInstructions") && (!object.value("bootInstructions").isString()
                || board.bootInstructions.trimmed().isEmpty()))
            || (object.contains("manualUrl") && (!object.value("manualUrl").isString()
                || !QUrl(board.manualUrl).isValid() || QUrl(board.manualUrl).scheme() != "https"
                || QUrl(board.manualUrl).host().isEmpty()))
            || ids.contains(board.id))
            return fail(QStringLiteral("Invalid or duplicate firmware board: %1").arg(board.id));
        QSet<QString> settings;
        for (const auto &value : board.settings) {
            const auto setting = value.toObject();
            const auto data = setting.value("value");
            const QString key = setting.value("object").toString() + '/' + setting.value("field").toString()
                + '/' + setting.value("element").toString();
            if (!value.isObject() || setting.value("object").toString().isEmpty()
                || setting.value("field").toString().isEmpty()
                || (setting.contains("element") && !setting.value("element").isString())
                || (!data.isString() && !data.isDouble()) || settings.contains(key))
                return fail(QStringLiteral("Invalid board settings: %1").arg(board.id));
            settings.insert(key);
        }
        ids.insert(board.id);
        QStringList keys = board.aliases;
        keys << board.id << board.name;
        QSet<QString> localNames;
        for (const auto &key : keys)
            localNames.insert(key.trimmed().toCaseFolded());
        for (const auto &key : localNames) {
            if (names.contains(key))
                return fail(QStringLiteral("Ambiguous firmware board name: %1").arg(key));
            names.insert(key);
        }
        if (board.mapping == "native")
            nativeTargets.insert(board.target);
        result.m_boards.append(board);
    }
    for (const auto &board : result.m_boards) {
        if (!nativeTargets.contains(board.target))
            return fail(QStringLiteral("Unknown firmware target: %1").arg(board.target));
        // Aliases must inherit the actual firmware descriptor, not invent an ID.
        for (const auto &native : result.m_boards) {
            if (native.mapping == "native" && native.target == board.target
                && (native.boardType != board.boardType || native.boardRevision != board.boardRevision))
                return fail(QStringLiteral("Conflicting firmware identity: %1").arg(board.id));
        }
    }
    return result;
}

FirmwareBoardProfile FirmwareCatalog::board(const QString &idOrName) const
{
    const QString name = idOrName.trimmed();
    for (const auto &board : m_boards) {
        QStringList names = board.aliases;
        names << board.id << board.name;
        if (names.contains(name, Qt::CaseInsensitive))
            return board;
    }
    return {};
}

QList<FirmwareBoardProfile> FirmwareCatalog::candidatesForMspTarget(const QString &target) const
{
    QList<FirmwareBoardProfile> candidates;
    if (target.trimmed().isEmpty())
        return candidates;
    for (const auto &board : m_boards) {
        if (board.mspTargets.contains(target.trimmed(), Qt::CaseInsensitive))
            candidates.append(board);
    }
    return candidates; // A shared MSP target must not become a unique hardware identity.
}

QString FirmwareCatalog::imageName(const FirmwareBoardProfile &board, ImageKind kind)
{
    if (!board.isValid() || !QRegularExpression("^[a-z0-9][a-z0-9-]*$").match(board.target).hasMatch())
        return {};
    switch (kind) {
    case ImageKind::Application: return QString("fw_%1.tlfw").arg(board.target);
    case ImageKind::BootloaderUpdate: return QString("bu_%1.tlfw").arg(board.target);
    case ImageKind::FullInstall: return QString("ef_%1.hex").arg(board.target);
    }
    return {};
}

QString FirmwareCatalog::imagePath(const FirmwareBoardProfile &board, ImageKind kind, const QStringList &roots)
{
    const QString name = imageName(board, kind);
    if (name.isEmpty())
        return {};
    const QString directory = name.left(name.lastIndexOf('.'));
    for (const auto &root : roots) {
        for (const auto &relative : {directory + '/' + name, name}) {
            const QFileInfo file(QDir(root).filePath(relative));
            if (file.isFile() && file.isReadable() && file.size() > 0)
                return file.absoluteFilePath();
        }
    }
    return {}; // Presence is separate from a compatibility mapping; contents need installer validation.
}

QStringList FirmwareCatalog::defaultImageRoots()
{
    const QDir application(QCoreApplication::applicationDirPath());
    QStringList roots{application.absolutePath(), application.absoluteFilePath("../firmware")};
    QDir ancestor = application;
    // Find build artifacts in nested Qt build trees as well as a portable package.
    for (int i = 0; i < 6; ++i) {
        roots << ancestor.absolutePath() << ancestor.absoluteFilePath("build") << ancestor.absoluteFilePath("firmware");
        if (!ancestor.cdUp())
            break;
    }
    roots << QStringLiteral("/usr/local/" GCS_PROJECT_BRANDING_PRETTY "/firmware");
    roots.removeDuplicates();
    return roots;
}

QString FirmwareCatalog::firmwareName(const QByteArray &descriptor, const QStringList &roots)
{
    if (descriptor.size() != 100 || (!descriptor.startsWith("TlFw") && !descriptor.startsWith("OpFw")))
        return {};
    const auto tag = [](const QByteArray &description) {
        return QString::fromLatin1(description.mid(14, 26).split('\0').first());
    };
    const QRegularExpression release("^DroniNG [0-9]+\\.[0-9]+(?:\\.[0-9]+)?$");
    const QString name = tag(descriptor);
    if (release.match(name).hasMatch()) return name;

    // Earlier builds used a Git branch label. Recognize their product release
    // only when the executable, protocol, board and commit exactly match the
    // verified installation image shipped with this application.
    for (const auto &board : instance().boards()) {
        if (board.mapping != "native" || board.boardType != quint8(descriptor[12])
            || board.boardRevision != quint8(descriptor[13])) continue;
        const auto image = FirmwareInstallImage::load(board, imagePath(board, ImageKind::FullInstall, roots));
        if (image.valid() && image.descriptor.mid(4, 4) == descriptor.mid(4, 4)
            && image.descriptor.mid(40, 40) == descriptor.mid(40, 40)
            && release.match(tag(image.descriptor)).hasMatch())
            return tag(image.descriptor);
    }
    const QString revision = QString::number(qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(descriptor.constData() + 4)), 16).rightJustified(8, '0');
    return QStringLiteral("dRonin %1").arg(name.isEmpty() ? revision : name);
}
