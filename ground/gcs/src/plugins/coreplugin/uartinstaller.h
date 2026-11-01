/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "firmwareinstallimage.h"
#include <QObject>
#include <QThread>
#include <atomic>
namespace Core {
class CORE_EXPORT UartInstaller : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~UartInstaller() override;
    void start(const FirmwareInstallImage &image, const QString &usbPath, const QString &restoreBackup = {});
    void cancel();
signals:
    void connected(const QString &usbPath);
    void backingUp();
    void writing();
    void progress(const QString &task, int percent);
    void backupSaved(const QString &path);
    void accessDenied();
    void finished(bool success, const QString &message);
private:
    QThread *worker = nullptr;
    std::atomic_bool cancelled{false};
};
}
