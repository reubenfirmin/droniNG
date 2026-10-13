/**
 ******************************************************************************
 *
 * @file       rawhid.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2012-2014
 * @author     dRonin, http://dronin.org Copyright (C) 2015
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup RawHIDPlugin Raw HID Plugin
 * @{
 * @brief Impliments a HID USB connection to the flight hardware as a QIODevice
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */

#include "rawhid.h"

#include "rawhid_const.h"
#include "coreplugin/connectionmanager.h"
#include <extensionsystem/pluginmanager.h>
#include <QtGlobal>
#include <QList>
#include <QMutexLocker>
#include <cerrno>
#include <cstring>

class IConnection;

// timeout value used when we want to return directly without waiting
static const int READ_TIMEOUT = 200;
static const int READ_SIZE = 64;

static const int WRITE_SIZE = 64;

static const int WRITE_RETRIES = 10;

static QString hidFailure(const QString &operation, int error)
{
    return QObject::tr("USB %1 failed: %2 (errno %3)")
        .arg(operation, QString::fromLocal8Bit(std::strerror(error ? error : EIO)))
        .arg(error ? error : EIO);
}

static bool transientError(int error)
{
    return error == EINTR || error == EAGAIN || error == EINPROGRESS;
}

RawHIDReadThread::RawHIDReadThread(hid_device *handle, const RawHIDBackend &backend)
    : m_handle(handle)
    , m_backend(backend)
    , m_running(true)
{
}

RawHIDReadThread::~RawHIDReadThread()
{
    stop();
    wait();
}

void RawHIDReadThread::run()
{
    while (m_running) {
        // here we use a temporary buffer so we don't need to lock
        // the mutex while we are reading from the device

        // Want to read in regular chunks that match the packet size the device
        // is using.  In this case it is 64 bytes (the interrupt packet limit)
        // although it would be nice if the device had a different report to
        // configure this
        unsigned char buffer[READ_SIZE] = { 0 };

        errno = 0;
        int ret = m_backend.read(m_handle, buffer, READ_SIZE, READ_TIMEOUT);
        const int error = errno;

        if (ret > 0) // read some data
        {
            // Reject truncated or malformed reports before using their length.
            if (ret < 2 || ret > READ_SIZE || buffer[0] != 1 || buffer[1] > ret - 2)
                continue;
            if (!buffer[1]) continue;
            QMutexLocker lock(&m_readBufMtx);

            bool needSignal = m_readBuffer.size() == 0;

            // Note: Preprocess the USB packets in this OS independent code
            // First byte is report ID, second byte is the number of valid bytes
            m_readBuffer.append(reinterpret_cast<char *>(&buffer[2]), buffer[1]);

            if (needSignal) {
                emit readyToRead();
            }
        } else if (ret == 0) // nothing read
        {
        } else // < 0 => error
        {
            if (transientError(error)) continue;
            m_running = false;
            emit failed(hidFailure(tr("read"), error), error ? error : EIO);
        }
    }
}

int RawHIDReadThread::getReadData(char *data, int size)
{
    QMutexLocker lock(&m_readBufMtx);

    size = qMin(size, m_readBuffer.size());

    memcpy(data, m_readBuffer.constData(), size);
    m_readBuffer.remove(0, size);

    return size;
}

qint64 RawHIDReadThread::getBytesAvailable()
{
    QMutexLocker lock(&m_readBufMtx);
    return m_readBuffer.size();
}

// *********************************************************************************

RawHIDWriteThread::RawHIDWriteThread(hid_device *handle, const RawHIDBackend &backend)
    : m_handle(handle)
    , m_backend(backend)
    , m_running(true)
{
}

RawHIDWriteThread::~RawHIDWriteThread()
{
    stop();
    wait();
}

void RawHIDWriteThread::run()
{
    int retry = 0;
    while (m_running) {
        unsigned char buffer[WRITE_SIZE] = { 0 };
        int size;

        {
            QMutexLocker lock(&m_writeBufMtx);
            size = qMin(WRITE_SIZE - 2, m_writeBuffer.size());

            while (m_running && (size <= 0)) {
                m_newDataToWrite.wait(&m_writeBufMtx);

                size = m_writeBuffer.size();
            }
            if (!m_running) return;

            // NOTE: data size is limited to 2 bytes less than the
            // usb packet size (64 bytes for interrupt) to make room
            // for the reportID and valid data length
            size = qMin(WRITE_SIZE - 2, m_writeBuffer.size());
            memcpy(&buffer[2], m_writeBuffer.constData(), size);

            buffer[0] = 2; // reportID
            buffer[1] = size; // valid data length
        }

        errno = 0;
        int ret = m_backend.write(m_handle, buffer, WRITE_SIZE);
        const int error = errno;

        if (ret == WRITE_SIZE) {
            {
                QMutexLocker lock(&m_writeBufMtx);
                m_writeBuffer.remove(0, size);
            }
            retry = 0;
            emit dataWritten(size);
        } else if (ret < 0 && transientError(error) && ++retry <= WRITE_RETRIES) {
            msleep(40);
        } else {
            m_running = false;
            emit failed(ret < 0 ? hidFailure(tr("write"), error)
                : tr("USB write failed: incomplete HID report (%1 of %2 bytes)").arg(ret).arg(WRITE_SIZE),
                ret < 0 && error ? error : EIO);
            return;
        }
    }
}

//! Tell the thread to stop and make sure it wakes up immediately
void RawHIDWriteThread::stop()
{
    m_running = false;

    QMutexLocker lock(&m_writeBufMtx);
    m_newDataToWrite.wakeOne();
}

int RawHIDWriteThread::pushDataToWrite(const char *data, int size)
{
    QMutexLocker lock(&m_writeBufMtx);
    if (!m_running) return -1;

    m_writeBuffer.append(data, size);
    m_newDataToWrite.wakeOne(); // signal that new data arrived

    return size;
}

qint64 RawHIDWriteThread::getBytesToWrite()
{
    QMutexLocker lock(&m_writeBufMtx);
    return m_writeBuffer.size();
}

// *********************************************************************************

RawHID::RawHID(USBDevice *deviceStructure)
    : RawHID(deviceStructure, {hid_open_path, hid_read_timeout, hid_write, hid_close})
{
}

RawHID::RawHID(USBDevice *deviceStructure, const RawHIDBackend &backend)
    : QIODevice()
    , m_backend(backend)
    , m_deviceInfo(deviceStructure)
    , m_handle(nullptr)
    , m_readThread(NULL)
    , m_writeThread(NULL)
{
}

RawHID::~RawHID()
{
    close();
    hid_exit();
}

bool RawHID::open(OpenMode mode)
{
    if (isOpen()) return false;
    hid_device *handle;

    // Initialize the hidapi library (safe to call multiple times)
    hid_init();

    // Open the device using the VID, PID
    errno = 0;
    handle = m_backend.open(m_deviceInfo->getPath().toLatin1());

    if (handle) {
        m_handle = handle;

        m_writeThread = new RawHIDWriteThread(m_handle, m_backend);
        m_readThread = new RawHIDReadThread(m_handle, m_backend);
        const quint64 generation = ++m_generation;

        // Plumb through read thread's ready read signal to our clients
        connect(m_readThread, &RawHIDReadThread::readyToRead, this, [this, generation] {
            if (generation == m_generation && isOpen()) emit readyRead();
        });
        connect(m_writeThread, &RawHIDWriteThread::dataWritten, this, [this, generation](qint64 bytes) {
            if (generation == m_generation && isOpen()) emit bytesWritten(bytes);
        });
        const auto failed = [this, generation](const QString &details, int error) {
            if (generation == m_generation && isOpen()) workerFailed(details, error);
        };
        connect(m_readThread, &RawHIDReadThread::failed, this, failed);
        connect(m_writeThread, &RawHIDWriteThread::failed, this, failed);

        QIODevice::open(mode);
        m_readThread->start();
        m_writeThread->start();
    } else {
        const int openError = errno;
        setErrorString(tr("%1: %2 (errno %3)").arg(m_deviceInfo->getPath(),
            QString::fromLocal8Bit(std::strerror(openError))).arg(openError));
        qWarning().noquote() << "[RawHID] Failed to open USB device:" << errorString();
        return false;
    }

    return true;
}

void RawHID::workerFailed(const QString &details, int error)
{
    setErrorString(m_deviceInfo->getPath() + ": " + details);
    if (error == ENODEV) qInfo().noquote() << "[RawHID] USB controller disconnected:" << m_deviceInfo->getPath();
    else qWarning().noquote() << "[RawHID]" << errorString();
    emit transportFailed(errorString(), error);
    close();
}

void RawHID::close()
{
    if (!m_handle)
        return;

    RAW_HID_QXTLOG_DEBUG("RawHID: close()");
    ++m_generation;

    m_readThread->stop();
    m_writeThread->stop();
    m_readThread->wait(); /* Block indefinitely for read thread exit */
    m_writeThread->wait();
    delete m_readThread;
    m_readThread = NULL;

    delete m_writeThread;
    m_backend.close(m_handle);
    m_handle = NULL;

    m_writeThread = NULL;

    QIODevice::close();
}

bool RawHID::isSequential() const
{
    return true;
}

qint64 RawHID::bytesAvailable() const
{
    if (!m_readThread)
        return -1;

    return m_readThread->getBytesAvailable() + QIODevice::bytesAvailable();
}

qint64 RawHID::bytesToWrite() const
{
    if (!m_writeThread)
        return -1;

    return m_writeThread->getBytesToWrite() + QIODevice::bytesToWrite();
}

qint64 RawHID::readData(char *data, qint64 maxSize)
{
    if (!m_readThread)
        return -1;

    return m_readThread->getReadData(data, maxSize);
}

qint64 RawHID::writeData(const char *data, qint64 maxSize)
{
    return m_writeThread ? m_writeThread->pushDataToWrite(data, maxSize) : -1;
}

/**
 * @}
 * @}
 */
