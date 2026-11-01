/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "firmwareinstallimage.h"
#include <functional>

namespace Core {
// STM32 AN3155 transport. Only the reviewed F303xC geometry is writable.
// The transport must send/read exactly the requested bytes or return failure.
class CORE_EXPORT RomUart {
public:
    using Send = std::function<bool(const QByteArray &)>;
    using Receive = std::function<QByteArray(int, int)>;
    using Progress = std::function<void(const QString &, int)>;
    RomUart(Send send, Receive receive) : send(send), receive(receive) {}
    bool synchronize();
    bool identify();
    bool read(quint32 address, int size, QByteArray &data);
    bool install(const FirmwareInstallImage &image, const QString &backupRoot, const Progress &progress);
    bool restore(const QString &backup, const QString &backupRoot, const Progress &progress);
    QString error, backupPath;
    QByteArray uid;
private:
    bool transmit(const QByteArray &bytes);
    bool ack(int timeout = 1500);
    bool command(quint8 command);
    bool address(quint32 address);
    bool erase(int page);
    bool write(quint32 address, const QByteArray &data);
    bool snapshot(const QString &root, const Progress &progress);
    bool program(const QByteArray &bytes, const QList<FirmwareInstallImage::Segment> &segments,
                 const Progress &progress);
    bool fail(const QString &message) { error = message; return false; }
    Send send;
    Receive receive;
    bool identified = false, extendedErase = false;
};
}
