/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "uartinstaller.h"
#include "romuart.h"
#include "idevice.h"
#include <QElapsedTimer>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QStandardPaths>
#ifdef Q_OS_UNIX
#include <sys/ioctl.h>
#include <termios.h>
#endif

using namespace Core;
UartInstaller::~UartInstaller()
{
    cancel();
    if (worker) { worker->wait(); delete worker; }
}
void UartInstaller::cancel() { cancelled = true; }
void UartInstaller::start(const FirmwareInstallImage &image, const QString &expectedPath, const QString &restoreBackup)
{
    if (worker) { cancelled = true; worker->wait(); delete worker; worker = nullptr; }
    cancelled = false;
    worker = QThread::create([this, image, expectedPath, restoreBackup] {
        QElapsedTimer waiting; waiting.start();
        QString lastError;
        while (!cancelled && waiting.elapsed() < 120000) {
            QList<QSerialPortInfo> matches;
            for (const auto &port : QSerialPortInfo::availablePorts())
                if (port.vendorIdentifier() == 0x10c4 && port.productIdentifier() == 0xea60
                    && (expectedPath.isEmpty() || IDevice::usbTopologyForDeviceNode(port.systemLocation()) == expectedPath))
                    matches.append(port);
            if (matches.size() > 1) {
                emit finished(false, tr("More than one CP2102 bridge is connected. Disconnect other bridges and retry.")); return;
            }
            if (matches.isEmpty()) { QThread::msleep(200); continue; }
            const auto path = IDevice::usbTopologyForDeviceNode(matches.first().systemLocation());
            if (path.isEmpty()) {
                emit finished(false, tr("Cannot identify the bridge's USB port. Reconnect the SP Racing F3 to a local USB port and retry.")); return;
            }
            QSerialPort serial(matches.first());
            serial.setBaudRate(115200); serial.setDataBits(QSerialPort::Data8);
            serial.setParity(QSerialPort::EvenParity); serial.setStopBits(QSerialPort::OneStop);
            serial.setFlowControl(QSerialPort::NoFlowControl);
            if (!serial.open(QIODevice::ReadWrite)) {
                if (serial.error() == QSerialPort::PermissionError) {
                    emit accessDenied(); return;
                }
                lastError = tr("Cannot open %1: %2. Close other serial applications. Check access with ls -l %1; after correcting access, reconnect USB and retry.")
                    .arg(matches.first().systemLocation(), serial.errorString());
                // Permission and lock failures need recovery, not a two-minute wait.
                emit finished(false, lastError); return;
            }
#ifdef Q_OS_UNIX
            if (ioctl(int(serial.handle()), TIOCEXCL) < 0) {
                emit finished(false, tr("Cannot reserve the serial port exclusively. Close other serial applications and retry.")); return;
            }
#endif
            serial.clear();
            auto send = [this, &serial](const QByteArray &bytes) {
                if (cancelled || serial.write(bytes) != bytes.size()) return false;
                QElapsedTimer deadline; deadline.start();
                while (!cancelled && serial.bytesToWrite() && deadline.elapsed() < 2000)
                    if (!serial.waitForBytesWritten(200) && serial.error() != QSerialPort::TimeoutError) return false;
                return !cancelled && !serial.bytesToWrite();
            };
            auto receive = [this, &serial](int size, int timeout) {
                QByteArray bytes;
                QElapsedTimer deadline; deadline.start();
                while (!cancelled && bytes.size() < size && deadline.elapsed() < timeout) {
                    bytes += serial.read(size - bytes.size());
                    const int remaining = timeout - int(deadline.elapsed());
                    if (bytes.size() < size && remaining > 0) serial.waitForReadyRead(qMin(200, remaining));
                }
                return bytes;
            };
            RomUart protocol(send, receive);
            if (!protocol.synchronize()) {
                serial.close(); QThread::msleep(200); continue;
            }
            emit connected(path);
            if (!protocol.identify()) { emit finished(false, protocol.error); return; }
            emit backingUp();
            bool startedWriting = false, reportedBackup = false;
            QString previousTask; int previousPercent = -1;
            auto progress = [this, &protocol, &startedWriting, &reportedBackup, &previousTask, &previousPercent](const QString &task, int percent) {
                if (!protocol.backupPath.isEmpty() && !reportedBackup) {
                    reportedBackup = true; emit backupSaved(protocol.backupPath);
                }
                if (reportedBackup && !startedWriting) { startedWriting = true; emit writing(); }
                if (previousTask != task || previousPercent != percent) {
                    previousTask = task; previousPercent = percent; emit this->progress(task, percent);
                }
            };
            const auto root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/firmware-backups";
            const bool success = restoreBackup.isEmpty() ? protocol.install(image, root, progress)
                                                         : protocol.restore(restoreBackup, root, progress);
            if (!protocol.backupPath.isEmpty() && !reportedBackup) emit backupSaved(protocol.backupPath);
            serial.close();
            if (!cancelled) emit finished(success, success
                ? tr("Flash verified. Disconnect USB and the flight battery, remove the BOOT bridge, then reconnect USB to the same port.")
                : protocol.error.isEmpty() ? tr("Serial transfer failed. Reconnect with BOOT connected and retry.") : protocol.error);
            return;
        }
        if (!cancelled) emit finished(false, tr("The STM32 serial bootloader did not answer. Disconnect USB and the flight battery, bridge the BOOT pads, reconnect USB to the same port and click Retry."));
    });
    worker->start();
}
