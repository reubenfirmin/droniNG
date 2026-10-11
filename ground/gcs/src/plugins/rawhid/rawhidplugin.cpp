/**
 ******************************************************************************
 *
 * @file       rawhidplugin.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
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

#include "rawhidplugin.h"
#include "rawhid.h"

#include <coreplugin/icore.h>
#include <QtCore/QtPlugin>
#include <QtCore/QMutexLocker>
#include <QDebug>
#include <QSet>

#include "rawhid_const.h"
#include "usbsignalfilter.h"

// **********************************************************************

RawHIDConnection::RawHIDConnection()
{
    RawHidHandle = NULL;
    enablePolling = true;

    m_usbMonitor = USBMonitor::instance();

    m_signalFilter = new USBSignalFilter(-1, -1, -1, USBMonitor::Running);
    connect(m_signalFilter, &USBSignalFilter::deviceDiscovered, this,
            &RawHIDConnection::onDeviceConnected);
    connect(m_signalFilter, &USBSignalFilter::deviceRemoved, this,
            &RawHIDConnection::onDeviceDisconnected);
}

RawHIDConnection::~RawHIDConnection()
{
    if (RawHidHandle)
        if (RawHidHandle->isOpen())
            RawHidHandle->close();

    delete m_signalFilter;
}

/**
  The USB monitor tells us a new device appeared
  */
void RawHIDConnection::onDeviceConnected()
{
    emit availableDevChanged(this);
}

/**
  The USB monitor tells us a device disappeard
  */
void RawHIDConnection::onDeviceDisconnected()
{
    RAW_HID_QXTLOG_DEBUG("onDeviceDisconnected()");
    if (enablePolling)
        emit availableDevChanged(this);
}

/**
 * Returns the list of all currently available devices
 *
 */
QList<Core::IDevice *> RawHIDConnection::availableDevices()
{
    QList<USBPortInfo> portsList;

    // Loop for all vendorIDs known by the board manager
    Core::BoardManager *brdMgr = Core::ICore::instance()->boardManager();
    QList<int> brdVID = brdMgr->getKnownVendorIDs();
    foreach (int vendorID, brdVID) {
        RAW_HID_QXTLOG_DEBUG("[rawhidplugin] VendorID type known: ", vendorID);
        portsList.append(m_usbMonitor->availableDevices(vendorID, -1, -1, USBMonitor::Running));
    }
    return devicesForPorts(portsList);
}

QList<Core::IDevice *> RawHIDConnection::devicesForPorts(const QList<USBPortInfo> &ports)
{
    QList<Core::IDevice *> devices;
    QSet<QString> present;
    for (const auto &prt : ports) {
        present.insert(prt.path);
        auto *dev = m_devices.value(prt.path).data();
        // Repeated inventory queries must preserve device identity. Otherwise
        // the manager treats an unchanged USB controller as unplugged/replugged.
        if (!dev || dev->getName() != prt.serialNumber || dev->getVendorID() != prt.vendorID
            || dev->getProductID() != prt.productID) {
            dev = new USBDevice();
            dev->setParent(this);
            dev->setName(prt.serialNumber);
            dev->setControllerCandidate(true);
            dev->setVendorID(prt.vendorID);
            dev->setProductID(prt.productID);
            dev->setPath(prt.path);
            dev->setUsbTopologyPath(Core::IDevice::usbTopologyForDeviceNode(prt.path));
            m_devices.insert(prt.path, dev);
        }
        dev->setDisplayName(prt.product);
        devices.append(dev);
    }
    for (auto it = m_devices.begin(); it != m_devices.end();)
        if (!present.contains(it.key())) it = m_devices.erase(it);
        else ++it;
    return devices;
}

QIODevice *RawHIDConnection::openDevice(Core::IDevice *deviceName)
{
    if (RawHidHandle)
        closeDevice(deviceName->getName());

    // We know this device is (should be?) a USB device:
    USBDevice *usbDev = dynamic_cast<USBDevice *>(deviceName);
    Q_ASSERT(usbDev);

    RawHidHandle = new RawHID(usbDev);
    return RawHidHandle;
}

void RawHIDConnection::closeDevice(const QString &deviceName)
{
    Q_UNUSED(deviceName);
    if (RawHidHandle) {
        RAW_HID_QXTLOG_DEBUG("Closing the device here");
        RawHidHandle->close();
        delete RawHidHandle;
        RawHidHandle = NULL;
    }
}

QString RawHIDConnection::connectionName()
{
    return QString("Raw HID USB");
}

QString RawHIDConnection::shortName()
{
    return QString("USB");
}

/**
 Tells the Raw HID plugin to stop polling for USB devices
 */
void RawHIDConnection::suspendPolling()
{
    enablePolling = false;
}

/**
 Tells the Raw HID plugin to resume polling for USB devices
 */
void RawHIDConnection::resumePolling()
{
    enablePolling = true;
}

// **********************************************************************

RawHIDPlugin::RawHIDPlugin()
{
    m_usbMonitor = NULL;
    hidConnection = NULL;
}

RawHIDPlugin::~RawHIDPlugin()
{
    if (m_usbMonitor)
        delete m_usbMonitor;
}

void RawHIDPlugin::extensionsInitialized()
{
    hidConnection = new RawHIDConnection();
    addAutoReleasedObject(hidConnection);
}

bool RawHIDPlugin::initialize(const QStringList &arguments, QString *errorString)
{
    Q_UNUSED(arguments);
    Q_UNUSED(errorString);

    // We have to create the USB Monitor here:
    m_usbMonitor = new USBMonitor(this);

    return true;
}
