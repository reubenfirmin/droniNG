/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MSPFIRMWAREPROBE_H
#define MSPFIRMWAREPROBE_H

#include <QByteArray>
#include <QString>
#include "uavtalk_global.h"

// A bounded MSP v1 identity probe. It never reads the device independently
// of UAVTalk and only requests API version, firmware variant/version and board identity.
class UAVTALK_EXPORT MspFirmwareProbe
{
public:
    QByteArray start();
    QByteArray receive(const QByteArray &bytes);
    void stop();
    bool active() const { return m_active; }
    bool responded() const { return m_responded; }
    bool complete() const { return !m_variant.isEmpty() && !m_version.isEmpty() && !m_boardTarget.isEmpty(); }
    QString variant() const { return m_variant; }
    QString version() const { return m_version; }
    QString boardTarget() const { return m_boardTarget; }
    QString description() const;

private:
    static QByteArray request(char command);
    bool m_active = false;
    bool m_responded = false;
    bool m_requestedIdentity = false;
    QByteArray m_buffer;
    QString m_api;
    QString m_variant;
    QString m_version;
    QString m_boardTarget;
};

#endif
