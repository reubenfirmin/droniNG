/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SOUNDSEQUENCEPLAYER_H
#define SOUNDSEQUENCEPLAYER_H

#include <QObject>
#include <QStringList>

class QMediaPlayer;

// A notification remains busy across every clip, including asynchronous loading.
class SoundSequencePlayer : public QObject
{
    Q_OBJECT
public:
    explicit SoundSequencePlayer(QObject *parent = nullptr);
    bool isPlaying() const { return m_playing; }
    void play(const QStringList &files);
    void stop();

signals:
    void playingChanged(bool playing);
    void clipStarted(const QString &filename);
    void clipFailed(const QString &filename, const QString &reason);
    void finished();

private:
    void loadCurrent();
    void advance();
    void scheduleAdvance();

    QMediaPlayer *m_player;
    QStringList m_files;
    int m_index = 0;
    quint64 m_generation = 0;
    bool m_playing = false;
    bool m_advanceScheduled = false;
};

#endif
