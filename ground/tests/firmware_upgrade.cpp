/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <uploader/firmwareupgradecontroller.h>
#include <uploader/upgradeassistantdialog.h>
#include <coreplugin/firmwareinstallimage.h>
#include <coreplugin/firmwareuicomponents.h>
#include <uavobjects/uavobjectmanager.h>
#include <systemsettings.h>
#include <manualcontrolsettings.h>
#include <hwrevolution.h>
#include <actuatorsettings.h>
#include <stabilizationsettings.h>
#include <mixersettings.h>
#include <attitudesettings.h>
#include <sensorsettings.h>
#include <inssettings.h>
#include <stateestimation.h>
#include <utils/darktheme.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QtEndian>

using namespace uploader;
using Controller = FirmwareUpgradeController;

namespace {
void word(QByteArray &data, int offset, quint32 value)
{ qToLittleEndian(value, reinterpret_cast<uchar *>(data.data() + offset)); }
QByteArray image()
{
    QByteArray binary(1024, char(0x5a)), description(100, 0);
    word(binary, 0, 0x20020000); word(binary, 4, 0x08020101);
    description.replace(0, 4, "TlFw"); description[12] = 9; description[13] = 3;
    description.replace(40, 20, QCryptographicHash::hash(binary, QCryptographicHash::Sha1));
    description.replace(60, 20, Core::FirmwareInstallImage::gcsUavoHash());
    return binary + description;
}
struct Fixture {
    QTemporaryDir directory;
    UAVObjectManager objects;
    SystemSettings *setting = new SystemSettings;
    FirmwareUpgradeContext context;
    QByteArray partition = QByteArray(0x40000, char(0xff));
    Fixture() {
        objects.registerObject(setting);
        auto values = setting->getData(); values.AirframeType = SystemSettings::AIRFRAMETYPE_QUADX; setting->setData(values);
        context.target = "revolution"; context.name = "Revolution"; context.serial = "0123ABCD";
        context.boardId = 0x0903; context.codeSize = 0x60000; context.descriptionSize = 100;
        context.settingsSize = partition.size(); context.firmware = image();
        context.sourceDescriptor = context.firmware.right(100); context.sourceDescriptor[4] = 1;
        context.backupRoot = directory.path();
        word(partition, 0, 0x99abcedf); word(partition, 4, 0xe6e66666);
        word(partition, 256, 0xfafaaaaa); word(partition, 260, setting->getObjID());
        qToLittleEndian<quint16>(0, reinterpret_cast<uchar *>(partition.data() + 264));
        qToLittleEndian<quint16>(setting->getNumBytes(), reinterpret_cast<uchar *>(partition.data() + 266));
        setting->pack(reinterpret_cast<quint8 *>(partition.data() + 268));
    }
};
QByteArray readFile(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
void addLegacyReceiver(Fixture &f, quint8 rssiType = 0) {
    auto *manual = new ManualControlSettings;
    f.objects.registerObject(manual);
    auto values = manual->getData(); values.RssiType = rssiType; manual->setData(values);
    word(f.partition, 512, 0xfafaaaaa); word(f.partition, 516, 0x884ed0e6);
    qToLittleEndian<quint16>(0, reinterpret_cast<uchar *>(f.partition.data() + 520));
    qToLittleEndian<quint16>(manual->getNumBytes(), reinterpret_cast<uchar *>(f.partition.data() + 522));
    manual->pack(reinterpret_cast<quint8 *>(f.partition.data() + 524));
    f.context.sourceDescriptor[60] ^= 1;
}
QByteArray historicalPartition() {
    QByteArray partition(0x60000, char(0xff));
    word(partition, 0, 0x99abcedf); word(partition, 4, 0xe6e66666);
    int offset = 256;
    for (const auto &line : readFile(SETTINGS_FIXTURE).split('\n')) {
        if (line.isEmpty() || line.startsWith('#')) continue;
        const auto record = QByteArray::fromHex(line);
        partition.replace(offset, record.size(), record); offset += 256;
    }
    return partition;
}
}

class FirmwareUpgradeTests : public QObject {
    Q_OBJECT
private slots:
    void invalidImage_data() {
        QTest::addColumn<int>("fault");
        for (int fault = 0; fault < 7; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void invalidImage() {
        QFETCH(int, fault); Fixture f; auto context = f.context;
        if (fault == 0) context.firmware.clear();
        if (fault == 1) context.firmware[0] = 1;
        if (fault == 2) context.boardId = 0x0904;
        if (fault == 3) context.firmware[context.firmware.size() - 40] = 0;
        if (fault == 4) context.codeSize = 1000;
        if (fault == 5) { context.needsBootloader = true; context.bootloader.clear(); }
        if (fault == 6) context.descriptionSize = 20;
        Controller controller(&f.objects); QSignalSpy read(&controller, &Controller::readSettingsRequested);
        QSignalSpy write(&controller, &Controller::writeRequested), erase(&controller, &Controller::eraseSettingsRequested);
        controller.start(context); QCOMPARE(controller.state(), Controller::Failed);
        controller.proceed(); QCOMPARE(read.count(), 0); QCOMPARE(write.count(), 0); QCOMPARE(erase.count(), 0);
    }
    void packagedImageIntegrity() {
        const auto packaged = readFile(qEnvironmentVariable("FIRMWARE_UPGRADE_IMAGE_ROOT", INSTALL_IMAGE_ROOT) + "/fw_revolution.tlfw");
        QVERIFY(!packaged.isEmpty()); QVERIFY(Controller::validateImage(packaged, 0x0903, 0x60000, true).isEmpty());
    }
    void partitionValidation_data() {
        QTest::addColumn<int>("fault");
        for (int fault = 0; fault < 9; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void partitionValidation() {
        QFETCH(int, fault); Fixture f;
        if (fault == 0) f.partition.chop(1);
        if (fault == 1) word(f.partition, 260, 0x01020304);
        if (fault == 2) qToLittleEndian<quint16>(245, reinterpret_cast<uchar *>(f.partition.data() + 266));
        if (fault == 3) { word(f.partition, 0x20000, 0x99abcedf); word(f.partition, 0x20004, 0xe6e66666); }
        if (fault == 4) word(f.partition, 256, 0x12345678);
        if (fault == 5) f.partition.replace(512, 256, f.partition.mid(256, 256));
        if (fault == 6) f.partition[520] = 0;
        if (fault == 7) { f.partition.replace(768, 256, f.partition.mid(256, 256)); word(f.partition, 256, 0); }
        const auto result = FirmwareSettingsBackup::inspect(f.partition, "revolution", fault != 8, &f.objects);
        QVERIFY(!result.compatible); QVERIFY(!result.empty); QVERIFY(result.xml.isEmpty()); QVERIFY(!result.reason.isEmpty());
    }
    void completePortableBackup() {
        Fixture f;
        const auto result = FirmwareSettingsBackup::inspect(f.partition, "revolution", true, &f.objects);
        QVERIFY(result.compatible); QVERIFY(!result.empty); QCOMPARE(result.records.size(), 1);
        QVERIFY(result.xml.contains("SystemSettings")); QVERIFY(result.xml.contains("QuadX"));
        Controller controller(&f.objects); controller.start(f.context); controller.settingsRead(f.partition, true);
        QCOMPARE(controller.state(), Controller::InstallDecision); QVERIFY(!controller.resetSettings());
        QCOMPARE(readFile(controller.backupPath() + "/settings.bin"), f.partition);
        QCOMPARE(readFile(controller.backupPath() + "/settings.uav"), result.xml);
        auto manifest = QJsonDocument::fromJson(readFile(controller.backupPath() + "/backup.json")).object();
        QCOMPARE(manifest.value("controllerSerial").toString(), f.context.serial);
        QCOMPARE(manifest.value("sha256").toString().toLatin1(), QCryptographicHash::hash(f.partition, QCryptographicHash::Sha256).toHex());
    }
    void compatibleUpgradeRequiresReadback() {
        Fixture f; Controller controller(&f.objects);
        QSignalSpy erase(&controller, &Controller::eraseSettingsRequested), write(&controller, &Controller::writeRequested);
        QSignalSpy verify(&controller, &Controller::verifySettingsRequested), boot(&controller, &Controller::bootRequested);
        controller.start(f.context); controller.settingsRead(f.partition, true); controller.proceed();
        QCOMPARE(erase.count(), 0); QCOMPARE(write.count(), 1); QCOMPARE(controller.state(), Controller::WritingFirmware);
        controller.cancel(); QCOMPARE(controller.state(), Controller::WritingFirmware);
        controller.writeFinished(true); QCOMPARE(boot.count(), 1); QCOMPARE(controller.state(), Controller::WaitingFirmware);
        controller.firmwareConnected(f.context.firmware.right(100), f.context.serial.toLower());
        QCOMPARE(controller.state(), Controller::VerifyingSettings); QCOMPARE(verify.count(), 1);
        controller.settingsVerified(true); QCOMPARE(controller.state(), Controller::Complete); QVERIFY(!controller.setupRequired());
    }
    void globalSchemaChangePreservesMatchingRecords() {
        Fixture f; f.context.sourceDescriptor[60] ^= 1; Controller controller(&f.objects);
        controller.start(f.context); controller.settingsRead(f.partition, true);
        QVERIFY(!controller.resetSettings());
        QCOMPARE(controller.primaryText(), QString("Install firmware"));
        QVERIFY(QFile::exists(controller.backupPath() + "/settings.uav"));
    }
    void unknownRecordRequiresConsent() {
        Fixture f; f.context.sourceDescriptor[60] ^= 1; word(f.partition, 260, 0x01020304); Controller controller(&f.objects);
        QSignalSpy erase(&controller, &Controller::eraseSettingsRequested), write(&controller, &Controller::writeRequested);
        controller.start(f.context); controller.settingsRead(f.partition, true);
        QVERIFY(controller.resetSettings()); QCOMPARE(controller.primaryText(), QString("Install and reset settings"));
        QCOMPARE(readFile(controller.backupPath() + "/settings.bin"), f.partition);
        QVERIFY(!QFile::exists(controller.backupPath() + "/settings.uav"));
        QCOMPARE(erase.count(), 0); QCOMPARE(write.count(), 0);
        controller.proceed(); QCOMPARE(erase.count(), 1); QCOMPARE(write.count(), 0);
        controller.settingsErased(true); QCOMPARE(write.count(), 1); controller.writeFinished(true);
        controller.firmwareConnected(f.context.firmware.right(100), f.context.serial);
        QCOMPARE(controller.state(), Controller::Complete); QVERIFY(controller.setupRequired());
    }
    void legacyReceiverConversion_data() {
        QTest::addColumn<int>("rssiType");
        for (int type = 0; type <= 10; ++type) QTest::newRow(qPrintable(QString::number(type))) << type;
    }
    void legacyReceiverConversion() {
        QFETCH(int, rssiType); Fixture f; addLegacyReceiver(f, rssiType);
        const auto result = FirmwareSettingsBackup::inspect(f.partition, "revolution", true, &f.objects);
        QCOMPARE(result.compatible, rssiType <= 8);
        if (rssiType > 8) { QVERIFY(result.convertedPartition.isEmpty()); QVERIFY(result.conversions.isEmpty()); return; }
        QCOMPARE(result.conversions.size(), 1); QCOMPARE(result.records.size(), 2);
        QCOMPARE(result.records.last().id, ManualControlSettings::OBJID);
        QCOMPARE(result.records.last().data, f.partition.mid(524, ManualControlSettings::NUMBYTES));
        auto expected = f.partition; word(expected, 516, ManualControlSettings::OBJID);
        QCOMPARE(result.convertedPartition, expected);
        const auto again = FirmwareSettingsBackup::inspect(expected, "revolution", true, &f.objects);
        QVERIFY(again.compatible); QVERIFY(again.convertedPartition.isEmpty());
    }
    void historicalControllerSettings() {
        UAVObjectManager objects;
        for (auto *object : QList<UAVDataObject *>{new HwRevolution, new ActuatorSettings,
             new StabilizationSettings, new SystemSettings, new MixerSettings,
             new ManualControlSettings, new AttitudeSettings, new SensorSettings,
             new INSSettings, new StateEstimation}) objects.registerObject(object);
        const auto partition = historicalPartition();
        const auto result = FirmwareSettingsBackup::inspect(partition, "revolution", true, &objects);
        QVERIFY2(result.compatible, qPrintable(result.reason));
        QCOMPARE(result.records.size(), 10); QCOMPARE(result.conversions.size(), 1);
        auto expected = partition; word(expected, 6 * 256 + 4, ManualControlSettings::OBJID);
        QCOMPARE(result.convertedPartition, expected);
        QVERIFY(result.xml.contains("ManualControlSettings")); QVERIFY(result.xml.contains("ChannelMin"));
        const auto original = qEnvironmentVariable("FIRMWARE_UPGRADE_SOURCE_SETTINGS");
        if (!original.isEmpty()) {
            const auto live = FirmwareSettingsBackup::inspect(readFile(original), "revolution", true, &objects);
            QVERIFY2(live.compatible, qPrintable(live.reason)); QCOMPARE(live.records.size(), result.records.size());
            for (int i = 0; i < result.records.size(); ++i) {
                QCOMPARE(live.records[i].id, result.records[i].id); QCOMPARE(live.records[i].data, result.records[i].data);
            }
        }
    }
    void conversionRequiresPartitionAndTelemetryReadback() {
        Fixture f; addLegacyReceiver(f); Controller controller(&f.objects);
        QSignalSpy erase(&controller, &Controller::eraseSettingsRequested), convert(&controller, &Controller::writeSettingsRequested);
        QSignalSpy read(&controller, &Controller::readConvertedSettingsRequested), boot(&controller, &Controller::bootRequested);
        QSignalSpy verify(&controller, &Controller::verifySettingsRequested);
        controller.start(f.context); controller.settingsRead(f.partition, true);
        QVERIFY(!controller.resetSettings()); QCOMPARE(controller.primaryText(), QString("Install and preserve settings"));
        const auto manifest = QJsonDocument::fromJson(readFile(controller.backupPath() + "/backup.json")).object();
        QCOMPARE(manifest.value("conversions").toArray().size(), 1);
        QCOMPARE(readFile(controller.backupPath() + "/settings.bin"), f.partition);
        controller.proceed(); QCOMPARE(erase.count(), 0); QCOMPARE(convert.count(), 0);
        controller.writeFinished(true); QCOMPARE(controller.state(), Controller::WritingSettings);
        QCOMPARE(convert.count(), 1); QCOMPARE(boot.count(), 0); QVERIFY(!controller.canCancel());
        const auto converted = convert.first().first().toByteArray();
        QCOMPARE(manifest.value("convertedSha256").toString().toLatin1(), QCryptographicHash::hash(converted, QCryptographicHash::Sha256).toHex());
        controller.convertedSettingsWritten(true); QCOMPARE(controller.state(), Controller::ReadingConvertedSettings);
        QCOMPARE(read.count(), 1); QCOMPARE(boot.count(), 0); QVERIFY(!controller.canCancel());
        controller.convertedSettingsRead(converted, true); QCOMPARE(boot.count(), 1);
        controller.firmwareConnected(f.context.firmware.right(100), f.context.serial);
        QCOMPARE(controller.state(), Controller::VerifyingSettings); QCOMPARE(verify.count(), 1);
        controller.settingsVerified(true); QCOMPARE(controller.state(), Controller::Complete); QVERIFY(!controller.setupRequired());
    }
    void failedConversionDoesNotBoot_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("write-error") << 0; QTest::newRow("read-error") << 1;
        QTest::newRow("readback-mismatch") << 2; QTest::newRow("disconnect-writing") << 3;
        QTest::newRow("disconnect-reading") << 4;
    }
    void failedConversionDoesNotBoot() {
        QFETCH(int, fault); Fixture f; addLegacyReceiver(f); Controller controller(&f.objects);
        QSignalSpy convert(&controller, &Controller::writeSettingsRequested), boot(&controller, &Controller::bootRequested);
        controller.start(f.context); controller.settingsRead(f.partition, true); controller.proceed(); controller.writeFinished(true);
        auto converted = convert.first().first().toByteArray();
        if (fault == 3) controller.deviceRemoved();
        controller.convertedSettingsWritten(fault != 0);
        if (fault != 0 && fault != 3) {
            if (fault == 4) controller.deviceRemoved();
            if (fault == 2) converted[524] ^= 1;
            controller.convertedSettingsRead(converted, fault != 1);
        }
        QCOMPARE(controller.state(), Controller::Failed); QCOMPARE(boot.count(), 0);
        QCOMPARE(readFile(controller.backupPath() + "/settings.bin"), f.partition);
        controller.convertedSettingsWritten(true); controller.convertedSettingsRead(converted, true); QCOMPARE(boot.count(), 0);
    }
    void destinationCollisionRejectsConversion() {
        Fixture f; addLegacyReceiver(f);
        f.partition.replace(768, 256, f.partition.mid(512, 256)); word(f.partition, 772, ManualControlSettings::OBJID);
        const auto result = FirmwareSettingsBackup::inspect(f.partition, "revolution", true, &f.objects);
        QVERIFY(!result.compatible); QVERIFY(result.convertedPartition.isEmpty()); QVERIFY(result.conversions.isEmpty());
    }
    void conversionRetryRetainsOriginalBackup() {
        Fixture f; addLegacyReceiver(f); Controller controller(&f.objects);
        QSignalSpy convert(&controller, &Controller::writeSettingsRequested), boot(&controller, &Controller::bootRequested);
        QSignalSpy backup(&controller, &Controller::readSettingsRequested);
        controller.start(f.context); controller.settingsRead(f.partition, true);
        const auto originalPath = controller.backupPath();
        controller.proceed(); controller.writeFinished(true); controller.deviceRemoved(); controller.convertedSettingsWritten(false);
        QVERIFY(controller.canRetrySettingsConversion());
        QCOMPARE(controller.primaryText(), QString("Restore saved settings"));
        controller.retrySettingsConversion(f.context.boardId, f.context.serial, f.context.codeSize,
            f.context.settingsSize, f.context.firmware.right(100));
        QCOMPARE(controller.state(), Controller::WritingSettings); QCOMPARE(convert.count(), 2);
        QCOMPARE(convert[0][0].toByteArray(), convert[1][0].toByteArray()); QCOMPARE(backup.count(), 1);
        QCOMPARE(controller.backupPath(), originalPath); QVERIFY(!controller.transferDisconnected());
        QCOMPARE(readFile(originalPath + "/settings.bin"), f.partition);
        controller.convertedSettingsWritten(true); controller.convertedSettingsRead(convert[1][0].toByteArray(), true);
        QCOMPARE(boot.count(), 1); QVERIFY(!controller.canRetrySettingsConversion());
    }
    void conversionRetryRechecksIdentity_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("board") << 0; QTest::newRow("serial") << 1; QTest::newRow("code-size") << 2;
        QTest::newRow("settings-size") << 3; QTest::newRow("firmware") << 4;
    }
    void conversionRetryRechecksIdentity() {
        QFETCH(int, fault); Fixture f; addLegacyReceiver(f); Controller controller(&f.objects);
        QSignalSpy convert(&controller, &Controller::writeSettingsRequested), boot(&controller, &Controller::bootRequested);
        controller.start(f.context); controller.settingsRead(f.partition, true); controller.proceed(); controller.writeFinished(true);
        controller.convertedSettingsWritten(false);
        auto descriptor = f.context.firmware.right(100); if (fault == 4) descriptor[40] ^= 1;
        controller.retrySettingsConversion(fault == 0 ? 0x0904 : f.context.boardId, fault == 1 ? QString("OTHER") : f.context.serial,
            fault == 2 ? 0x50000 : f.context.codeSize, fault == 3 ? 0x30000 : f.context.settingsSize, descriptor);
        QCOMPARE(controller.state(), Controller::Failed); QCOMPARE(convert.count(), 1); QCOMPARE(boot.count(), 0);
    }
    void reconnectUsesCpuSerial_data() {
        QTest::addColumn<QString>("serial"); QTest::addColumn<bool>("matching");
        QTest::newRow("telemetry") << QString("260053000951363438333337") << true;
        QTest::newRow("bootloader") << QString("260053000951363438333337+BL") << true;
        QTest::newRow("firmware-usb") << QString("260053000951363438333337+fw") << true;
        QTest::newRow("updater-usb") << QString("260053000951363438333337+UP") << true;
        QTest::newRow("other-controller") << QString("260053000951363438333338+FW") << false;
        QTest::newRow("unknown-suffix") << QString("260053000951363438333337+OTHER") << false;
        QTest::newRow("missing-identity") << QString() << false;
    }
    void reconnectUsesCpuSerial() {
        QFETCH(QString, serial); QFETCH(bool, matching); Fixture f;
        f.context.serial = "260053000951363438333337+BL";
        Controller controller(&f.objects);
        controller.start(f.context); controller.settingsRead(f.partition, true); controller.proceed(); controller.writeFinished(true);
        controller.firmwareConnected(f.context.firmware.right(100), serial);
        QCOMPARE(controller.state(), matching ? Controller::VerifyingSettings : Controller::Failed);
    }
    void failedResetStopsFlash() {
        Fixture f; f.context.sourceDescriptor.clear(); Controller controller(&f.objects);
        QSignalSpy write(&controller, &Controller::writeRequested);
        controller.start(f.context); controller.settingsRead(f.partition, true); controller.proceed(); controller.settingsErased(false);
        QCOMPARE(controller.state(), Controller::Failed); controller.settingsErased(true); QCOMPARE(write.count(), 0);
    }
    void failedBackup_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("read-error") << 0; QTest::newRow("short-read") << 1; QTest::newRow("unwritable-destination") << 2;
    }
    void failedBackup() {
        QFETCH(int, fault); Fixture f;
        if (fault == 2) { QFile file(f.directory.filePath("blocked")); QVERIFY(file.open(QIODevice::WriteOnly)); f.context.backupRoot = file.fileName(); }
        Controller controller(&f.objects); QSignalSpy erase(&controller, &Controller::eraseSettingsRequested), write(&controller, &Controller::writeRequested);
        controller.start(f.context); controller.settingsRead(fault == 1 ? f.partition.left(1024) : f.partition, fault != 0);
        QCOMPARE(controller.state(), Controller::Failed); QVERIFY(controller.backupPath().isEmpty());
        controller.proceed(); QCOMPARE(write.count(), 0); QCOMPARE(erase.count(), 0);
    }
    void emptyAndUnknownAreDistinct() {
        Fixture f; Controller controller(&f.objects);
        controller.start(f.context); controller.settingsRead(QByteArray(f.partition.size(), char(0xff)), true);
        QVERIFY(!controller.resetSettings()); QVERIFY(controller.setupRequired());
        controller.cancel(); QCOMPARE(controller.state(), Controller::Cancelled);
        f.context.sourceDescriptor.clear(); controller.start(f.context); controller.settingsRead(f.partition, true);
        QVERIFY(controller.resetSettings());
    }
    void cancellationWaitsForRead() {
        Fixture f; Controller controller(&f.objects); QSignalSpy stop(&controller, &Controller::cancelReadRequested), write(&controller, &Controller::writeRequested);
        controller.start(f.context); controller.cancel(); QCOMPARE(stop.count(), 1); QCOMPARE(controller.state(), Controller::ReadingSettings);
        controller.settingsRead(f.partition, true); QCOMPARE(controller.state(), Controller::Cancelled); QVERIFY(controller.backupPath().isEmpty());
        controller.proceed(); QCOMPARE(write.count(), 0);
    }
    void writeFailureStopsBoot() {
        Fixture f; Controller controller(&f.objects); QSignalSpy boot(&controller, &Controller::bootRequested);
        controller.start(f.context); controller.settingsRead(f.partition, true); controller.proceed(); controller.writeFinished(false);
        QCOMPARE(controller.state(), Controller::Failed); controller.writeFinished(true); QCOMPARE(boot.count(), 0); QVERIFY(!controller.backupPath().isEmpty());
    }
    void mismatchedReconnect_data() { QTest::addColumn<bool>("identity"); QTest::newRow("wrong-controller") << true; QTest::newRow("wrong-firmware") << false; }
    void mismatchedReconnect() {
        QFETCH(bool, identity); Fixture f; Controller controller(&f.objects);
        controller.start(f.context); controller.settingsRead(f.partition, true); controller.proceed(); controller.writeFinished(true);
        auto descriptor = f.context.firmware.right(100); if (!identity) descriptor[40] = 0;
        controller.firmwareConnected(descriptor, identity ? QString("OTHER") : f.context.serial);
        QCOMPARE(controller.state(), Controller::Failed);
    }
    void bootloaderRevalidatesStorage() {
        Fixture f; f.context.needsBootloader = true; f.context.bootloader = image(); Controller controller(&f.objects);
        controller.start(f.context); controller.settingsRead(f.partition, true); QCOMPARE(controller.state(), Controller::BootloaderDecision);
        controller.proceed(); controller.cancel(); QCOMPARE(controller.state(), Controller::WritingBootloader);
        controller.writeFinished(true); controller.deviceRemoved(); QCOMPARE(controller.state(), Controller::WaitingBootloader);
        controller.bootloaderReady(0x0903, f.context.serial, f.context.codeSize, f.partition.size(), true);
        QCOMPARE(controller.state(), Controller::ReadingSettings); controller.settingsRead(f.partition, true);
        QCOMPARE(controller.state(), Controller::InstallDecision);
    }
    void disconnectedDecisionStopsWrites() {
        Fixture f; Controller controller(&f.objects); QSignalSpy write(&controller, &Controller::writeRequested);
        controller.start(f.context); controller.settingsRead(f.partition, true); controller.deviceRemoved(); controller.proceed();
        QCOMPARE(controller.state(), Controller::Failed); QCOMPARE(write.count(), 0);
    }
    void disconnectDuringTransfer_data() {
        QTest::addColumn<bool>("writing"); QTest::newRow("backup") << false; QTest::newRow("firmware") << true;
    }
    void disconnectDuringTransfer() {
        QFETCH(bool, writing); Fixture f; Controller controller(&f.objects);
        QSignalSpy boot(&controller, &Controller::bootRequested), write(&controller, &Controller::writeRequested);
        controller.start(f.context);
        if (writing) { controller.settingsRead(f.partition, true); controller.proceed(); }
        controller.deviceRemoved();
        if (writing) controller.writeFinished(true); else controller.settingsRead(f.partition, true);
        QCOMPARE(controller.state(), Controller::Failed); QCOMPARE(boot.count(), 0); QCOMPARE(write.count(), writing ? 1 : 0);
    }
    void failedVerificationRetainsBackup() {
        Fixture f; Controller controller(&f.objects);
        controller.start(f.context); controller.settingsRead(f.partition, true); controller.proceed(); controller.writeFinished(true);
        controller.firmwareConnected(f.context.firmware.right(100), f.context.serial); controller.settingsVerified(false, "readback mismatch");
        QCOMPARE(controller.state(), Controller::Failed); QVERIFY(QFile::exists(controller.backupPath() + "/settings.bin"));
        QCOMPARE(controller.primaryText(), QString("Review configuration")); QVERIFY(controller.canConfigure());
    }
    void renderedStates_data() {
        QTest::addColumn<int>("scenario"); QTest::addColumn<int>("width");
        for (int scenario = 0; scenario < 9; ++scenario)
            for (int width : {480, 840}) QTest::newRow(qPrintable(QString("%1-%2").arg(scenario).arg(width))) << scenario << width;
    }
    void renderedStates() {
        QFETCH(int, scenario); QFETCH(int, width); Fixture f; Controller controller(&f.objects);
        if (scenario >= 5) addLegacyReceiver(f);
        Utils::applyDarkTheme();
        QWidget page; page.setProperty("pageLayout", true);
        auto *layout = new QVBoxLayout(&page); layout->setContentsMargins(24, 24, 24, 24);
        auto *container = Core::FirmwareUi::card(&page);
        auto *content = qobject_cast<QVBoxLayout *>(container->layout());
        content->addWidget(Core::FirmwareUi::label("Firmware maintenance", "heading", container));
        auto *card = new UpgradeAssistantDialog(container); card->bind(&controller); content->addWidget(card);
        layout->addWidget(container); layout->addStretch();
        controller.start(f.context);
        if (scenario == 0) controller.transferProgress(35);
        else {
            if (scenario == 2) { controller.cancel(); controller.settingsRead({}, false); f.context.sourceDescriptor.clear(); controller.start(f.context); }
            controller.settingsRead(f.partition, true);
            if (scenario == 3) { controller.proceed(); controller.writeFinished(false); }
            if (scenario == 4) { controller.proceed(); controller.writeFinished(true); controller.firmwareConnected(f.context.firmware.right(100), f.context.serial); controller.settingsVerified(true); }
            if (scenario >= 6) { controller.proceed(); controller.writeFinished(true); }
            if (scenario == 7) controller.convertedSettingsWritten(true);
            if (scenario == 8) controller.convertedSettingsWritten(false);
        }
        page.resize(width, 600); page.show(); QTest::qWait(30);
        auto *cancel = card->findChild<QPushButton *>("firmwareUpgradeCancel");
        auto *primary = card->findChild<QPushButton *>("firmwareUpgradePrimary");
        auto *detail = card->findChild<QLabel *>("firmwareUpgradeDetail");
        QVERIFY(cancel); QVERIFY(primary); QVERIFY(detail);
        QVERIFY(cancel->icon().isNull()); QVERIFY(primary->icon().isNull()); QVERIFY(primary->property("primary").toBool());
        QVERIFY(cancel->geometry().top() >= detail->geometry().bottom());
        QVERIFY(card->rect().contains(cancel->geometry()));
        if (primary->isVisible()) QVERIFY(card->rect().contains(primary->geometry()));
        QCOMPARE(page.width(), width);
        const QString capture = qEnvironmentVariable("FIRMWARE_UPGRADE_CAPTURE_DIR");
        if (!capture.isEmpty()) { QVERIFY(QDir().mkpath(capture)); QVERIFY(page.grab().save(QDir(capture).filePath(QString("upgrade-%1-%2.png").arg(scenario).arg(width)))); }
    }
};

QTEST_MAIN(FirmwareUpgradeTests)
#include "firmware_upgrade.moc"
