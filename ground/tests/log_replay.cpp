#include <QDir>
#include <QMessageBox>
#include <QPluginLoader>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <logging/logfile.h>
#include <extensionsystem/iplugin.h>
#include <extensionsystem/pluginmanager.h>
#include <utils/pathutils.h>

class LogReplayTests : public QObject
{
    Q_OBJECT
    const QByteArray first{"first\0packet", 12};
    const QByteArray second = QString::fromUtf8("actitud café 日本語").toUtf8();
    const QByteArray third = QByteArray(1024, '\xff');
    QTemporaryDir settingsDirectory;
    ExtensionSystem::PluginManager plugins;
    QPluginLoader core{QString::fromUtf8(CORE_PLUGIN_FILE)};

    void writeTimedLog(const QString &filename)
    {
        LogFile writer;
        writer.setFileName(filename);
        QVERIFY(writer.open(QIODevice::WriteOnly));
        writer.close();

        QFile bytes(filename);
        QVERIFY(bytes.open(QIODevice::Append));
        quint32 timestamp = 1000;
        for (const QByteArray &packet : {first, second, third}) {
            const quint32 wireTimestamp = qToLittleEndian(timestamp);
            const qint64 wireLength = qToLittleEndian(qint64(packet.size()));
            QCOMPARE(bytes.write(reinterpret_cast<const char *>(&wireTimestamp), sizeof(wireTimestamp)),
                     qint64(sizeof(wireTimestamp)));
            QCOMPARE(bytes.write(reinterpret_cast<const char *>(&wireLength), sizeof(wireLength)),
                     qint64(sizeof(wireLength)));
            QCOMPARE(bytes.write(packet), qint64(packet.size()));
            timestamp += 400;
        }
    }

    void checkReplay(const QString &filename, const QString &expectedWarning = {})
    {
        LogFile reader;
        reader.setFileName(filename);
        QStringList warnings;
        QTimer warningTimer;
        connect(&warningTimer, &QTimer::timeout, this, [&] {
            if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                warnings.append(dialog->text());
                dialog->accept();
            }
        });
        warningTimer.start(10);
        QVERIFY(reader.open(QIODevice::ReadOnly));
        warningTimer.stop();
        QCOMPARE(warnings, expectedWarning.isEmpty() ? QStringList{}
                                                    : QStringList{expectedWarning});
        QByteArray received;
        connect(&reader, &QIODevice::readyRead, this, [&] { received += reader.readAll(); });
        QSignalSpy finished(&reader, &LogFile::replayFinished);
        QVERIFY(reader.startReplay());
        QTRY_VERIFY(!received.isEmpty());
        reader.pauseReplay();
        const QByteArray paused = received;
        QTest::qWait(80);
        QCOMPARE(received, paused);
        reader.setReplaySpeed(4);
        reader.resumeReplay();
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(received, first + second + third);
        QVERIFY(!reader.isOpen());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(settingsDirectory.isValid());
        Utils::PathUtils().setSettingsFilename(settingsDirectory.filePath("settings.xml"));
        auto *plugin = qobject_cast<ExtensionSystem::IPlugin *>(core.instance());
        QVERIFY2(plugin, qPrintable(core.errorString()));
        QString error;
        QVERIFY2(plugin->initialize({}, &error), qPrintable(error));
        plugins.loadPlugins();
        QVERIFY(plugins.allPluginsLoaded());
    }

    void seekWithoutReplay()
    {
        LogFile reader;
        reader.setReplayTime(0);
        reader.setReplayTime(1000);
        QVERIFY(!reader.isOpen());
    }

    void seekReplaysExactlyOnce_data()
    {
        QTest::addColumn<double>("seconds");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("before-start") << -0.5 << first + second + third;
        QTest::newRow("start") << 0.0 << first + second + third;
        QTest::newRow("exact-second") << 0.4 << second + third;
        QTest::newRow("between-packets") << 0.5 << third;
        QTest::newRow("last-packet") << 0.8 << third;
        QTest::newRow("past-end") << 1000.0 << third;
    }

    void seekReplaysExactlyOnce()
    {
        QFETCH(double, seconds);
        QFETCH(QByteArray, expected);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString filename = temporary.filePath("seek.drlog");
        writeTimedLog(filename);

        LogFile reader;
        reader.setFileName(filename);
        QVERIFY(reader.open(QIODevice::ReadOnly));
        QByteArray received;
        connect(&reader, &QIODevice::readyRead, this, [&] { received += reader.readAll(); });
        QSignalSpy finished(&reader, &LogFile::replayFinished);
        QVERIFY(reader.startReplay());
        reader.pauseReplay();
        reader.setReplayTime(seconds);
        reader.setReplaySpeed(20);
        reader.resumeReplay();
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(received, expected);
        QVERIFY(!reader.isOpen());
        reader.setReplayTime(0);
        QCOMPARE(finished.count(), 1);
    }

    void seekDiscardsUnreadData()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString filename = temporary.filePath("buffered-seek.drlog");
        writeTimedLog(filename);
        LogFile reader;
        reader.setFileName(filename);
        QVERIFY(reader.open(QIODevice::ReadOnly));
        QByteArray received;
        bool firstPacket = true;
        connect(&reader, &QIODevice::readyRead, this, [&] {
            if (firstPacket) {
                received += reader.read(1);
                reader.pauseReplay();
                firstPacket = false;
            } else {
                received += reader.readAll();
            }
        });
        QSignalSpy finished(&reader, &LogFile::replayFinished);
        QVERIFY(reader.startReplay());
        QTRY_COMPARE(received, first.left(1));
        received.clear();
        reader.setReplayTime(0.4);
        reader.setReplaySpeed(20);
        reader.resumeReplay();
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(received, second + third);
    }

    void seekPreservesPacketTiming()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString filename = temporary.filePath("timed-seek.drlog");
        writeTimedLog(filename);
        LogFile reader;
        reader.setFileName(filename);
        QVERIFY(reader.open(QIODevice::ReadOnly));
        QByteArray received;
        connect(&reader, &QIODevice::readyRead, this, [&] { received += reader.readAll(); });
        QSignalSpy finished(&reader, &LogFile::replayFinished);
        QVERIFY(reader.startReplay());
        reader.pauseReplay();
        reader.setReplayTime(0.4);
        reader.resumeReplay();
        QTRY_VERIFY(!received.isEmpty());
        QCOMPARE(received, second);
        QTest::qWait(120);
        QCOMPARE(received, second);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(received, second + third);
    }

    void headerWarnings_data()
    {
        QTest::addColumn<bool>("changeDefinitions");
        QTest::addColumn<QString>("warning");
        QTest::newRow("revision") << false << QStringLiteral("Possible log file incompatibility.");
        QTest::newRow("definitions") << true << QStringLiteral("Likely log file incompatibility.");
    }

    void headerWarnings()
    {
        QFETCH(bool, changeDefinitions);
        QFETCH(QString, warning);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString filename = temporary.filePath("changed-header.drlog");
        writeTimedLog(filename);
        QFile file(filename);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray contents = file.readAll();
        file.close();
        auto begin = contents.indexOf('\n') + 1;
        if (changeDefinitions)
            begin = contents.indexOf('\n', begin) + 1;
        const auto end = contents.indexOf('\n', begin);
        QVERIFY(begin > 0 && end > begin);
        contents.replace(begin, end - begin, changeDefinitions
                             ? QByteArray("0123456789012345678901234567890123456789")
                             : QByteArray("different-revision"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(contents), qint64(contents.size()));
        file.close();
        checkReplay(filename, warning);
    }

    void writeAndReplay()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString filename = temporary.filePath("flight.opl");
        LogFile writer;
        writer.setFileName(filename);
        QVERIFY(writer.open(QIODevice::WriteOnly));
        QCOMPARE(writer.write(first), qint64(first.size()));
        QTest::qWait(200);
        QCOMPARE(writer.write(second), qint64(second.size()));
        QTest::qWait(200);
        QCOMPARE(writer.write(third), qint64(third.size()));
        writer.close();

        QFile bytes(filename);
        QVERIFY(bytes.open(QIODevice::ReadOnly));
        QVERIFY(bytes.readLine().contains("git hash"));
        bytes.readLine();
        bytes.readLine();
        QCOMPARE(bytes.readLine(), QByteArray("##\n"));
        quint32 previous = 0;
        for (const QByteArray &packet : {first, second, third}) {
            quint32 timestamp;
            qint64 length;
            QCOMPARE(bytes.read(reinterpret_cast<char *>(&timestamp), sizeof(timestamp)),
                     qint64(sizeof(timestamp)));
            QCOMPARE(bytes.read(reinterpret_cast<char *>(&length), sizeof(length)), qint64(sizeof(length)));
            QVERIFY(timestamp >= previous);
            previous = timestamp;
            QCOMPARE(length, qint64(packet.size()));
            QCOMPARE(bytes.read(length), packet);
        }
        QVERIFY(previous >= 350);
        QVERIFY(bytes.atEnd());
        bytes.close();
        checkReplay(filename);

        const QString directory = qEnvironmentVariable("GCS_LOG_REFERENCE_DIR");
        QVERIFY(!directory.isEmpty());
        const QString reference = directory + "/flight.opl";
#if QT_VERSION < 0x060000
        QVERIFY(QDir().mkpath(directory));
        QFile::remove(reference);
        QVERIFY(QFile::copy(filename, reference));
#endif
        const QString warning = qEnvironmentVariable("GCS_LOG_EXPECT_WARNING",
            qEnvironmentVariableIsSet("GCS_LOG_EXPECT_VERSION_WARNING")
                ? QStringLiteral("Possible log file incompatibility.") : QString{});
        checkReplay(reference, warning);
    }

    void cleanupTestCase()
    {
        QVERIFY(core.unload());
    }
};

QTEST_MAIN(LogReplayTests)
#include "log_replay.moc"
