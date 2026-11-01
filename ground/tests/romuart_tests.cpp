/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <coreplugin/romuart.h>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
using namespace Core;

// Stateful wire emulator: validates AN3155 complements, addresses, lengths and
// checksums independently, and rejects every command outside the reviewed set.
struct BootRom {
    QByteArray flash = QByteArray(262144, char(0xa5)), queued;
    QByteArray uid = QByteArray::fromHex("0123456789abcdef10203040");
    enum State { Command, Address, ReadLength, WriteData, ErasePages } state = Command;
    int active = 0, pid = 0x422, kib = 256, reads = 0, nack = -1;
    quint32 address = 0;
    bool extended = true, corruptBackup = false, corruptWritten = false, shortRead = false;
    QList<int> erased;
    std::function<bool()> beforeErase;
    void ack() { queued.append(char(0x79)); }
    static bool checksum(const QByteArray &bytes) {
        quint8 sum = 0; for (auto byte : bytes) sum ^= quint8(byte); return sum == 0;
    }
    bool send(const QByteArray &bytes) {
        if (bytes.isEmpty()) return false;
        if (state == Command) {
            if (bytes == QByteArray(1, char(0x7f))) { ack(); return true; }
            if (bytes.size() != 2 || quint8(bytes[0]) != (quint8(bytes[1]) ^ 0xff)) return false;
            active = quint8(bytes[0]);
            if (active == nack) { queued.append(char(0x1f)); return true; }
            ack();
            if (active == 0) queued += QByteArray::fromHex(extended ? "064100021131214479" : "064100021131214379");
            else if (active == 2) { queued.append(char(1)); queued.append(char(pid >> 8)); queued.append(char(pid)); ack(); }
            else if (active == 0x11 || active == 0x31) state = Address;
            else if (active == 0x43 || active == 0x44) state = ErasePages;
            else return false;
        } else if (state == Address) {
            if (bytes.size() != 5 || !checksum(bytes)) return false;
            address = qFromBigEndian<quint32>(bytes.constData());
            state = active == 0x11 ? ReadLength : WriteData; ack();
        } else if (state == ReadLength) {
            if (bytes.size() != 2 || quint8(bytes[0]) != (quint8(bytes[1]) ^ 0xff)) return false;
            const int size = quint8(bytes[0]) + 1;
            QByteArray data;
            if (address == 0x1ffff7cc) { data.resize(2); qToLittleEndian<quint16>(kib, data.data()); }
            else if (address == 0x1ffff7ac) data = uid;
            else if (address >= 0x08000000 && address + size <= 0x08040000) {
                data = flash.mid(address - 0x08000000, size);
                if ((corruptBackup && ++reads > 1024) || (corruptWritten && !erased.isEmpty())) data[0] ^= 1;
            } else return false;
            if (data.size() != size) return false;
            ack(); queued += data; state = Command;
        } else if (state == WriteData) {
            if (!checksum(bytes) || quint8(bytes[0]) + 3 != bytes.size()
                || address < 0x08000000 || address + bytes.size() - 2 > 0x08040000) return false;
            flash.replace(address - 0x08000000, bytes.size() - 2, bytes.mid(1, bytes.size() - 2));
            ack(); state = Command;
        } else if (state == ErasePages) {
            if (!checksum(bytes) || (extended ? bytes.size() != 5 || bytes.left(2) != QByteArray(2, char(0))
                                              : bytes.size() != 3 || bytes[0] != 0)) return false;
            const int page = extended ? qFromBigEndian<quint16>(bytes.constData() + 2) : quint8(bytes[1]);
            if (page >= 128 || (beforeErase && !beforeErase())) return false;
            erased.append(page); flash.replace(page * 2048, 2048, QByteArray(2048, char(0xff)));
            ack(); state = Command;
        }
        return true;
    }
    QByteArray receive(int size, int) {
        if (shortRead || queued.size() < size) { queued.clear(); return {}; }
        const auto result = queued.left(size); queued.remove(0, size); return result;
    }
};

class RomUartTests : public QObject {
    Q_OBJECT
    const RomUart::Progress progress = [](const QString &, int) {};
    FirmwareInstallImage image() {
        FirmwareInstallImage result;
        result.profileId = "spracingf3"; result.base = 0x08000000;
        result.bytes = QByteArray(262144, char(0x42));
        result.segments = {{0, 2048}, {2048, 2048}, {36864, 225280}};
        return result;
    }
private slots:
    void identity_data() {
        QTest::addColumn<int>("pid"); QTest::addColumn<int>("kib"); QTest::addColumn<bool>("allowed");
        QTest::newRow("F303xC") << 0x422 << 256 << true;
        QTest::newRow("F303xB-too-small") << 0x422 << 128 << false;
        QTest::newRow("F303xE-different-geometry") << 0x446 << 512 << false;
        QTest::newRow("F405") << 0x413 << 1024 << false;
    }
    void identity() {
        QFETCH(int, pid); QFETCH(int, kib); QFETCH(bool, allowed);
        BootRom device; device.pid = pid; device.kib = kib;
        RomUart protocol([&](const QByteArray &bytes) { return device.send(bytes); }, [&](int n, int t) { return device.receive(n, t); });
        QVERIFY(protocol.synchronize()); QCOMPARE(protocol.identify(), allowed); QVERIFY(device.erased.isEmpty());
    }
    void installAndRestore_data() { QTest::addColumn<bool>("extended"); QTest::newRow("extended-erase") << true; QTest::newRow("legacy-erase") << false; }
    void installAndRestore() {
        QFETCH(bool, extended);
        BootRom device; device.extended = extended;
        const auto original = device.flash;
        QTemporaryDir root;
        device.beforeErase = [&] {
            QDirIterator iterator(root.path(), {"flash.bin"}, QDir::Files, QDirIterator::Subdirectories);
            if (!iterator.hasNext()) return false;
            const auto path = iterator.next(); QFile file(path), metadata(path + ".json");
            return file.open(QIODevice::ReadOnly) && file.readAll() == original && metadata.open(QIODevice::ReadOnly);
        };
        RomUart protocol([&](const QByteArray &bytes) { return device.send(bytes); }, [&](int n, int t) { return device.receive(n, t); });
        QVERIFY(protocol.synchronize()); QVERIFY2(protocol.identify(), qPrintable(protocol.error));
        QVERIFY2(protocol.install(image(), root.path(), progress), qPrintable(protocol.error));
        QCOMPARE(device.flash.mid(4096, 32768), original.mid(4096, 32768));
        QCOMPARE(device.flash.left(4096), image().bytes.left(4096));
        QCOMPARE(device.flash.mid(36864), image().bytes.mid(36864));
        QCOMPARE(device.erased.size(), 112); QCOMPARE(device.erased.last(), 0);
        for (int page = 2; page < 18; ++page) QVERIFY(!device.erased.contains(page));
        const auto backup = protocol.backupPath;
        QVERIFY(!backup.isEmpty()); device.beforeErase = {};
        QVERIFY2(protocol.restore(backup, root.path(), progress), qPrintable(protocol.error));
        QCOMPARE(device.flash, original);
    }
    void refusesBeforeErase_data() {
        QTest::addColumn<QString>("failure");
        for (const auto &failure : {"read-protected", "inconsistent-backup", "backup-storage", "unsafe-segments", "short-response", "wrong-backup"})
            QTest::newRow(failure) << QString(failure);
    }
    void refusesBeforeErase() {
        QFETCH(QString, failure);
        BootRom device; QTemporaryDir root;
        RomUart protocol([&](const QByteArray &bytes) { return device.send(bytes); }, [&](int n, int t) { return device.receive(n, t); });
        QVERIFY(protocol.synchronize()); QVERIFY(protocol.identify());
        auto firmware = image(); QString destination = root.path();
        if (failure == "read-protected") device.nack = 0x11;
        if (failure == "inconsistent-backup") device.corruptBackup = true;
        if (failure == "backup-storage") destination = root.filePath("blocked/child");
        if (failure == "backup-storage") { QFile file(root.filePath("blocked")); QVERIFY(file.open(QIODevice::WriteOnly)); }
        if (failure == "unsafe-segments") firmware.segments[2].offset = 4096;
        if (failure == "short-response") device.shortRead = true;
        if (failure == "wrong-backup") QVERIFY(!protocol.restore(root.filePath("missing.bin"), destination, progress));
        else QVERIFY(!protocol.install(firmware, destination, progress));
        QVERIFY(device.erased.isEmpty()); QVERIFY(!protocol.error.isEmpty());
    }
    void stopsOnReadbackFailure() {
        BootRom device; device.corruptWritten = true; QTemporaryDir root;
        RomUart protocol([&](const QByteArray &bytes) { return device.send(bytes); }, [&](int n, int t) { return device.receive(n, t); });
        QVERIFY(protocol.synchronize()); QVERIFY(protocol.identify());
        QVERIFY(!protocol.install(image(), root.path(), progress));
        QCOMPARE(device.erased, QList<int>{18}); QVERIFY(!protocol.backupPath.isEmpty());
        QVERIFY(protocol.error.contains("readback"));
    }
    void refusesDamagedOrForeignBackup_data() {
        QTest::addColumn<QString>("damage");
        for (const auto &damage : {"identity", "checksum", "missing-metadata"})
            QTest::newRow(damage) << QString(damage);
    }
    void refusesDamagedOrForeignBackup() {
        QFETCH(QString, damage);
        BootRom device; QTemporaryDir root;
        RomUart protocol([&](const QByteArray &bytes) { return device.send(bytes); }, [&](int n, int t) { return device.receive(n, t); });
        QVERIFY(protocol.synchronize()); QVERIFY(protocol.identify());
        QVERIFY(protocol.install(image(), root.path(), progress));
        const auto backup = protocol.backupPath;
        if (damage == "identity") { device.uid[0] ^= 1; QVERIFY(protocol.identify()); }
        if (damage == "checksum") {
            QFile file(backup); QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(262144, char(0x17))), qint64(262144)); file.close();
        }
        if (damage == "missing-metadata") QVERIFY(QFile::remove(backup + ".json"));
        device.erased.clear(); const auto before = device.flash;
        QVERIFY(!protocol.restore(backup, root.path(), progress));
        QVERIFY(!protocol.error.isEmpty()); QVERIFY(device.erased.isEmpty()); QCOMPARE(device.flash, before);
    }
    void failedSerialWriteExplainsRecovery() {
        RomUart protocol([](const QByteArray &) { return false; }, [](int, int) { return QByteArray(); });
        QVERIFY(!protocol.synchronize()); QVERIFY(protocol.error.contains("serial write"));
    }
};
QTEST_GUILESS_MAIN(RomUartTests)
#include "romuart_tests.moc"
