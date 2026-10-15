/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "betaflightbootcontroller.h"
#include <QtEndian>

using namespace Core;

BetaflightBootController::BetaflightBootController(QObject *parent) : QObject(parent)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        finish(false, tr("The controller did not answer the restart request. Reconnect USB and try again, or use the BOOT procedure below."));
    });
}
void BetaflightBootController::cancel()
{
    m_timeout.stop();
    if (m_device) disconnect(m_device, nullptr, this, nullptr);
    m_device.clear(); m_buffer.clear(); m_phase = Phase::Stopped;
}
void BetaflightBootController::start(QIODevice *device, int timeoutMs)
{
    cancel(); m_armBit = -1;
    if (!device || !device->isOpen() || !device->isReadable() || !device->isWritable()) {
        finish(false, tr("The serial connection could not be opened. Use the BOOT procedure below.")); return;
    }
    m_device = device;
    device->readAll(); // Discard data from the earlier detection session.
    connect(device, &QIODevice::readyRead, this, &BetaflightBootController::receive);
    connect(device, &QIODevice::bytesWritten, this, &BetaflightBootController::checkWritten);
    connect(device, &QObject::destroyed, this, [this] {
        if (m_phase != Phase::Stopped) finish(false, tr("The serial connection was lost before the restart command was sent."));
    });
    m_phase = Phase::Identity; m_timeout.start(timeoutMs);
    request(1);
}
void BetaflightBootController::finish(bool sent, const QString &message)
{
    cancel(); emit finished(sent, message);
}
void BetaflightBootController::write(const QByteArray &bytes)
{
    if (!m_device || m_device->write(bytes) != bytes.size())
        finish(false, tr("The restart command could not be sent. Reconnect USB and try again."));
}
void BetaflightBootController::request(int command)
{
    m_command = command;
    QByteArray frame("$M<", 3); frame.append(char(0)); frame.append(char(command)); frame.append(char(command));
    write(frame);
}
void BetaflightBootController::checkWritten()
{
    if (m_phase == Phase::SendingBoot && m_device && m_device->bytesToWrite() == 0)
        finish(true, tr("Restart command sent. Waiting for USB bootloader…"));
}
void BetaflightBootController::receive()
{
    if (!m_device) return;
    const auto bytes = m_device->readAll();
    for (char byte : bytes) {
        if (m_phase == Phase::Stopped || m_phase == Phase::SendingBoot) return;
        m_buffer.append(byte);
        if (m_phase == Phase::CliPrompt) {
            m_buffer = m_buffer.right(512);
            if (m_buffer.contains("Entering CLI Mode") && m_buffer.endsWith("\r\n# ")) {
                m_phase = Phase::SendingBoot;
                // Betaflight 3.4 MSP_REBOOT only restarts normally. Its CLI `bl`
                // stops PWM outputs and calls systemResetToBootloader().
                write("bl\n");
                QTimer::singleShot(0, this, &BetaflightBootController::checkWritten);
            }
            continue;
        }
        while (!m_buffer.isEmpty()) {
            if (!QByteArray("$M>").startsWith(m_buffer.left(3))) { m_buffer.remove(0, 1); continue; }
            if (m_buffer.size() < 6) break;
            const int size = quint8(m_buffer[3]), length = size + 6;
            if (m_buffer.size() < length) break;
            quint8 checksum = 0;
            for (int i = 3; i < length - 1; ++i) checksum ^= quint8(m_buffer[i]);
            if (checksum != quint8(m_buffer[length - 1])) { m_buffer.remove(0, 1); continue; }
            const int command = quint8(m_buffer[4]); const auto payload = m_buffer.mid(5, size);
            m_buffer.remove(0, length);
            if (command == m_command) response(command, payload);
            if (m_phase != Phase::Identity) break;
        }
    }
}
void BetaflightBootController::response(int command, const QByteArray &payload)
{
    if (command == 1 && payload == QByteArray::fromHex("000127")) request(2);
    else if (command == 2 && payload == "BTFL") request(3);
    else if (command == 3 && payload == QByteArray::fromHex("030400")) request(4);
    else if (command == 4 && payload.size() >= 6 && payload.left(4) == "OBSD") {
        if (payload.size() >= 9) {
            const int length = quint8(payload[8]);
            if (length && (length > payload.size() - 9 || payload.mid(9, length) != "OMNIBUSF4SD")) {
                finish(false, tr("The connected board's identity changed. Reconnect it and check the firmware selection.")); return;
            }
        }
        request(119); // MSP_BOXIDS: find the ARM bit in the current mode ordering.
    } else if (command == 119 && !payload.isEmpty() && payload.size() <= 32 && payload.count(char(0)) == 1) {
        m_armBit = payload.indexOf(char(0)); request(101); // Fresh MSP_STATUS, after identity verification.
    } else if (command == 101 && payload.size() >= 11 && m_armBit >= 0) {
        const auto flags = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData() + 6));
        if (flags & (quint32(1) << m_armBit)) {
            finish(false, tr("Disarm the controller before installing firmware. Then retry.")); return;
        }
        m_buffer.clear(); m_phase = Phase::CliPrompt;
        write("#"); // CLI entry itself is also gated on disarmed state upstream.
    } else finish(false, tr("The controller's firmware could not be verified for automatic restart. Use the BOOT procedure below."));
}
