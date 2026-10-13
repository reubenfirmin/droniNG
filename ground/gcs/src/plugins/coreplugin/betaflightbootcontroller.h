/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "core_global.h"
#include <QIODevice>
#include <QPointer>
#include <QTimer>

namespace Core {
// Reviewed against the pinned Betaflight 3.4.0 OMNIBUSF4SD source. Other
// releases keep the board's manual boot procedure until their commands are reviewed.
class CORE_EXPORT BetaflightBootController final : public QObject {
    Q_OBJECT
public:
    explicit BetaflightBootController(QObject *parent = nullptr);
    void start(QIODevice *device, int timeoutMs = 5000);
    void cancel();
signals:
    void finished(bool sent, const QString &message);
private:
    enum class Phase { Stopped, Identity, CliPrompt, SendingBoot };
    void receive();
    void response(int command, const QByteArray &payload);
    void request(int command);
    void write(const QByteArray &bytes);
    void finish(bool sent, const QString &message);
    void checkWritten();
    Phase m_phase = Phase::Stopped;
    QPointer<QIODevice> m_device;
    QTimer m_timeout;
    QByteArray m_buffer;
    int m_command = 0, m_armBit = -1;
};
}
