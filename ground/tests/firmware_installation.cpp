/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <coreplugin/dfuinstaller.h>
#include <uploader/firmwarebootcontroller.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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

class FakeRunner : public DfuCommandRunner {
public:
    QString discovery = listing(), secondDiscovery, usbDescription;
    QList<QStringList> commands;
    QByteArray written;
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
        } else if (arguments.contains("-U")) {
            const bool leaving = arguments.join(' ').contains(":leave");
            if (!leaving) for (const auto &chunk : readProgress) emit this->output(chunk);
            QByteArray bytes = leaving ? written.left(1) : written;
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

class FirmwareInstallationTests : public QObject {
    Q_OBJECT
private slots:
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
        for (int offset : {4, 12, 13, 40, 60}) {
            auto changed = installed; changed[offset] = char(quint8(changed[offset]) ^ 1);
            QCOMPARE(FirmwareCatalog::firmwareName(changed, roots), QString("dRonin next-dirty"));
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
