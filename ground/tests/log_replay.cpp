#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <logging/logfile.h>

class LogReplayTests : public QObject
{
    Q_OBJECT
    const QByteArray first{"first\0packet", 12};
    const QByteArray second = QString::fromUtf8("actitud café 日本語").toUtf8();
    const QByteArray third = QByteArray(1024, '\xff');

    void checkReplay(const QString &filename)
    {
        LogFile reader;
        reader.setFileName(filename);
        QVERIFY(reader.open(QIODevice::ReadOnly));
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
        checkReplay(reference);
    }
};

QTEST_MAIN(LogReplayTests)
#include "log_replay.moc"
