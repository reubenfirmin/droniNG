/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <coreplugin/dfuinstaller.h>
#include <coreplugin/betaflightbootcontroller.h>
#include <uploader/firmwarebootcontroller.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QtEndian>

using namespace Core;

struct Fixture {
    QTemporaryDir directory;
    FirmwareBoardProfile board = FirmwareCatalog::instance().board("hglrc-f4-v6pro");
    QByteArray image, descriptor;
    QJsonObject manifest;
    QString path() const { return directory.filePath("ef_revolution.hex"); }
    Fixture() {
        board.settings = {}; // Transport fixture; profile configuration is checked separately.
        image = QByteArray(524288, char(0xff));
        const auto word = [this](int offset, quint32 value) {
            qToLittleEndian(value, reinterpret_cast<uchar *>(image.data() + offset));
        };
        word(0, 0x20020000); word(4, 0x08000101);
        word(131072, 0x20020000); word(131076, 0x08020101);
        descriptor = QByteArray(100, 0);
        descriptor.replace(0, 4, "TlFw"); descriptor[12] = 9; descriptor[13] = 3;
        descriptor.replace(40, 20, QCryptographicHash::hash(image.mid(131072, 1024), QCryptographicHash::Sha1));
        descriptor.replace(60, 20, FirmwareInstallImage::gcsUavoHash());
        image.replace(132096, 100, descriptor); image.replace(524188, 100, descriptor);
        word(32640, 0xbdbdbdbd); image[32644] = 9; image[32645] = 3;
        word(32648, 0x08020000); word(32652, 393116); word(32656, 0x0807ff9c); word(32660, 100);
        manifest = {{"schemaVersion", 1}, {"target", "revolution"}, {"boardType", 9}, {"boardRevision", 3},
            {"image", QJsonObject{{"file", "ef_revolution.bin"}, {"base", 0x08000000}, {"size", 524288},
                {"sha256", QString::fromLatin1(QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex())}}},
            {"bootloader", QJsonObject{{"infoOffset", 32640}, {"size", 32768}}},
            {"firmware", QJsonObject{{"offset", 131072}, {"size", 1024}, {"descriptorOffset", 524188},
                {"sha1", QString::fromLatin1(descriptor.mid(40, 20).toHex())},
                {"uavoSha1", QString::fromLatin1(FirmwareInstallImage::gcsUavoHash().toHex())}}}};
        save();
    }
    void save(bool rehash = false) {
        if (rehash) {
            auto object = manifest.value("image").toObject();
            object["sha256"] = QString::fromLatin1(QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex());
            manifest["image"] = object;
        }
        for (const auto &file : {qMakePair(QString("ef_revolution.bin"), image),
                                qMakePair(QString("ef_revolution.manifest.json"), QJsonDocument(manifest).toJson()),
                                qMakePair(QString("ef_revolution.hex"), QByteArray(":00000001FF\n"))}) {
            QFile output(directory.filePath(file.first));
            QVERIFY(output.open(QIODevice::WriteOnly)); QCOMPARE(output.write(file.second), qint64(file.second.size()));
        }
    }
};

static QString listing(const QString &path = "1-3", const QString &layout = "@Internal Flash /0x08000000/04*016Kg,01*064Kg,07*128Kg")
{
    return QString("Found DFU: [0483:df11] ver=2200, devnum=8, cfg=1, intf=0, path=\"%1\", alt=0, name=\"%2\", serial=\"1234ABCD\"\n").arg(path, layout);
}
static QString radixListing()
{ return listing("1-3", "@Internal Flash /0x08000000/04*016Kg,01*064Kg,03*128Kg"); }

class FakeRunner : public DfuCommandRunner {
public:
    QString discovery = listing(), secondDiscovery, usbDescription;
    QList<QStringList> commands;
    QByteArray written;
    QByteArray flash = QByteArray(1048576, char(0x5a));
    QList<QPair<quint32, QByteArray>> writes;
    int listCount = 0;
    bool corruptRead = false, shortRead = false, writeFailure = false, leaveFailure = false;
    QStringList writeProgress, readProgress;
    void run(const QString &, const QStringList &arguments, int) override {
        commands.append(arguments);
        int code = 0;
        QString output;
        if (arguments.contains("-l")) {
            output = listCount++ > 0 && !secondDiscovery.isNull() ? secondDiscovery : discovery;
        } else if (arguments.contains("-D")) {
            for (const auto &chunk : writeProgress) emit this->output(chunk);
            QFile file(arguments[arguments.indexOf("-D") + 1]);
            QVERIFY(file.open(QIODevice::ReadOnly)); written = file.readAll();
            code = writeFailure ? 1 : 0;
            const quint32 address = arguments[arguments.indexOf("-s") + 1].section(':', 0, 0).toUInt(nullptr, 0);
            QVERIFY(address >= 0x08000000);
            const quint32 offset = address - 0x08000000;
            QVERIFY(offset + written.size() <= flash.size());
            if (!writeFailure) { flash.replace(offset, written.size(), written); writes.append({offset, written}); }
        } else if (arguments.contains("-U")) {
            const bool leaving = arguments.join(' ').contains(":leave");
            if (!leaving) for (const auto &chunk : readProgress) emit this->output(chunk);
            const auto range = arguments[arguments.indexOf("-s") + 1].split(':');
            const quint32 address = range[0].toUInt(nullptr, 0);
            const int size = range[1].toInt();
            QByteArray bytes = flash.mid(address - 0x08000000, size);
            if (!leaving && corruptRead) bytes[20] = char(bytes[20] ^ 1);
            if (!leaving && shortRead) bytes.chop(1);
            QFile file(arguments[arguments.indexOf("-U") + 1]);
            QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size()));
            code = leaving && leaveFailure ? 1 : 0;
        }
        QTimer::singleShot(0, this, [this, code, output] { emit finished(code, output); });
    }
    void cancel() override {}
    QString connectedUsbDevice(const QString &) const override { return usbDescription; }
};

class FakeBootTransport final : public uploader::FirmwareBootTransport {
public:
    tl_dfu::device caps{};
    QByteArray descriptor;
    bool opens = true, starts = true;
    int startCount = 0;
    std::function<void()> afterDescription;
    bool open(const USBPortInfo &) override { return opens; }
    tl_dfu::device capabilities() override { return caps; }
    QByteArray description(int size) override {
        if (afterDescription) afterDescription();
        return descriptor.left(size);
    }
    bool start() override { ++startCount; return starts; }
};

class BetaflightDevice final : public QIODevice {
public:
    QByteArray incoming;
    QList<QByteArray> writes;
    bool shortWrite = false, delayedWrite = false;
    qint64 queued = 0;
    BetaflightDevice() { open(QIODevice::ReadWrite); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return incoming.size() + QIODevice::bytesAvailable(); }
    qint64 bytesToWrite() const override { return queued; }
    void feed(const QByteArray &bytes) { incoming += bytes; emit readyRead(); }
    void reply(int command, const QByteArray &payload) {
        QByteArray frame("$M>", 3); frame.append(char(payload.size())); frame.append(char(command)); frame += payload;
        quint8 checksum = 0; for (int i = 3; i < frame.size(); ++i) checksum ^= quint8(frame[i]);
        frame.append(char(checksum)); feed(frame);
    }
    void identity() {
        reply(1, QByteArray::fromHex("000127")); reply(2, "BTFL"); reply(3, QByteArray::fromHex("030400"));
        reply(4, QByteArray::fromHex("4f425344000002030b") + "OMNIBUSF4SD");
        reply(119, QByteArray::fromHex("010500")); // ARM is bit 2, not bit 0.
    }
    void status(quint32 flags) {
        QByteArray payload(11, 0); qToLittleEndian(flags, reinterpret_cast<uchar *>(payload.data() + 6)); reply(101, payload);
    }
protected:
    qint64 readData(char *buffer, qint64 maximum) override {
        const auto size = qMin(maximum, qint64(incoming.size())); memcpy(buffer, incoming.constData(), size);
        incoming.remove(0, size); return size;
    }
    qint64 writeData(const char *buffer, qint64 size) override {
        writes.append(QByteArray(buffer, size));
        if (shortWrite) return -1;
        if (delayedWrite) queued += size;
        return size;
    }
};

class FirmwareInstallationTests : public QObject {
    Q_OBJECT
private slots:
    void betaflightBootVerifiesIdentityDisarmedStateAndCli() {
        BetaflightDevice device; BetaflightBootController boot; QSignalSpy finished(&boot, &BetaflightBootController::finished);
        boot.start(&device); device.feed(QByteArray(5000, 'x'));
        device.reply(4, "OBSD"); // Unrequested or malformed identity cannot skip verification.
        QCOMPARE(device.writes.size(), 1); device.identity(); QCOMPARE(device.writes.size(), 6);
        device.status(1); // Another flight mode is active; ARM is still clear.
        QCOMPARE(device.writes.last(), QByteArray("#"));
        device.feed("\r\n# "); QVERIFY(!device.writes.contains("bl\n"));
        device.feed("\r\nEntering CLI Mode, type 'exit' to return, or 'help'\r\n");
        device.feed("\r\n#"); QVERIFY(!device.writes.contains("bl\n"));
        device.delayedWrite = true; device.feed(" "); QCOMPARE(device.writes.last(), QByteArray("bl\n"));
        QCoreApplication::processEvents(); QCOMPARE(finished.size(), 0);
        const auto sent = device.queued; device.queued = 0; emit device.bytesWritten(sent);
        QCOMPARE(finished.size(), 1); QVERIFY(finished.first()[0].toBool());
        device.feed("\r\nEntering CLI Mode\r\n# "); QCOMPARE(device.writes.count("bl\n"), 1);
    }
    void betaflightBootRejectsWrongIdentity_data() {
        QTest::addColumn<int>("command"); QTest::addColumn<QByteArray>("payload");
        QTest::newRow("API-major") << 1 << QByteArray::fromHex("000227");
        QTest::newRow("firmware-family") << 2 << QByteArray("INAV");
        QTest::newRow("unreviewed-version") << 3 << QByteArray::fromHex("040501");
        QTest::newRow("other-Omnibus") << 4 << QByteArray::fromHex("4f4246340000");
        QTest::newRow("truncated-board") << 4 << QByteArray("OBSD");
        QTest::newRow("conflicting-target") << 4 << (QByteArray::fromHex("4f4253440000020309") + "OMNIBUSF4");
        QTest::newRow("missing-ARM-mode") << 119 << QByteArray::fromHex("010203");
        QTest::newRow("duplicate-ARM-mode") << 119 << QByteArray::fromHex("0000");
        QTest::newRow("missing-status") << 101 << QByteArray(6, 0);
        QByteArray armed(11, 0); armed[6] = 4;
        QTest::newRow("armed") << 101 << armed;
    }
    void betaflightBootRejectsWrongIdentity() {
        QFETCH(int, command); QFETCH(QByteArray, payload);
        BetaflightDevice device; BetaflightBootController boot; QSignalSpy finished(&boot, &BetaflightBootController::finished);
        boot.start(&device);
        if (command != 1) device.reply(1, QByteArray::fromHex("000127"));
        if (command > 2) device.reply(2, "BTFL");
        if (command > 3) device.reply(3, QByteArray::fromHex("030400"));
        if (command > 4) device.reply(4, QByteArray::fromHex("4f425344000002030b") + "OMNIBUSF4SD");
        if (command == 101) device.reply(119, QByteArray::fromHex("010500"));
        device.reply(command, payload);
        QCOMPARE(finished.size(), 1); QVERIFY(!finished.first()[0].toBool());
        QVERIFY(!device.writes.contains("#")); QVERIFY(!device.writes.contains("bl\n"));
    }
    void betaflightBootChecksumTimeoutCancelAndWriteFailure() {
        BetaflightDevice device; BetaflightBootController boot; QSignalSpy finished(&boot, &BetaflightBootController::finished);
        boot.start(&device, 25); device.feed(QByteArray::fromHex("244d3e030100012700"));
        QCOMPARE(device.writes.size(), 1); QTRY_COMPARE(finished.size(), 1); QVERIFY(!finished.first()[0].toBool());
        finished.clear(); boot.start(&device, 25); boot.cancel(); QTest::qWait(30); QVERIFY(finished.isEmpty());
        device.shortWrite = true; boot.start(&device); QCOMPARE(finished.size(), 1); QVERIFY(!finished.first()[0].toBool());
        QVERIFY(!device.writes.contains("#")); QVERIFY(!device.writes.contains("bl\n"));
    }
    void importedBuildBundle_data() {
        QTest::addColumn<QString>("profile"); QTest::addColumn<int>("flashSize");
        QTest::newRow("Radix-F446") << QString("radix") << 524288;
        QTest::newRow("OmnibusF4SD-F405") << QString("omnibusf4sd") << 1048576;
    }
    void importedBuildBundle() {
        QFETCH(QString, profile); QFETCH(int, flashSize);
        const auto board = FirmwareCatalog::instance().board(profile);
        const auto image = FirmwareInstallImage::load(board, QString(INSTALL_IMAGE_ROOT "/ef_%1.hex").arg(profile));
        QVERIFY2(image.valid(), qPrintable(image.error));
        QVERIFY(image.romBoot); QCOMPARE(image.bytes.size(), flashSize);
        QCOMPARE(image.descriptor.mid(60, 20), FirmwareInstallImage::gcsUavoHash());
        QCOMPARE(image.profileId, profile); QCOMPARE(image.layoutId, board.layoutId);
        QCOMPARE(image.bytes.mid(49256, 32), FirmwareInstallImage::importedProfileHash(profile));
        QCOMPARE(image.segments.size(), 3);
        QCOMPARE(image.segments[0].offset, quint32(0)); QCOMPARE(image.segments[0].size, quint32(16384));
        QCOMPARE(image.segments[1].offset, quint32(49152)); QCOMPARE(image.segments[1].size, quint32(16384));
        QCOMPARE(image.segments[2].offset, quint32(131072)); QCOMPARE(image.segments[2].size, quint32(flashSize - 131072));
    }
    void importedWritesOnlyReviewedSectors_data() { importedBuildBundle_data(); }
    void importedWritesOnlyReviewedSectors() {
        QFETCH(QString, profile); QFETCH(int, flashSize);
        const auto board = FirmwareCatalog::instance().board(profile);
        const auto path = QString(INSTALL_IMAGE_ROOT "/ef_%1.hex").arg(profile);
        const auto image = FirmwareInstallImage::load(board, path);
        QVERIFY2(image.valid(), qPrintable(image.error));
        FakeRunner runner; runner.discovery = flashSize == 524288 ? radixListing() : listing();
        const auto before = runner.flash;
        DfuInstaller installer(nullptr, &runner);
        QSignalSpy nativeStart(&installer, &DfuInstaller::nativeFirmwareStartRequested);
        installer.start(board, path, "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        QCOMPARE(runner.writes.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(runner.writes[i].first, image.segments[i].offset);
            QCOMPARE(runner.writes[i].second, image.bytes.mid(image.segments[i].offset, image.segments[i].size));
        }
        QCOMPARE(runner.flash.mid(16384, 32768), before.mid(16384, 32768));
        QCOMPARE(runner.flash.mid(65536, 65536), before.mid(65536, 65536));
        int reads = 0;
        for (const auto &command : runner.commands)
            if (command.contains("-U") && !command.join(' ').contains(":leave")) ++reads;
        QCOMPARE(reads, 3);
        runner.discovery = ""; installer.startInstalledFirmware();
        QTRY_VERIFY(!installer.startError().isEmpty());
        QVERIFY(!installer.startError().contains("remove the BOOT"));
        QCOMPARE(nativeStart.size(), 0); QCOMPARE(runner.writes.size(), 3);
        installer.startupTimedOut();
        QCOMPARE(installer.stage(), DfuInstaller::Stage::StartupFailed);
        QVERIFY(installer.startError().contains("written and verified"));
        installer.telemetryReceived(image.descriptor);
        QCOMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        installer.telemetryReceived(image.descriptor, 1, profile, board.layoutId, FirmwareInstallImage::importedProfileHash(profile));
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Complete);
    }
    void importedRejectsWrongRuntimeProfile_data() { importedBuildBundle_data(); }
    void importedRejectsWrongRuntimeProfile() {
        QFETCH(QString, profile); QFETCH(int, flashSize);
        const auto board = FirmwareCatalog::instance().board(profile);
        const auto path = QString(INSTALL_IMAGE_ROOT "/ef_%1.hex").arg(profile);
        FakeRunner runner; runner.discovery = flashSize == 524288 ? radixListing() : listing(); DfuInstaller installer(nullptr, &runner);
        installer.start(board, path, "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        const auto image = FirmwareInstallImage::load(board, path);
        installer.telemetryReceived(image.descriptor, 1, profile == "radix" ? "omnibusf4sd" : "radix",
            board.layoutId, FirmwareInstallImage::importedProfileHash(profile));
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Failed);
    }
    void importedRejectsUnsafePackage_data() {
        QTest::addColumn<QString>("profile");
        QTest::addColumn<QString>("damage");
        for (const auto &profile : {"radix", "omnibusf4sd"})
        for (const auto &name : {"layout", "profile", "settings-sector", "autotune-sector", "identity", "vector", "uavo", "hash"})
            QTest::newRow(qPrintable(QString("%1-%2").arg(profile, name))) << QString(profile) << QString(name);
    }
    void importedRejectsUnsafePackage() {
        QFETCH(QString, profile); QFETCH(QString, damage);
        QTemporaryDir directory;
        const QString name = "ef_" + profile;
        for (const auto &suffix : {"hex", "bin", "manifest.json"})
            QVERIFY(QFile::copy(QString(INSTALL_IMAGE_ROOT "/") + name + "." + suffix,
                directory.filePath(name + "." + suffix)));
        QFile file(directory.filePath(name + ".manifest.json"));
        QVERIFY(file.open(QIODevice::ReadOnly)); auto manifest = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        QFile binary(directory.filePath(name + ".bin"));
        QVERIFY(binary.open(QIODevice::ReadOnly)); auto bytes = binary.readAll(); binary.close();
        if (damage == "layout") manifest["layoutId"] = "another-layout";
        else if (damage == "profile") manifest["profileSha256"] = QString(64, '0');
        else if (damage.endsWith("-sector")) {
            auto regions = manifest["programSegments"].toArray();
            auto region = regions[1].toObject(); region["offset"] = damage == "settings-sector" ? 16384 : 65536;
            regions[1] = region; manifest["programSegments"] = regions;
        } else {
            const int descriptor = bytes.size() - 100;
            int offset = damage == "identity" ? 49256 : damage == "vector" ? 4 : damage == "uavo" ? descriptor + 60 : 200000;
            bytes[offset] ^= 1;
            if (damage != "hash") {
                // Retain valid outer integrity so semantic identity/vector validation is exercised.
                const int size = manifest["firmware"].toObject()["size"].toInt();
                const auto sha1 = QCryptographicHash::hash(bytes.left(size), QCryptographicHash::Sha1);
                bytes.replace(descriptor + 40, 20, sha1);
                auto firmware = manifest["firmware"].toObject(); firmware["sha1"] = QString::fromLatin1(sha1.toHex());
                manifest["firmware"] = firmware;
                auto image = manifest["image"].toObject();
                image["sha256"] = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
                manifest["image"] = image;
            }
        }
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(manifest).toJson()); file.close();
        QVERIFY(binary.open(QIODevice::WriteOnly)); binary.write(bytes); binary.close();
        FakeRunner runner; DfuInstaller installer(nullptr, &runner);
        installer.start(FirmwareCatalog::instance().board(profile), directory.filePath(name + ".hex"), {}, "/bin/true");
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Failed); QVERIFY(runner.commands.isEmpty());
    }
    void importedFailedVerificationStopsFurtherWrites_data() { importedBuildBundle_data(); }
    void importedFailedVerificationStopsFurtherWrites() {
        QFETCH(QString, profile); QFETCH(int, flashSize);
        FakeRunner runner; runner.discovery = flashSize == 524288 ? radixListing() : listing(); runner.corruptRead = true; DfuInstaller installer(nullptr, &runner);
        installer.start(FirmwareCatalog::instance().board(profile), QString(INSTALL_IMAGE_ROOT "/ef_%1.hex").arg(profile), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::Failed);
        QCOMPARE(runner.writes.size(), 1);
        for (const auto &command : runner.commands) QVERIFY(!command.join(' ').contains(":leave"));
    }
    void radixRejectsDifferentEraseGeometry_data() {
        QTest::addColumn<QString>("geometry");
        QTest::newRow("1MiB-F405") << QString("@Internal Flash /0x08000000/04*016Kg,01*064Kg,07*128Kg");
        QTest::newRow("64KiB-sectors-erase-settings") << QString("@Internal Flash /0x08000000/08*064Kg");
        QTest::newRow("128KiB-sectors-erase-settings") << QString("@Internal Flash /0x08000000/04*128Kg");
    }
    void radixRejectsDifferentEraseGeometry() {
        QFETCH(QString, geometry); FakeRunner runner; runner.discovery = listing("1-3", geometry);
        DfuInstaller installer(nullptr, &runner);
        installer.start(FirmwareCatalog::instance().board("radix"), INSTALL_IMAGE_ROOT "/ef_radix.hex", "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::Failed); QVERIFY(runner.writes.isEmpty());
    }
    void omnibusRejectsDifferentEraseGeometry_data() {
        QTest::addColumn<QString>("geometry");
        QTest::newRow("512KiB-F446") << QString("@Internal Flash /0x08000000/04*016Kg,01*064Kg,03*128Kg");
        QTest::newRow("128KiB-sectors-erase-settings") << QString("@Internal Flash /0x08000000/08*128Kg");
        QTest::newRow("read-only-identity-sector") << QString("@Internal Flash /0x08000000/03*016Kg,01*016Ka,01*064Kg,07*128Kg");
    }
    void omnibusRejectsDifferentEraseGeometry() {
        QFETCH(QString, geometry); FakeRunner runner; runner.discovery = listing("1-3", geometry);
        DfuInstaller installer(nullptr, &runner);
        installer.start(FirmwareCatalog::instance().board("omnibusf4sd"), INSTALL_IMAGE_ROOT "/ef_omnibusf4sd.hex", "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::Failed); QVERIFY(runner.writes.isEmpty());
    }
    void realBuildBundle() {
        const auto board = FirmwareCatalog::instance().board("hglrc-f4-v6pro");
        const auto image = FirmwareInstallImage::load(board, QString(INSTALL_IMAGE_ROOT "/ef_revolution.hex"));
        QVERIFY2(image.valid(), qPrintable(image.error)); QCOMPARE(image.bytes.size(), 524288);
        QCOMPARE(image.descriptor.mid(60, 20), FirmwareInstallImage::gcsUavoHash());
        QCOMPARE(FirmwareCatalog::firmwareName(image.descriptor), QString("DroniNG 1.0"));
        auto previous = image.descriptor;
        previous.replace(14, 26, QByteArray(26, 0)); previous.replace(14, 10, "next-dirty");
        QCOMPARE(FirmwareCatalog::firmwareName(previous, {INSTALL_IMAGE_ROOT}), QString("DroniNG 1.0"));
        previous[40] = char(quint8(previous[40]) ^ 1);
        QCOMPARE(FirmwareCatalog::firmwareName(previous, {INSTALL_IMAGE_ROOT}), QString("dRonin next-dirty"));
    }
    void productNameRequiresMatchingImage() {
        Fixture fixture;
        fixture.descriptor.replace(14, 11, "DroniNG 1.0");
        fixture.image.replace(132096, 100, fixture.descriptor);
        fixture.image.replace(524188, 100, fixture.descriptor);
        fixture.save(true);
        auto installed = fixture.descriptor;
        installed.replace(14, 26, QByteArray(26, 0)); installed.replace(14, 10, "next-dirty");
        const QStringList roots{fixture.directory.path()};
        QCOMPARE(FirmwareCatalog::firmwareName(installed, roots), QString("DroniNG 1.0"));
        auto otherCommit = installed; otherCommit[4] = char(quint8(otherCommit[4]) ^ 1);
        QCOMPARE(FirmwareCatalog::firmwareName(otherCommit, roots), QString("DroniNG 1.0"));
        QVERIFY(!FirmwareCatalog::matchingFirmwareDescriptor(otherCommit, roots).isEmpty());
        for (int offset : {12, 13, 40, 60}) {
            auto changed = installed; changed[offset] = char(quint8(changed[offset]) ^ 1);
            QCOMPARE(FirmwareCatalog::firmwareName(changed, roots), QString("dRonin next-dirty"));
            QVERIFY(FirmwareCatalog::matchingFirmwareDescriptor(changed, roots).isEmpty());
        }
        fixture.image[200] = char(quint8(fixture.image[200]) ^ 1); fixture.save();
        QCOMPARE(FirmwareCatalog::firmwareName(installed, roots), QString("dRonin next-dirty"));
        QCOMPARE(FirmwareCatalog::firmwareName(fixture.descriptor, {}), QString("DroniNG 1.0"));
        QVERIFY(FirmwareCatalog::firmwareName(installed.left(80), roots).isEmpty());
    }
    void rejectedImage_data() {
        QTest::addColumn<QString>("damage");
        for (const auto &name : {"hash", "board", "uavo", "bootloader", "vector", "bounds", "path", "missingManifest"})
            QTest::newRow(name) << QString(name);
    }
    void rejectedImage() {
        QFETCH(QString, damage); Fixture fixture;
        if (damage == "hash") fixture.image[200] = 0;
        else if (damage == "board") fixture.manifest["boardType"] = 136;
        else if (damage == "uavo") { fixture.image[524248] = 0; fixture.image[132156] = 0; }
        else if (damage == "bootloader") fixture.image[32644] = 136;
        else if (damage == "vector") fixture.image[4] = 0;
        else if (damage == "bounds") {
            auto fw = fixture.manifest["firmware"].toObject(); fw["offset"] = -1; fixture.manifest["firmware"] = fw;
        } else if (damage == "path") {
            auto image = fixture.manifest["image"].toObject(); image["file"] = "../../other.bin"; fixture.manifest["image"] = image;
        }
        fixture.save(damage != "hash");
        if (damage == "missingManifest") QVERIFY(QFile::remove(fixture.directory.filePath("ef_revolution.manifest.json")));
        FakeRunner runner; DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), {}, "/bin/true");
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Failed); QVERIFY(runner.commands.isEmpty());
    }
    void layout_data() {
        QTest::addColumn<QString>("layout"); QTest::addColumn<bool>("allowed");
        QTest::newRow("f405-1mb") << "@Internal Flash /0x08000000/04*016Kg,01*064Kg,07*128Kg" << true;
        QTest::newRow("f405-512k") << "@Internal Flash /0x08000000/04*016Kg,01*064Kg,03*128Kg" << true;
        QTest::newRow("too-small") << "@Internal Flash /0x08000000/04*016Kg" << false;
        QTest::newRow("read-protected") << "@Internal Flash /0x08000000/08*128Kf" << false;
        QTest::newRow("read-only") << "@Internal Flash /0x08000000/08*128Ka" << false;
        QTest::newRow("wrong-bank") << "@Internal Flash /0x08020000/08*128Kg" << false;
        QTest::newRow("options") << "@Option Bytes /0x1ffff800/01*016Bg" << false;
        QTest::newRow("sector-crossing") << "@Internal Flash /0x08000000/02*384Kg" << false;
        QTest::newRow("malformed") << "@Internal Flash /0x08000000/9999999999999*128Kg" << false;
    }
    void layout() {
        QFETCH(QString, layout); QFETCH(bool, allowed);
        DfuFlashDevice device; device.layout = layout; QCOMPARE(device.supports(0x08000000, 524288), allowed);
    }
    void installationStartsOnDfuAndNeedsTelemetry() {
        Fixture fixture; FakeRunner runner; DfuInstaller installer(nullptr, &runner);
        QSignalSpy written(&installer, &DfuInstaller::firmwareWritten);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QCOMPARE(runner.commands.size(), 1); QVERIFY(runner.written.isEmpty());
        // Validate a private snapshot even if the original file changes after preparation.
        fixture.image[42] = 0; fixture.save();
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        QCOMPARE(written.size(), 1); QCOMPARE(runner.commands.size(), 5);
        QCOMPARE(runner.written.at(42), char(0xff));
        const auto download = runner.commands[2];
        QVERIFY(download.contains("-p")); QCOMPARE(download[download.indexOf("-p") + 1], QString("1-3"));
        QVERIFY(download.contains(",1234ABCD")); QVERIFY(!download.join(' ').contains("force"));
        QVERIFY(!download.join(' ').contains("leave"));
        QVERIFY(runner.commands.last().join(' ').contains("0x8000000:1:leave"));
        installer.telemetryReceived(fixture.descriptor); QCOMPARE(installer.stage(), DfuInstaller::Stage::Complete);
    }
    void rejectedDevice_data() {
        QTest::addColumn<QString>("damage");
        for (const auto &name : {"multiple", "port", "permissions", "usbUnavailable", "protected", "changed",
                                "serialChanged", "layoutChanged", "missingAtWrite"})
            QTest::newRow(name) << QString(name);
    }
    void profileSettingsMustFinishBeforeInstallationCompletes() {
        Fixture fixture;
        fixture.board.settings = FirmwareCatalog::instance().board("hglrc-f4-v6pro").settings;
        FakeRunner runner; DfuInstaller installer(nullptr, &runner);
        QSignalSpy settings(&installer, &DfuInstaller::boardSettingsRequested);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        installer.telemetryReceived(fixture.descriptor);
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Configuring);
        QCOMPARE(settings.size(), 1);
        installer.telemetryReceived(fixture.descriptor);
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Configuring);
        installer.boardSettingsApplied(true, "Reconnecting after restart");
        QCOMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        installer.telemetryReceived(fixture.descriptor);
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Complete);
    }
    void rejectedProfileSettingsCannotReportSuccess() {
        Fixture fixture;
        fixture.board.settings = FirmwareCatalog::instance().board("hglrc-f4-v6pro").settings;
        FakeRunner runner; DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        installer.telemetryReceived(fixture.descriptor);
        installer.boardSettingsApplied(false, "Settings rejected");
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Failed);
        installer.telemetryReceived(fixture.descriptor);
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Failed);
    }
    void rejectedDevice() {
        QFETCH(QString, damage); Fixture fixture; FakeRunner runner;
        if (damage == "multiple") runner.discovery += listing("1-4");
        else if (damage == "port") runner.discovery = listing("1-4");
        else if (damage == "permissions") runner.discovery = "Cannot open DFU device (LIBUSB_ERROR_ACCESS)";
        else if (damage == "usbUnavailable") runner.discovery = "dfu-util: unable to initialize libusb: LIBUSB_ERROR_OTHER";
        else if (damage == "protected") runner.discovery = listing("1-3", "@Internal Flash /0x08000000/08*128Kf");
        else if (damage == "changed") runner.secondDiscovery = listing("1-4");
        else if (damage == "serialChanged") runner.secondDiscovery = listing().replace("1234ABCD", "5678EFAB");
        else if (damage == "layoutChanged") runner.secondDiscovery = listing("1-3", "@Internal Flash /0x08000000/04*016Kg,01*064Kg,03*128Kg");
        else runner.secondDiscovery = "";
        DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::Failed); QVERIFY(runner.written.isEmpty());
        if (damage == "usbUnavailable") QVERIFY(installer.log().contains("Linux USB access is unavailable"));
        QCOMPARE(installer.usbAccessDenied(), damage == "permissions");
        if (damage == "permissions") {
            QVERIFY(installer.accessCommand().isEmpty());
            QVERIFY(installer.log().contains("missing its USB access rules"));
        }
    }
    void usbAccessCommandQuotesPackagePath() {
        const QString path = "/tmp/controller's $(id)/rules.udev";
        QCOMPARE(DfuInstaller::usbAccessRulesCommand(path),
            QString("sudo install -m 0644 '/tmp/controller'\\''s $(id)/rules.udev' /etc/udev/rules.d/45-droning.rules && sudo udevadm control --reload-rules"));
        QVERIFY(DfuInstaller::usbAccessRulesCommand({}).isEmpty());
    }
    void accessRulesCommandAndRetry() {
        Fixture fixture; FakeRunner runner;
        QVERIFY(QDir(fixture.directory.path()).mkdir("bin"));
        const QString helper = fixture.directory.filePath("bin/dfu-util");
        QFile executable(helper); QVERIFY(executable.open(QIODevice::WriteOnly)); executable.close();
        QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::ExeOwner));
        const QString rulesPath = fixture.directory.filePath("rules.udev");
        QFile rules(rulesPath); QVERIFY(rules.open(QIODevice::WriteOnly));
        QVERIFY(rules.write("SUBSYSTEM==\"usb\", ATTR{idVendor}==\"0483\", ATTR{idProduct}==\"df11\", TAG+=\"uaccess\"\n") > 0);
        rules.close();
        runner.discovery = "Cannot open DFU device 0483:df11 (LIBUSB_ERROR_ACCESS)";
        DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), "1-3", helper);
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::Failed);
        QVERIFY(installer.usbAccessDenied());
        QCOMPARE(installer.accessCommand(), DfuInstaller::usbAccessRulesCommand(rulesPath));
        QVERIFY(installer.log().contains("The board is in DFU mode"));
        QVERIFY(runner.written.isEmpty());
        for (const auto &command : runner.commands) QVERIFY(command.contains("-l"));
        runner.discovery = listing();
        installer.start(fixture.board, fixture.path(), "1-3", helper);
        QVERIFY(!installer.usbAccessDenied()); QVERIFY(installer.accessCommand().isEmpty());
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        QCOMPARE(runner.written, fixture.image);
        installer.telemetryReceived(fixture.descriptor);
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Complete);
    }
    void transferFailures_data() {
        QTest::addColumn<QString>("damage");
        for (const auto &name : {"write", "corrupt", "short", "identity"}) QTest::newRow(name) << QString(name);
    }
    void transferFailures() {
        QFETCH(QString, damage); Fixture fixture; FakeRunner runner;
        runner.writeFailure = damage == "write"; runner.corruptRead = damage == "corrupt";
        runner.shortRead = damage == "short"; runner.leaveFailure = damage == "leave";
        DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), {}, "/bin/true");
        if (damage == "identity") {
            QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
            installer.telemetryReceived(QByteArray(100, 0));
        }
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::Failed);
        if (damage == "corrupt" || damage == "short") QCOMPARE(runner.commands.size(), 4);
    }
    void progressNamesTheCurrentTransfer() {
        Fixture fixture; FakeRunner runner; DfuInstaller installer(nullptr, &runner);
        runner.writeProgress = {"Erase   [== ] 25%\r", "Erase   [===] 100%\rDown",
                                "load [   ] 0%\rDownload [===] 100%\n"};
        runner.readProgress = {"Upload [=  ] 40%\rUpload [===] 100%\n"};
        QSignalSpy progress(&installer, &DfuInstaller::transferProgress);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        QCOMPARE(progress.size(), 6);
        QCOMPARE(progress[0][0].toString(), QString("Erasing previous firmware"));
        QCOMPARE(progress[0][1].toInt(), 25);
        QCOMPARE(progress[2][0].toString(), QString("Writing firmware"));
        QCOMPARE(progress[2][1].toInt(), 0);
        QCOMPARE(progress[4][0].toString(), QString("Verifying firmware"));
        QCOMPARE(progress[4][1].toInt(), 40);
    }
    void startFailureKeepsVerifiedFirmwareAndRetriesOnlyBoot() {
        Fixture fixture; FakeRunner runner; runner.leaveFailure = true;
        DfuInstaller installer(nullptr, &runner);
        QSignalSpy written(&installer, &DfuInstaller::firmwareWritten);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        QVERIFY(!installer.startError().isEmpty()); QCOMPARE(written.size(), 1);
        runner.leaveFailure = false;
        installer.startInstalledFirmware();
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        QVERIFY(installer.startError().isEmpty()); QCOMPARE(written.size(), 1);
        QCOMPARE(runner.commands.size(), 7);
        QVERIFY(runner.commands[5].contains("-l"));
        QVERIFY(runner.commands[6].join(' ').contains("0x8000000:1:leave"));
        int writes = 0;
        for (const auto &command : runner.commands) if (command.contains("-D")) ++writes;
        QCOMPARE(writes, 1);
        installer.telemetryReceived(fixture.descriptor);
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Complete);
    }
    void startRejectsDifferentDfuDevice_data() {
        QTest::addColumn<QString>("discovery");
        QTest::newRow("other-port") << listing("1-4");
        QTest::newRow("other-serial") << QString(listing()).replace("1234ABCD", "5678DCBA");
        QTest::newRow("other-layout") << listing("1-3", "@Internal Flash /0x08000000/08*128Kg");
        QTest::newRow("ambiguous") << listing() + listing("1-4");
        QTest::newRow("permission") << QString("LIBUSB_ERROR_ACCESS");
        QTest::newRow("usb-unavailable") << QString("unable to initialize libusb");
    }
    void startRejectsDifferentDfuDevice() {
        QFETCH(QString, discovery); Fixture fixture; FakeRunner runner;
        DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        runner.discovery = discovery;
        installer.startInstalledFirmware();
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        QVERIFY(!installer.startError().isEmpty());
        QCOMPARE(runner.commands.size(), 6); QVERIFY(runner.commands.last().contains("-l"));
    }
    void nativeStartUsesInstalledIdentityAndIgnoresCancelledReply() {
        Fixture fixture; FakeRunner runner; DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        runner.discovery = "";
        QSignalSpy request(&installer, &DfuInstaller::nativeFirmwareStartRequested);
        installer.startInstalledFirmware();
        QTRY_COMPARE(request.size(), 1);
        QCOMPARE(request[0][0].toString(), QString("1-3"));
        QCOMPARE(request[0][1].toByteArray(), fixture.descriptor);
        installer.nativeFirmwareStarted(false, "No bootloader response");
        QCOMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        QCOMPARE(installer.startError(), QString("No bootloader response"));
        installer.startInstalledFirmware();
        QTRY_COMPARE(request.size(), 2);
        installer.nativeFirmwareStarted(true, {});
        QVERIFY(installer.startError().isEmpty());
        QCOMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        installer.startInstalledFirmware(); QTRY_COMPARE(request.size(), 3);
        installer.cancel(); installer.nativeFirmwareStarted(true, {});
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Idle);
        QCOMPARE(runner.commands.size(), 8);
    }
    void nativeStartValidatesControllerBeforeJump_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto &name : {"success", "invalid-descriptor", "open-fails", "wrong-board",
                "wrong-revision", "wrong-size", "wrong-firmware", "cancelled", "cancel-after-read", "jump-fails"})
            QTest::newRow(name) << QString(name);
    }
    void nativeStartValidatesControllerBeforeJump() {
        QFETCH(QString, scenario); Fixture fixture; FakeBootTransport transport;
        auto descriptor = fixture.descriptor;
        transport.caps.ID = 0x0903; transport.caps.HW_Rev = 3; transport.caps.SizeOfDesc = 100;
        transport.descriptor = descriptor;
        bool cancelled = scenario == "cancelled";
        if (scenario == "invalid-descriptor") descriptor[0] = 0;
        else if (scenario == "open-fails") transport.opens = false;
        else if (scenario == "wrong-board") transport.caps.ID = 0x0904;
        else if (scenario == "wrong-revision") transport.caps.HW_Rev = 4;
        else if (scenario == "wrong-size") transport.caps.SizeOfDesc = 99;
        else if (scenario == "wrong-firmware") transport.descriptor[40] ^= 1;
        else if (scenario == "cancel-after-read") transport.afterDescription = [&cancelled] { cancelled = true; };
        else if (scenario == "jump-fails") transport.starts = false;
        QString error;
        const bool success = uploader::FirmwareBootController::startVerifiedFirmware(transport,
            {}, descriptor, [&cancelled] { return cancelled; }, &error);
        QCOMPARE(success, scenario == "success");
        QCOMPARE(transport.startCount, scenario == "success" || scenario == "jump-fails" ? 1 : 0);
        QCOMPARE(error.isEmpty(), success || cancelled);
    }
    void cancelWait() {
        Fixture fixture; FakeRunner runner; runner.discovery = ""; DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), {}, "/bin/true"); installer.cancel();
        runner.discovery = listing();
        QTest::qWait(1200); QCOMPARE(installer.stage(), DfuInstaller::Stage::Idle); QCOMPARE(runner.commands.size(), 1);
    }
    void cancellationOnDiscoveryPreventsWriting() {
        Fixture fixture; FakeRunner runner; DfuInstaller installer(nullptr, &runner);
        connect(&installer, &DfuInstaller::changed, &installer, [&installer](DfuInstaller::Stage stage) {
            if (stage == DfuInstaller::Stage::Ready) installer.cancel();
        });
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::Idle);
        QCOMPARE(runner.commands.size(), 1); QVERIFY(runner.written.isEmpty());
    }
    void normalFirmwareReconnectionThenDfu() {
        Fixture fixture; FakeRunner runner; runner.discovery = "";
        runner.usbDescription = "Betaflight / OmnibusF4";
        DfuInstaller installer(nullptr, &runner);
        QSignalSpy changed(&installer, &DfuInstaller::changed);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_VERIFY(changed.last().at(1).toString().contains("Connected as Betaflight / OmnibusF4"));
        QCOMPARE(installer.stage(), DfuInstaller::Stage::NeedsBoot);
        QVERIFY(changed.last().at(1).toString().contains("Bootloader mode is not active"));
        QVERIFY(changed.last().at(1).toString().contains("your board's manual"));
        const auto events = changed.count();
        QTRY_VERIFY(runner.listCount >= 2);
        QCOMPARE(changed.count(), events); // An unchanged device must not repeat instructions or log entries.
        runner.usbDescription.clear();
        QTRY_COMPARE(changed.last().at(1).toString(), QString("Waiting for STM32 DFU…"));
        runner.usbDescription = "Betaflight / OmnibusF4";
        QTRY_VERIFY(changed.last().at(1).toString().contains("Connected as Betaflight / OmnibusF4"));
        QCOMPARE(installer.stage(), DfuInstaller::Stage::NeedsBoot);
        QVERIFY(runner.written.isEmpty());
        for (const auto &command : runner.commands) QVERIFY(command.contains("-l"));
        runner.discovery = listing();
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::AwaitingTelemetry);
        QCOMPARE(runner.written, fixture.image);
        installer.telemetryReceived(fixture.descriptor);
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Complete);
    }
    void cancelBootGuidanceStopsDiscovery() {
        Fixture fixture; FakeRunner runner; runner.discovery = "";
        runner.usbDescription = "Betaflight / OmnibusF4";
        DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QTRY_COMPARE(installer.stage(), DfuInstaller::Stage::NeedsBoot);
        installer.start(fixture.board, fixture.path(), "1-3", "/bin/true");
        QCOMPARE(runner.commands.size(), 1); // Existing discovery remains a single installation.
        installer.cancel(); runner.discovery = listing();
        QTest::qWait(1200);
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Idle);
        QCOMPARE(runner.commands.size(), 1); QVERIFY(runner.written.isEmpty());
    }
    void missingHelper() {
        Fixture fixture; FakeRunner runner; DfuInstaller installer(nullptr, &runner);
        installer.start(fixture.board, fixture.path(), {}, "/missing/dfu-util");
        QCOMPARE(installer.stage(), DfuInstaller::Stage::Failed); QVERIFY(runner.commands.isEmpty());
    }
};
QTEST_MAIN(FirmwareInstallationTests)
#include "firmware_installation.moc"
