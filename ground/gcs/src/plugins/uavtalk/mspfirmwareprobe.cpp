/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mspfirmwareprobe.h"
#include <QRegularExpression>

QByteArray MspFirmwareProbe::request(char command)
{
    QByteArray frame("$M<", 3);
    frame.append(char(0));
    frame.append(command);
    frame.append(command); // XOR checksum of the zero size and command.
    return frame;
}

QByteArray MspFirmwareProbe::start()
{
    *this = MspFirmwareProbe();
    m_active = true;
    return request(1); // MSP_API_VERSION
}

void MspFirmwareProbe::stop()
{
    m_active = false;
    m_buffer.clear();
}

QByteArray MspFirmwareProbe::receive(const QByteArray &bytes)
{
    if (!m_active)
        return {};
    QByteArray requests;
    // Process one byte at a time to bound storage even for a large noisy read.
    for (char byte : bytes) {
        m_buffer.append(byte);
        while (!m_buffer.isEmpty()) {
            if (!QByteArray("$M>").startsWith(m_buffer.left(3))) {
                m_buffer.remove(0, 1);
                continue;
            }
            if (m_buffer.size() < 6)
                break;
            const int size = quint8(m_buffer[3]);
            const int frameSize = size + 6;
            if (m_buffer.size() < frameSize)
                break;
            quint8 checksum = 0;
            for (int i = 3; i < frameSize - 1; ++i)
                checksum ^= quint8(m_buffer[i]);
            if (checksum != quint8(m_buffer[frameSize - 1])) {
                m_buffer.remove(0, 1);
                continue;
            }
            const quint8 command = quint8(m_buffer[4]);
            const QByteArray payload = m_buffer.mid(5, size);
            m_buffer.remove(0, frameSize);
            if (command == 1 && size == 3 && !m_responded) {
                m_responded = true;
                m_api = QString("%1.%2").arg(quint8(payload[1])).arg(quint8(payload[2]));
                // Do not make further requests for an unsupported API major.
                if (quint8(payload[1]) == 1) {
                    m_requestedIdentity = true;
                    requests += request(2); // MSP_FC_VARIANT
                    requests += request(3); // MSP_FC_VERSION
                    requests += request(4); // MSP_BOARD_INFO (read-only)
                }
            } else if (m_requestedIdentity && command == 2 && size == 4
                       && QRegularExpression("^[A-Z0-9]{4}$")
                           .match(QString::fromLatin1(payload)).hasMatch()) {
                m_variant = QString::fromLatin1(payload);
            } else if (m_requestedIdentity && command == 3 && size == 3) {
                m_version = QString("%1.%2.%3").arg(quint8(payload[0]))
                    .arg(quint8(payload[1])).arg(quint8(payload[2]));
            } else if (m_requestedIdentity && command == 4 && size >= 6
                       && QRegularExpression("^[A-Z0-9]{4}$")
                           .match(QString::fromLatin1(payload.left(4))).hasMatch()) {
                m_boardTarget = QString::fromLatin1(payload.left(4));
                // Newer BOARD_INFO replies also include the full target name.
                if (size >= 9) {
                    const int length = quint8(payload[8]);
                    if (length > 0 && length <= size - 9) {
                        const QString target = QString::fromLatin1(payload.mid(9, length));
                        if (QRegularExpression("^[A-Z0-9_-]+$").match(target).hasMatch())
                            m_boardTarget = target;
                    }
                }
            }
        }
    }
    return requests;
}

QString MspFirmwareProbe::description() const
{
    if (!m_responded)
        return {};
    QString family = m_variant;
    if (family == "BTFL")
        family = "Betaflight";
    else if (family == "CLFL")
        family = "Cleanflight";
    else if (family == "INAV")
        family = "INAV";
    else if (family == "DRON")
        family = "dRonin";
    return QString("MSP API %1; firmware %2%3%4").arg(m_api,
        family.isEmpty() ? QString("unidentified") : family,
        m_version.isEmpty() ? QString() : QString(" %1").arg(m_version),
        m_boardTarget.isEmpty() ? QString() : QString("; board target %1").arg(m_boardTarget));
}
