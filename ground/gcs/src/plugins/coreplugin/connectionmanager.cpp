/**
 ******************************************************************************
 *
 * @file       connectionmanager.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 *             Parts by Nokia Corporation (qt-info@nokia.com) Copyright (C) 2009.
 * @author     dRonin, http://dronin.org Copyright (C) 2015
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup CorePlugin Core Plugin
 * @{
 * @brief The Core GCS plugin
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

#include "connectionmanager.h"
#include "modemanager.h"
#include "imode.h"
#include <coreplugin/icore.h>
#include <aggregation/aggregate.h>
#include <coreplugin/iconnection.h>
#include <coreplugin/idevice.h>
#include <extensionsystem/pluginmanager.h>
#include <QtSerialPort/QSerialPort>
#include <QtSerialPort/QSerialPortInfo>
#include <QDebug>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFrame>
#include <QLabel>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QComboBox>
#include <QEventLoop>
#include <QMainWindow>
#include <alarmsmonitorwidget.h>

namespace Core {

ConnectionManager::ConnectionManager(Internal::MainWindow *mainWindow, QTabWidget *modeStack)
    : QWidget(mainWindow)
    , m_availableDevList(nullptr)
    , m_connectBtn(nullptr)
    , m_ioDev(NULL)
    , polling(true)
    , m_mainWindow(mainWindow)
{
    QHBoxLayout *layout = new QHBoxLayout;
    layout->setSpacing(5);
    layout->setContentsMargins(0, 0, 9, 0);

    m_monitorWidget = new TelemetryMonitorWidget(this);
    AlarmsMonitorWidget::getInstance().init(m_monitorWidget->getRenderer(),
                                            m_monitorWidget->getBackgroundItem());
    layout->addWidget(m_monitorWidget, Qt::AlignHCenter | Qt::AlignVCenter);

    m_availableDevList = new QComboBox(this);
    m_availableDevList->setMinimumWidth(100);
    m_availableDevList->setMaximumWidth(150);
    m_availableDevList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_availableDevList->hide();

    m_connectBtn = new QPushButton(tr("Connect"), this);
    m_connectBtn->setEnabled(false);
    m_connectBtn->hide();

    setLayout(layout);

    // Keep telemetry and alarms in the flight workspaces. Firmware owns the
    // connection controls; the internal selector also supports auto-reconnect.
    modeStack->setCornerWidget(this, Qt::TopRightCorner);
    hide();

    QObject::connect(m_connectBtn, SIGNAL(clicked()), this, SLOT(onConnectClicked()));

    // setup our reconnect timers
    reconnect = new QTimer(this);
    reconnectCheck = new QTimer(this);
    connect(reconnect, SIGNAL(timeout()), this, SLOT(reconnectSlot()));
    connect(reconnectCheck, SIGNAL(timeout()), this, SLOT(reconnectCheckSlot()));
}

ConnectionManager::~ConnectionManager()
{
    disconnectDevice();
    suspendPolling();
    if (m_monitorWidget)
        delete m_monitorWidget;
}

void ConnectionManager::init()
{
    if (auto *modes = ModeManager::instance()) {
        connect(modes, &ModeManager::currentModeChanged, this, [this](IMode *mode) {
            setVisible(mode && mode->name() != "Welcome" && mode->name() != "Firmware");
        });
    }
    // register to the plugin manager so we can receive
    // new connection object from plugins
    QObject::connect(ExtensionSystem::PluginManager::instance(), SIGNAL(objectAdded(QObject *)),
                     this, SLOT(objectAdded(QObject *)));
    QObject::connect(ExtensionSystem::PluginManager::instance(),
                     SIGNAL(aboutToRemoveObject(QObject *)), this,
                     SLOT(aboutToRemoveObject(QObject *)));
}

/**
 * @brief Notifies the user when a connection attempt fails
 * @param device The device to which connection was attemped
 */
void ConnectionManager::connectDeviceFailed(DevListItem &device)
{
    Q_UNUSED(device);
    m_firmwareError = m_connectionError;
    m_firmwareStatus = tr("Connection failed");
    m_firmwareDetecting = false;
    emit firmwareStateChanged();
    showFirmwarePage();
}

void ConnectionManager::telemetryConnectionFailed(const ConnectionError &error)
{
    if (!m_ioDev || m_firmwareInstalling)
        return;
    m_flightDataAvailable = false;
    setControllerSetupRequired(false);
    m_connectBtn->setToolTip(tr("The port is open, but flight data is unavailable."));
    ConnectionError diagnosis = error;
    if (auto *serial = qobject_cast<QSerialPort *>(m_ioDev)) {
        const QSerialPortInfo info(*serial);
        diagnosis.details += tr("\nSerial port: %1 at %2 baud; USB manufacturer: %3; product: %4")
            .arg(info.systemLocation()).arg(serial->baudRate())
            .arg(info.manufacturer(), info.description());
    }
    m_firmwareError = diagnosis;
    m_firmwareDetecting = false;
    m_firmwareStatus = m_mspFirmwareDetected && m_firmwareVariant != "DRON"
        ? tr("Install DroniNG to load flight data.") : tr("Flight data unavailable");
    emit firmwareStateChanged();
}

void ConnectionManager::setFirmwareIdentity(const QString &variant, const QString &boardTarget,
                                             bool mspDetected, const QString &version)
{
    m_firmwareVariant = variant;
    m_firmwareBoardTarget = boardTarget;
    m_mspFirmwareDetected = mspDetected;
    m_firmwareVersion = version;
    QString family = variant;
    if (variant == "BTFL") family = "Betaflight";
    else if (variant == "CLFL") family = "Cleanflight";
    else if (variant == "DRON") family = "dRonin";
    else if (mspDetected && variant.isEmpty()) family = tr("MSP firmware");
    m_detectedFirmware = family + (version.isEmpty() ? QString() : " " + version);
    m_detectedBoard = boardTarget;
    m_detectedBootloader.clear();
    emit firmwareStateChanged();
}

void ConnectionManager::setNativeFirmwareIdentity(const QString &board, const QString &firmware,
                                                  const QString &bootloader)
{
    if (!board.isEmpty()) m_detectedBoard = board;
    if (!firmware.isEmpty()) m_detectedFirmware = firmware;
    if (!bootloader.isEmpty()) m_detectedBootloader = bootloader;
    emit firmwareStateChanged();
}

void ConnectionManager::setFirmwareStatus(const QString &message)
{
    if (m_firmwareStatus == message && m_firmwareError.message.isEmpty()
        && m_firmwareError.details.isEmpty() && m_firmwareError.command.isEmpty()) return;
    m_firmwareStatus = message;
    m_firmwareError = {};
    emit firmwareStateChanged();
}

void ConnectionManager::setFirmwareDetectionActive(bool active)
{
    if (m_firmwareDetecting == active) return;
    m_firmwareDetecting = active;
    emit firmwareStateChanged();
}

void ConnectionManager::showFirmwarePage()
{
    if (auto *modes = ModeManager::instance()) modes->activateModeByWorkspaceName("Firmware");
    emit firmwarePageRequested();
}

void ConnectionManager::beginFirmwareInstallation()
{
    m_firmwareInstalling = true;
    m_firmwareDetecting = false;
    emit firmwareStateChanged();
    disconnectDevice();
    suspendPolling();
}

void ConnectionManager::endFirmwareInstallation()
{
    m_firmwareInstalling = false;
    emit firmwareStateChanged();
    resumePolling();
}

void ConnectionManager::reportFirmwareDescription(const QByteArray &description)
{
    emit firmwareDescriptionReceived(description);
}

void ConnectionManager::telemetryHandshakeCompleted()
{
    m_flightDataAvailable = false;
    setControllerSetupRequired(false);
    setFirmwareDetectionActive(true);
    setFirmwareIdentity({}, {});
    setFirmwareStatus(tr("Reading controller data…"));
    m_connectBtn->setToolTip(tr("Reading controller data."));
}

void ConnectionManager::reportFlightDataReceived()
{
    m_flightDataAvailable = true;
    setFirmwareDetectionActive(false);
    setFirmwareStatus(tr("Connected. The controller is sending flight data."));
    emit flightDataReady();
}

void ConnectionManager::setControllerSetupRequired(bool required)
{
    if (m_controllerSetupRequired == required) return;
    m_controllerSetupRequired = required;
    emit firmwareStateChanged();
}

void ConnectionManager::requestControllerSetup()
{
    if (isConnected() && m_flightDataAvailable && !m_firmwareInstalling)
        emit setupWizardRequested();
}

/**
 *   Method called when the user clicks the "Connect" button
 */
bool ConnectionManager::connectDevice(DevListItem device)
{
    m_connectionError = {};
    const QString name = device.getConName();
    const auto fail = [&](const ConnectionError &error) {
        m_connectionError = error;
        m_firmwareError = error;
        m_firmwareStatus = tr("Connection failed");
        m_firmwareDetecting = false;
        emit firmwareStateChanged();
        showFirmwarePage();
        qWarning().noquote() << QString("Connection failed for %1: %2")
            .arg(name.isEmpty() ? "unknown device" : name,
                 error.details.isEmpty() ? error.message : error.details);
        return false;
    };
    if (!device.connection || device.device.isNull())
        return fail({tr("This controller is no longer available. "
                        "Plug it in, select it from the connection list, and try again."), {}, {}});
    if (m_ioDev)
        return fail({tr("Another controller is already connected. Disconnect it first."), {}, {}});

    m_flightDataAvailable = false;
    setFirmwareDetectionActive(true);
    setFirmwareIdentity({}, {});
    setFirmwareStatus(tr("Connecting to %1…").arg(device.device->getDisplayName()));
    showFirmwarePage();
    qInfo().noquote() << "Connecting to" << name;

    QIODevice *io_dev = device.connection->openDevice(device.device);
    if (!io_dev) {
        ConnectionError error = device.connection->error();
        if (error.message.isEmpty())
            error.message = tr("Check that your controller is plugged in, then try connecting again.");
#ifdef Q_OS_LINUX
        if (device.connection->shortName() == "USB")
            error.message.append(tr(" Check that the USB access rules are installed."));
#endif
        return fail(error);
    }

    // Serial plugins configure and open their device before returning it.
    if ((!io_dev->isOpen() && !io_dev->open(QIODevice::ReadWrite))
        || !io_dev->isReadable() || !io_dev->isWritable()) {
        const QString detail = io_dev->errorString();
        device.connection->closeDevice(name);
        return fail({tr("DroniNG couldn't access this controller. "
                        "Close any other apps using it, then try connecting again."), {},
                     tr("The device could not be opened for reading and writing: %1").arg(detail)});
    }

    // The device is connected make it the one selected on the dropbox
    for (int x = 0; x < m_availableDevList->count(); ++x) {
        if (device.getConName() == m_availableDevList->itemData(x, Qt::ToolTipRole).toString())
            m_availableDevList->setCurrentIndex(x);
    }
    // we appear to have connected to the device OK
    // remember the connection/device details
    m_connectionDevice = device;
    m_ioDev = io_dev;
    setFirmwareStatus(tr("Detecting board and firmware…"));
    qInfo().noquote() << "Transport opened for" << name << "- waiting for the UAVTalk handshake";

    const quint64 generation = ++m_connectionGeneration;
    m_connectionDestroyed = connect(device.connection, &QObject::destroyed, this,
                                   [this, generation] {
        // An old queued notification must not disconnect a subsequent device.
        if (generation != m_connectionGeneration)
            return;
        m_connectionDevice.connection = nullptr;
        disconnectDevice();
    }, Qt::QueuedConnection);

    // signal interested plugins that we connected to the device
    emit deviceConnected(io_dev);
    m_connectBtn->setText("Disconnect");
    m_connectBtn->setToolTip(tr("The port is open. Waiting for the controller's telemetry handshake."));
    m_availableDevList->setEnabled(false);

    // Opening the transport does not mean that telemetry is connected.
    m_monitorWidget->disconnect();

    return true;
}

/**
 *   Method called by plugins who want to force a disconnection.
 *   Used by Uploader gadget for instance.
 */
bool ConnectionManager::disconnectDevice()
{
    m_flightDataAvailable = false;
    setControllerSetupRequired(false);
    setFirmwareDetectionActive(false);
    if (!m_firmwareInstalling) {
        setFirmwareIdentity({}, {});
        setFirmwareStatus(tr("Connect a controller to detect its board and firmware."));
    }
    // tell the monitor widget we're disconnected
    m_monitorWidget->disconnect();

    if (!m_ioDev) {
        // apparently we are already disconnected: this can
        // happen if a plugin tries to force a disconnect whereas
        // we are not connected. Just return.
        return false;
    }

    // We are connected - disconnect from the device
    ++m_connectionGeneration;
    QObject::disconnect(m_connectionDestroyed);

    // stop our timers
    if (reconnect->isActive())
        reconnect->stop();
    if (reconnectCheck->isActive())
        reconnectCheck->stop();

    // signal interested plugins that user is disconnecting his device
    emit deviceAboutToDisconnect();

    try {
        if (m_connectionDevice.connection) {
            m_connectionDevice.connection->closeDevice(m_connectionDevice.getConName());
        }
    } catch (...) { // handle exception
        qDebug() << "Exception: m_connectionDevice.connection->closeDevice("
                 << m_connectionDevice.getConName() << ")";
    }

    m_connectionDevice.connection = NULL;
    m_ioDev = NULL;

    emit deviceDisconnected();
    m_connectBtn->setText("Connect");
    m_connectBtn->setToolTip(QString());
    m_availableDevList->setEnabled(true);

    return true;
}

/**
 *   Slot called when a plugin added an object to the core pool
 */
void ConnectionManager::objectAdded(QObject *obj)
{
    // Check if a plugin added a connection object to the pool
    IConnection *connection = Aggregation::query<IConnection>(obj);
    if (!connection)
        return;

    // register devices and populate CB
    devChanged(connection);

    // Keep track of the registration to be able to tell plugins
    // to do things
    m_connectionsList.append(connection);

    QObject::connect(connection, SIGNAL(availableDevChanged(IConnection *)), this,
                     SLOT(devChanged(IConnection *)));
}

void ConnectionManager::aboutToRemoveObject(QObject *obj)
{
    // Check if a plugin added a connection object to the pool
    IConnection *connection = Aggregation::query<IConnection>(obj);
    if (!connection)
        return;
    m_enumeratedConnections.remove(connection);

    if (m_connectionDevice.connection
        && m_connectionDevice.connection
            == connection) { // we are currently using the one that is about to be removed
        disconnectDevice();
        m_connectionDevice.connection = NULL;
        m_ioDev = NULL;
    }

    if (m_connectionsList.contains(connection))
        m_connectionsList.removeAt(m_connectionsList.indexOf(connection));
}

/**
 *   Slot called when the user clicks the connect/disconnect button
 */
void ConnectionManager::onConnectClicked()
{
    // Check if we have a ioDev already created:
    if (!m_ioDev) {
        // connecting to currently selected device
        DevListItem device = findDevice(
            m_availableDevList->itemData(m_availableDevList->currentIndex(), Qt::ToolTipRole)
                .toString());
        if (!connectDevice(device))
            connectDeviceFailed(device);
    } else { // disconnecting
        disconnectDevice();
    }
}

/**
 *   Slot called when the telemetry is connected
 */
void ConnectionManager::telemetryConnected()
{
    // Object retrieval completes after the handshake. Preserve live data and
    // board identity already received during retrieval.
    m_connectBtn->setToolTip(m_flightDataAvailable ? tr("Flight data is connected.")
                                                 : tr("Waiting for live flight data."));
    qInfo().noquote() << "Telemetry connected to" << m_connectionDevice.getConName();

    if (reconnectCheck->isActive())
        reconnectCheck->stop();

    // tell the monitor we're connected
    m_monitorWidget->connected();
}

/**
 *   Slot called when the telemetry is disconnected
 */
void ConnectionManager::telemetryDisconnected()
{
    qDebug() << "TelemetryMonitor: disconnected";

    const bool wasReady = m_flightDataAvailable;
    m_flightDataAvailable = false;
    setControllerSetupRequired(false);
    if (wasReady && m_ioDev && !m_firmwareInstalling) {
        setFirmwareDetectionActive(true);
        setFirmwareStatus(tr("Flight data interrupted. Checking the controller connection…"));
    }
    if (m_ioDev && m_connectionDevice.connection) {
        if (m_connectionDevice.connection->reconnect()) // currently used with bluetooth only
        {
            if (!reconnect->isActive())
                reconnect->start(1000);
        }
    }
    // tell the monitor we're disconnected
    m_monitorWidget->disconnect();
}

/**
 *   Slot called when the telemetry rates are updated
 */
void ConnectionManager::telemetryUpdated(double txRate, double rxRate)
{
    m_monitorWidget->updateTelemetry(txRate, rxRate);
}

void ConnectionManager::reconnectSlot()
{
    qDebug() << "reconnect";
    if (m_ioDev->isOpen())
        m_ioDev->close();

    if (m_ioDev->open(QIODevice::ReadWrite)) {
        qDebug() << "reconnect successfull";
        reconnect->stop();
        reconnectCheck->start(20000);
    } else
        qWarning().noquote() << "Transport reconnect failed for"
                            << m_connectionDevice.getConName() << ":" << m_ioDev->errorString();
}

void ConnectionManager::reconnectCheckSlot()
{
    reconnectCheck->stop();
    reconnect->start(1000);
}

/**
 *   Find a device by its displayed (visible on screen) name
 */
DevListItem ConnectionManager::findDevice(const QString &devName)
{
    foreach (DevListItem d, m_devList) {
        if (d.getConName() == devName)
            return d;
    }

    qDebug() << "findDevice: cannot find " << devName << " in device list";

    DevListItem d;
    d.connection = NULL;
    return d;
}

/**
 *   Tells every connection plugin to stop polling for devices if they
 *   are doing that.
 *
 */
void ConnectionManager::suspendPolling()
{
    foreach (IConnection *cnx, m_connectionsList) {
        cnx->suspendPolling();
    }

    m_connectBtn->setEnabled(false);
    m_availableDevList->setEnabled(false);
    polling = false;
}

/**
 *   Tells every connection plugin to resume polling for devices if they
 *   are doing that.
 */
void ConnectionManager::resumePolling()
{
    foreach (IConnection *cnx, m_connectionsList) {
        cnx->resumePolling();
    }

    m_connectBtn->setEnabled(true);
    m_availableDevList->setEnabled(true);
    polling = true;
}

/**
 *   Returns true if autoconnection is enabled
 */
bool ConnectionManager::getAutoconnect()
{
    return m_mainWindow->generalSettings()->autoConnect();
}

/**
 * Synchronize the list of connections displayed with those physically
 * present
 * @param[in] connection Connection type that you want to forget about :)
 */
void ConnectionManager::updateConnectionList(IConnection *connection)
{
    // Get the updated list of devices
    QList<IDevice *> availableDev = connection->availableDevices();

    // Go through the list of connections of that type.  If they are not in the
    // available device list then remove them.  If they are connected, then
    // disconnect them.
    for (QList<DevListItem>::iterator iter = m_devList.begin(); iter != m_devList.end();) {
        if (iter->connection != connection) {
            ++iter;
            continue;
        }

        // See if device exists in the updated availability list
        bool found = availableDev.contains(iter->device);
        if (!found) {
            // we are currently using the one we are about to erase
            if (m_connectionDevice.connection && m_connectionDevice.connection == connection
                && m_connectionDevice.device == iter->device) {
                disconnectDevice();
            }

            // We have to delete the IDevice in that DevListItem before getting rid
            // of the iter itself
            if (!iter->device.isNull())
                iter->device->deleteLater();

            iter = m_devList.erase(iter);
        } else
            ++iter;
    }

    // Add any back to list that don't exist
    foreach (IDevice *dev, availableDev) {
        bool found = m_devList.contains(DevListItem(connection, dev));
        if (!found) {
            registerDevice(connection, dev);
        }
    }
    m_enumeratedConnections.insert(connection);
}

/**
 *   Register a device from a specific connection plugin
 *   @param devN contains the connection shortname + device name which is diplayed in the tooltip
 *  @param disp is the name that is displayed in the dropdown menu
 *  @param name is the actual device name
 */
void ConnectionManager::registerDevice(IConnection *conn, IDevice *device)
{
    DevListItem d(conn, device);
    m_devList.append(d);
    // The initial inventory must leave the startup workspace alone. Only a
    // newly attached controller or an explicit connection selects Firmware.
    if (m_enumeratedConnections.contains(conn) && !m_ioDev && !m_firmwareInstalling
        && device->isControllerCandidate())
        showFirmwarePage();
}

/**
 *   A device plugin notified us that its device list has changed
 *   (eg: user plug/unplug a usb device)
 *   \param[in] connection Connection type which signaled the update
 */
void ConnectionManager::devChanged(IConnection *connection)
{
    if (!ExtensionSystem::PluginManager::instance()->allPluginsLoaded()) {
        connectionBackup.append(connection);
        connect(ExtensionSystem::PluginManager::instance(), SIGNAL(pluginsLoadEnded()), this,
                SLOT(connectionsCallBack()), Qt::UniqueConnection);
        return;
    }
    // clear device list combobox
    m_availableDevList->clear();

    // remove registered devices of this IConnection from the list
    updateConnectionList(connection);

    updateConnectionDropdown();

    emit availableDevicesChanged(m_devList);

    // disable connection button if the liNameif (m_availableDevList->count() > 0)
    if (m_availableDevList->count() > 0)
        m_connectBtn->setEnabled(true);
    else
        m_connectBtn->setEnabled(false);
}

void ConnectionManager::updateConnectionDropdown()
{
    // add all the list again to the combobox
    foreach (DevListItem d, m_devList) {
        m_availableDevList->addItem(d.getConName());
        m_availableDevList->setItemData(m_availableDevList->count() - 1, d.getConName(),
                                        Qt::ToolTipRole);
        if (!m_ioDev && d.getConName().startsWith("USB")) {
            if (m_mainWindow->generalSettings()->autoConnect()
                || m_mainWindow->generalSettings()->autoSelect())
                m_availableDevList->setCurrentIndex(m_availableDevList->count() - 1);

            if (m_mainWindow->generalSettings()->autoConnect() && polling && !m_firmwareInstalling) {
                qDebug() << "Automatically opening device";
                connectDevice(d);
                qDebug() << "ConnectionManager::devChanged autoconnected USB device";
            }
        }
    }
    if (m_ioDev) // if a device is connected make it the one selected on the dropbox
    {
        for (int x = 0; x < m_availableDevList->count(); ++x) {
            if (m_connectionDevice.getConName()
                == m_availableDevList->itemData(x, Qt::ToolTipRole).toString())
                m_availableDevList->setCurrentIndex(x);
        }
    }
}

void Core::ConnectionManager::connectionsCallBack()
{
    foreach (IConnection *con, connectionBackup) {
        devChanged(con);
    }
    connectionBackup.clear();
    disconnect(ExtensionSystem::PluginManager::instance(), SIGNAL(pluginsLoadEnded()), this,
               SLOT(connectionsCallBack()));
}

} // namespace Core
