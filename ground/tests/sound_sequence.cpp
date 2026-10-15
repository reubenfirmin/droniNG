/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <soundsequenceplayer.h>
#include <QDataStream>
#include <QFile>
#include <QMediaPlayer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QAudioOutput>

class SoundSequenceTests : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    QStringList files;

    void mute(SoundSequencePlayer &sequence)
    {
        auto player = sequence.findChild<QMediaPlayer *>();
        QVERIFY(player);
        QVERIFY(player->audioOutput());
        player->audioOutput()->setMuted(true);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(directory.isValid());
        for (int index = 0; index < 3; ++index) {
            const QString filename = directory.filePath(QString("clip%1.wav").arg(index));
            QFile file(filename);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QDataStream stream(&file);
            stream.setByteOrder(QDataStream::LittleEndian);
            const QByteArray samples(16000, '\0'); // 500 ms, mono PCM16 at 16 kHz.
            stream.writeRawData("RIFF", 4);
            stream << quint32(36 + samples.size());
            stream.writeRawData("WAVEfmt ", 8);
            stream << quint32(16) << quint16(1) << quint16(1) << quint32(16000)
                   << quint32(32000) << quint16(2) << quint16(16);
            stream.writeRawData("data", 4);
            stream << quint32(samples.size());
            QCOMPARE(stream.writeRawData(samples.constData(), samples.size()), samples.size());
            QCOMPARE(stream.status(), QDataStream::Ok);
            files.append(filename);
        }
    }

    void sequence()
    {
        SoundSequencePlayer player;
        mute(player);
        QSignalSpy started(&player, &SoundSequencePlayer::clipStarted);
        QSignalSpy states(&player, &SoundSequencePlayer::playingChanged);
        QSignalSpy finished(&player, &SoundSequencePlayer::finished);
        QSignalSpy failed(&player, &SoundSequencePlayer::clipFailed);
        int ended = 0;
        connect(player.findChild<QMediaPlayer *>(), &QMediaPlayer::mediaStatusChanged, this,
                [&](QMediaPlayer::MediaStatus status) { ended += status == QMediaPlayer::EndOfMedia; });
        player.play(files);
        QVERIFY(player.isPlaying());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(ended, 3);
        QCOMPARE(started.count(), 3);
        for (int index = 0; index < files.size(); ++index)
            QCOMPARE(started.at(index).at(0).toString(), files.at(index));
        QCOMPARE(states.count(), 2);
        QCOMPARE(states.first().at(0).toBool(), true);
        QCOMPARE(states.last().at(0).toBool(), false);
        QVERIFY(!player.isPlaying());
    }

    void missingMedia()
    {
        SoundSequencePlayer player;
        mute(player);
        QSignalSpy started(&player, &SoundSequencePlayer::clipStarted);
        QSignalSpy failed(&player, &SoundSequencePlayer::clipFailed);
        QSignalSpy finished(&player, &SoundSequencePlayer::finished);
        player.play({files.first(), directory.filePath("missing.wav"), files.last()});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QCOMPARE(started.count(), 3);
        QVERIFY(!failed.isEmpty());
        QCOMPARE(failed.first().at(0).toString(), directory.filePath("missing.wav"));
        QVERIFY(!player.isPlaying());
    }

    void cancellation()
    {
        SoundSequencePlayer player;
        mute(player);
        QSignalSpy started(&player, &SoundSequencePlayer::clipStarted);
        QSignalSpy finished(&player, &SoundSequencePlayer::finished);
        player.play(files);
        QTest::qWait(30);
        player.stop();
        QTest::qWait(650);
        QCOMPARE(started.count(), 1);
        QCOMPARE(finished.count(), 0);
        QVERIFY(!player.isPlaying());
        player.play({});
        QVERIFY(!player.isPlaying());
    }

    void replacement()
    {
        SoundSequencePlayer player;
        mute(player);
        QSignalSpy started(&player, &SoundSequencePlayer::clipStarted);
        QSignalSpy finished(&player, &SoundSequencePlayer::finished);
        player.play(files);
        QTest::qWait(30);
        player.play({files.last()});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QCOMPARE(started.count(), 2);
        QCOMPARE(started.last().at(0).toString(), files.last());
        QVERIFY(!player.isPlaying());
    }
};

QTEST_GUILESS_MAIN(SoundSequenceTests)
#include "sound_sequence.moc"
