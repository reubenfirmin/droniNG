/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FIRMWAREBOOTCONTROLLER_H
#define FIRMWAREBOOTCONTROLLER_H
#include "tl_dfu.h"
#include "uploader_global.h"
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>

namespace Core { class ConnectionManager; }
namespace uploader {
class UPLOADER_EXPORT FirmwareBootTransport {
public:
    virtual ~FirmwareBootTransport() = default;
    virtual bool open(const USBPortInfo &port) = 0;
    virtual tl_dfu::device capabilities() = 0;
    virtual QByteArray description(int size) = 0;
    virtual bool start() = 0;
};

class UPLOADER_EXPORT FirmwareBootController : public QObject {
    Q_OBJECT
public:
    explicit FirmwareBootController(Core::ConnectionManager *manager, QObject *parent = nullptr);
    ~FirmwareBootController() override;
    static bool startVerifiedFirmware(FirmwareBootTransport &transport, const USBPortInfo &port,
        const QByteArray &descriptor, const std::function<bool()> &cancelled, QString *error);
private:
    void start(const QString &usbPath, const QByteArray &descriptor);
    void cancel();
    QPointer<Core::ConnectionManager> m_manager;
    QPointer<QThread> m_thread;
    std::shared_ptr<std::atomic_bool> m_cancelled;
};
}
#endif
