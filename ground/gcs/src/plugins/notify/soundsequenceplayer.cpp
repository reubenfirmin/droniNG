/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "soundsequenceplayer.h"

#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif

SoundSequencePlayer::SoundSequencePlayer(QObject *parent)
    : QObject(parent), m_player(new QMediaPlayer(this))
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_player->setAudioOutput(new QAudioOutput(m_player));
#endif
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia)
            scheduleAdvance();
    });
    const auto reportError = [this](QMediaPlayer::Error error) {
        if (error == QMediaPlayer::NoError || !m_playing)
            return;
        emit clipFailed(m_files.at(m_index), m_player->errorString());
        scheduleAdvance();
    };
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(m_player, &QMediaPlayer::errorOccurred, this, reportError);
#else
    connect(m_player, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error),
            this, reportError);
#endif
}

void SoundSequencePlayer::play(const QStringList &files)
{
    stop();
    if (files.isEmpty())
        return;
    m_files = files;
    m_index = 0;
    m_playing = true;
    const quint64 generation = m_generation;
    emit playingChanged(true);
    if (m_playing && generation == m_generation)
        loadCurrent();
}

void SoundSequencePlayer::stop()
{
    ++m_generation; // Invalidate queued end/error events before stopping the backend.
    const bool wasPlaying = m_playing;
    m_playing = false;
    m_advanceScheduled = false;
    m_player->stop();
    m_files.clear();
    if (wasPlaying)
        emit playingChanged(false);
}

void SoundSequencePlayer::loadCurrent()
{
    const quint64 generation = m_generation;
    const QString filename = m_files.at(m_index);
    const QUrl source = QUrl::fromLocalFile(filename);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_player->setSource(source);
#else
    m_player->setMedia(source);
#endif
    if (!m_playing || generation != m_generation)
        return;
    emit clipStarted(filename);
    if (m_playing && generation == m_generation)
        m_player->play();
}

void SoundSequencePlayer::scheduleAdvance()
{
    if (!m_playing || m_advanceScheduled)
        return;
    m_advanceScheduled = true;
    const quint64 generation = m_generation;
    QTimer::singleShot(0, this, [this, generation] {
        if (!m_playing || generation != m_generation)
            return;
        advance();
    });
}

void SoundSequencePlayer::advance()
{
    m_advanceScheduled = false;
    if (++m_index < m_files.size()) {
        loadCurrent();
        return;
    }
    stop();
    emit finished();
}
