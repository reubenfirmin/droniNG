/**
 ******************************************************************************
 *
 * @file       serialplugin.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup SerialPlugin Serial Connection Plugin
 * @{
 * @brief Impliments serial connection to the flight hardware for Telemetry
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

#include "serialplugin.h"
#include "serialdevice.h"

#include <extensionsystem/pluginmanager.h>
#include <coreplugin/icore.h>

#include <QtCore/QtPlugin>
#include <QMainWindow>
#include <coreplugin/icore.h>
#include <QDebug>
#include <QFontMetrics>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#ifdef Q_OS_LINUX
#include <cerrno>
#include <pwd.h>
#include <unistd.h>
#include <vector>

namespace {
bool belongsToGroup(gid_t group)
{
    if (::getegid() == group)
        return true;
    const int count = ::getgroups(0, nullptr);
    if (count < 0)
        return true; // Do not recommend changing groups when membership is unknown.
    if (count == 0)
        return false;
    std::vector<gid_t> groups(count);
    const int received = ::getgroups(count, groups.data());
    if (received < 0)
        return true;
    return std::find(groups.begin(), groups.begin() + received, group)
        != groups.begin() + received;
}
}
#endif

SerialConnection::SerialConnection()
    : enablePolling(true)
    , m_deviceOpened(false)
{
    serialHandle = NULL;
    m_config = new SerialPluginConfiguration("Serial Telemetry", NULL, this);
    m_config->restoresettings();

    m_optionspage = new SerialPluginOptionsPage(m_config, this);

    connect(&periodicTimer, &QTimer::timeout, this, &SerialConnection::periodic);
    periodicTimer.start(1000);
}

SerialConnection::~SerialConnection()
{
}

void SerialConnection::onEnumerationChanged()
{
}

bool sortPorts(const QSerialPortInfo &s1, const QSerialPortInfo &s2)
{
    return s1.portName() < s2.portName();
}

void SerialConnection::periodic()
{
    availableDevices();
}

QList<SerialConnection::Port> SerialConnection::ports() const
{
    QList<Port> result;
    for (const auto &port : QSerialPortInfo::availablePorts())
        result.append({port.portName(), port.systemLocation(), port.description(), port.serialNumber(),
                       IDevice::usbTopologyForDeviceNode(port.systemLocation()),
                       port.vendorIdentifier(), port.productIdentifier()});
    std::sort(result.begin(), result.end(), [](const Port &a, const Port &b) { return a.name < b.name; });
    return result;
}

QList<IDevice *> SerialConnection::availableDevices()
{
    if (enablePolling) {
        bool changed = false;
        const auto available = ports();
        QList<Core::IDevice *> current;
        for (const auto &port : available) {
            const QStringList identity{port.location, port.description, port.serialNumber, port.usbPath,
                                       QString::number(port.vendor), QString::number(port.product)};
            IDevice *device = nullptr;
            for (auto *existing : m_devices) {
                if (existing->getName() == port.name && existing->property("serialIdentity").toStringList() == identity) {
                    device = existing;
                    break;
                }
            }
            if (!device) {
                device = new SerialDevice;
                device->setParent(this);
                device->setName(port.name);
                device->setDisplayName(port.description.isEmpty() ? port.name : port.description + " - " + port.name);
                device->setUsbTopologyPath(port.usbPath);
                device->setControllerCandidate(IDevice::plausibleControllerUsb(port.vendor, port.product, port.description));
                device->setProperty("serialIdentity", identity);
                changed = true;
            }
            current.append(device);
        }
        for (auto *device : m_devices)
            if (!current.contains(device)) { device->deleteLater(); changed = true; }
        m_devices = current;
        if (changed) emit availableDevChanged(this);
    }
    return m_devices;
}

Core::ConnectionError SerialConnection::groupAccessFailure(const QString &group,
                                                          const QString &username,
                                                          const QString &details)
{
    const auto shellQuote = [](QString value) {
        return "'" + value.replace("'", "'\\''") + "'";
    };
    return {
        tr("Your account needs permission to access this controller. "
           "Run this command in a terminal. "
           "Then log out and back in, reopen DroniNG, and click Connect."),
        QStringLiteral("sudo usermod -aG %1 %2").arg(shellQuote(group), shellQuote(username)),
        details
    };
}

Core::ConnectionError SerialConnection::openFailure(const QString &systemLocation, int baudRate,
                                                    QSerialPort::SerialPortError error,
                                                    const QString &errorText)
{
    Core::ConnectionError failure{
        tr("Check that your controller is plugged in. "
           "Close any other apps using it, then try connecting again."),
        QString(),
        tr("Could not open %1 at %2 baud (8N1, no flow control): %3 (Qt serial error %4)")
            .arg(systemLocation).arg(baudRate).arg(errorText).arg(int(error))
    };
#ifdef Q_OS_LINUX
    if (error != QSerialPort::PermissionError)
        return failure;

    // Qt also reports PermissionError when another process holds the port.
    // Check actual access (including ACLs) before recommending a group change.
    const int accessResult = ::access(QFile::encodeName(systemLocation).constData(), R_OK | W_OK);
    const int accessError = errno;
    if (accessResult != 0 && accessError == EACCES) {
        failure.message = tr("Your account doesn't have permission to access this controller. "
                             "Check the Linux device permissions for %1, then try connecting again.")
                              .arg(systemLocation);
        failure.details.append(tr("\nRead/write access denied for %1.").arg(systemLocation));
        const QFileInfo port(systemLocation);
        const QString group = port.group();
        const passwd *account = ::getpwuid(::geteuid());
        const QString username = account ? QString::fromLocal8Bit(account->pw_name) : QString();
        const QFile::Permissions groupAccess = QFile::ReadGroup | QFile::WriteGroup;
        if (!group.isEmpty() && !username.isEmpty() && port.groupId() != 0
            && port.ownerId() != ::geteuid()
            && (port.permissions() & groupAccess) == groupAccess
            && !belongsToGroup(port.groupId())) {
            failure.details.append(tr("\nThe device group is '%1'; this session is not a member.")
                                       .arg(group));
            return groupAccessFailure(group, username, failure.details);
        }
    } else if (accessResult == 0) {
        failure.message = tr("Another app may be using this controller. "
                             "Close other ground stations, configurators, or serial monitors. "
                             "Then click Connect again.");
        failure.details.append(tr("\nRead/write access is available; the port may already be in use."));
    } else {
        failure.details.append(tr("\nThe port's access permissions could not be checked."));
    }
#endif
    return failure;
}

QIODevice *SerialConnection::openDevice(IDevice *deviceName)
{
    m_error = {};
    if (!deviceName) {
        m_error.message = tr("This controller is no longer available. "
                             "Plug it in, select it from the connection list, and try again.");
        m_error.details = tr("The selected serial device is no longer available.");
        qWarning().noquote() << "Serial connection failed:" << m_error.details;
        return nullptr;
    }
    if (serialHandle) {
        closeDevice(deviceName->getName());
    }
    for (const auto &port : ports()) {
        if (port.name == deviceName->getName()) {
            const int baudRate = m_config->speed().toInt();
            const QString description = QString("%1 at %2 baud (8N1, no flow control)")
                                            .arg(port.location).arg(baudRate);
            qInfo().noquote() << "Opening serial port:" << description;
            serialHandle = new QSerialPort(port.location, this);
            const bool opened = serialHandle->open(QIODevice::ReadWrite);
            const bool configured = opened
                && serialHandle->setBaudRate(baudRate)
                && serialHandle->setDataBits(QSerialPort::Data8)
                && serialHandle->setParity(QSerialPort::NoParity)
                && serialHandle->setStopBits(QSerialPort::OneStop)
                && serialHandle->setFlowControl(QSerialPort::NoFlowControl);
            if (!configured) {
                m_error = opened
                    ? Core::ConnectionError{
                          tr("DroniNG couldn't set up this controller's serial connection. "
                             "Check the serial settings and try again."),
                          QString(),
                          tr("Could not configure %1: %2 (Qt serial error %3)")
                              .arg(description, serialHandle->errorString()).arg(int(serialHandle->error()))}
                    : openFailure(port.location, baudRate, serialHandle->error(),
                                  serialHandle->errorString());
                qWarning().noquote() << "Serial connection failed:" << m_error.details;
                serialHandle->close();
                delete serialHandle;
                serialHandle = nullptr;
                m_deviceOpened = false;
                return nullptr;
            }

            m_deviceOpened = true;
            connect(serialHandle, &QSerialPort::errorOccurred, this,
                    [this, description, handle = serialHandle](QSerialPort::SerialPortError error) {
                if (error != QSerialPort::NoError)
                    qWarning().noquote() << QString("Serial port error on %1: %2 (Qt serial error %3)")
                                               .arg(description, handle->errorString()).arg(int(error));
                if (handle != serialHandle || !m_deviceOpened) return;
                if (error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError)
                    emit transportDisconnected(this);
            }, Qt::QueuedConnection);
            return serialHandle;
        }
    }
    m_error.message = tr("This controller was disconnected. "
                         "Plug it back in, select it from the connection list, and try again.");
    m_error.details = tr("Serial port %1 disappeared before it could be opened.")
                          .arg(deviceName->getName());
    qWarning().noquote() << "Serial connection failed:" << m_error.details;
    return nullptr;
}

void SerialConnection::closeDevice(const QString &deviceName)
{
    Q_UNUSED(deviceName);
    // we have to delete the serial connection we created
    if (serialHandle) {
        disconnect(serialHandle, nullptr, this, nullptr);
        serialHandle->close();
        serialHandle->deleteLater();
        serialHandle = NULL;
        m_deviceOpened = false;
    }
}

QString SerialConnection::connectionName()
{
    return QString("Serial port");
}

QString SerialConnection::shortName()
{
    return QString("Serial");
}

/**
 Tells the Serial plugin to stop polling for serial devices
 */
void SerialConnection::suspendPolling()
{
    enablePolling = false;
}

/**
 Tells the Serial plugin to resume polling for serial devices
 */
void SerialConnection::resumePolling()
{
    enablePolling = true;
    periodic();
}

SerialPlugin::SerialPlugin()
{
}

SerialPlugin::~SerialPlugin()
{
    removeObject(m_connection->Optionspage());
}

void SerialPlugin::extensionsInitialized()
{
    addAutoReleasedObject(m_connection);
}

bool SerialPlugin::initialize(const QStringList &arguments, QString *errorString)
{
    Q_UNUSED(arguments);
    Q_UNUSED(errorString);
    m_connection = new SerialConnection();
    // must manage this registration of child object ourselves
    // if we use an autorelease here it causes the GCS to crash
    // as it is deleting objects as the app closes...
    addObject(m_connection->Optionspage());
    return true;
}
