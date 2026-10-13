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
#include "firmwarecatalog.h"
#include "firmwareinstallimage.h"
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
#include <QDateTime>
#include <QSettings>
#include <QRegularExpression>
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

    // MainWindow places telemetry and alarms in the application footer.
    // Firmware owns connection controls; the internal selector supports reconnect.
    Q_UNUSED(modeStack);

    QObject::connect(m_connectBtn, SIGNAL(clicked()), this, SLOT(onConnectClicked()));

    // setup our reconnect timers
    reconnect = new QTimer(this);
    reconnectCheck = new QTimer(this);
    connect(reconnect, SIGNAL(timeout()), this, SLOT(reconnectSlot()));
    connect(reconnectCheck, SIGNAL(timeout()), this, SLOT(reconnectCheckSlot()));
}

ConnectionManager::~ConnectionManager()
{
    // MainWindow closes the connection before deleting Core. Avoid notifying
    // surviving QML bindings again while its child widgets are being destroyed.
    if (m_ioDev) disconnectDevice();
    suspendPolling();
    if (m_monitorWidget)
        delete m_monitorWidget;
}

void ConnectionManager::init()
{
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
    appendConnectionLog(diagnosis.message + (diagnosis.details.isEmpty() ? QString() : '\n' + diagnosis.details));
    m_firmwareDetecting = false;
    m_firmwareStatus = m_mspFirmwareDetected && m_firmwareVariant != "DRON"
        ? tr("Install DroniNG to load flight data.") : tr("Flight data unavailable");
    if (!m_mspFirmwareDetected || m_firmwareVariant == "DRON") reportControllerProblem(diagnosis);
    emit firmwareStateChanged();
}

void ConnectionManager::rememberController(DevListItem device)
{
    if (!device.device) return;
    const QString path = device.device->usbTopologyPath();
    const QString name = device.device->getDisplayName();
    if (path != m_controllerRecovery.usbPath
        || name.section(" - ", 0, 0) != m_controllerRecovery.deviceName.section(" - ", 0, 0))
        m_controllerRecovery = {};
    m_controllerRecovery.deviceName = name;
    m_controllerRecovery.usbPath = path;
    const auto &catalog = FirmwareCatalog::instance();
    const auto pending = verifiedInstallation();
    const auto product = catalog.board(name.section(" - ", 0, 0));
    const auto installed = catalog.board(pending.boardId);
    if (product.isValid() && installed.isValid() && product.target != installed.target)
        m_controllerRecovery.boardId = product.id;
    else if (!pending.boardId.isEmpty() && !path.isEmpty() && pending.usbPath == path)
        m_controllerRecovery.boardId = pending.boardId;
    else if (product.isValid()) m_controllerRecovery.boardId = product.id;
}

void ConnectionManager::reportControllerProblem(const ConnectionError &error)
{
    const auto pending = verifiedInstallation();
    if (m_controllerRecovery.deviceName.isEmpty() && !pending.boardId.isEmpty()) {
        m_controllerRecovery.boardId = pending.boardId;
        m_controllerRecovery.usbPath = pending.usbPath;
        m_controllerRecovery.deviceName = FirmwareCatalog::instance().board(pending.boardId).name;
    }
    m_controllerRecovery.problem = error;
    m_firmwareError = error;
    m_firmwareDetecting = false;
    m_firmwareStatus = tr("Controller needs attention");
    if (!m_controllerRecovery.usbPath.isEmpty()) {
        m_startupFailurePath = m_controllerRecovery.usbPath;
        m_blockedUsbPaths.insert(m_controllerRecovery.usbPath);
    }
    emit firmwareStateChanged();
}

void ConnectionManager::clearControllerRecovery()
{
    m_controllerRecovery = {};
    m_startupFailurePath.clear();
    m_firmwareError = {};
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
    if (variant.isEmpty() && boardTarget.isEmpty()) {
        m_controllerSerial.clear();
        m_hardwareBoardId.clear();
    }
    const auto candidates = FirmwareCatalog::instance().candidatesForMspTarget(boardTarget);
    if (mspDetected && candidates.size() == 1
        && (boardTarget != "RADI" || m_controllerRecovery.boardId == candidates.first().id))
        m_controllerRecovery.boardId = candidates.first().id;
    if (!m_detectedFirmware.isEmpty())
        appendConnectionLog(tr("Detected firmware: %1%2").arg(m_detectedFirmware,
            boardTarget.isEmpty() ? QString() : tr("; board target: %1").arg(boardTarget)));
    emit firmwareStateChanged();
}

void ConnectionManager::setNativeFirmwareIdentity(const QString &board, const QString &firmware,
                                                  const QString &bootloader)
{
    QStringList detected;
    if (!board.isEmpty() && board != m_detectedBoard) detected << tr("Firmware target: %1").arg(board);
    if (!firmware.isEmpty() && firmware != m_detectedFirmware) detected << tr("Firmware: %1").arg(firmware);
    if (!bootloader.isEmpty() && bootloader != m_detectedBootloader) detected << tr("Bootloader: %1").arg(bootloader);
    if (!board.isEmpty()) m_detectedBoard = board;
    const auto profile = FirmwareCatalog::instance().board(board);
    if (!board.isEmpty()) m_firmwareBoardTarget = profile.isValid() ? profile.target : board;
    if (profile.isValid()) m_controllerRecovery.boardId = m_hardwareBoardId.isEmpty() ? profile.id : m_hardwareBoardId;
    if (!firmware.isEmpty()) m_detectedFirmware = firmware;
    if (!bootloader.isEmpty()) m_detectedBootloader = bootloader;
    if (!detected.isEmpty()) appendConnectionLog(detected.join("; "));
    emit firmwareStateChanged();
}

void ConnectionManager::setControllerSerial(const QString &serial)
{
    // STM32's 96-bit unique ID is shared by the USB and UAVTalk serials.
    // USB adds +BL / +FW; neither the firmware target nor the USB port identifies hardware.
    QString key = serial.trimmed().toLower();
    key.remove(QRegularExpression("\\+(bl|fw)$"));
    if (!QRegularExpression("^[0-9a-f]{24}$").match(key).hasMatch()
        || key == QString(24, '0') || key == QString(24, 'f')) key.clear();
    if (key == m_controllerSerial) return;
    m_controllerSerial = key;
    m_hardwareBoardId.clear();
    if (!key.isEmpty()) {
        const auto board = FirmwareCatalog::instance().board(
            ICore::instance()->settings()->value("ControllerHardware/" + key + "/board").toString());
        if (board.isValid()) m_hardwareBoardId = board.id;
    }
    emit firmwareStateChanged();
}

bool ConnectionManager::confirmHardwareBoard(const QString &boardId)
{
    const auto board = FirmwareCatalog::instance().board(boardId);
    if (!boardId.isEmpty() && !board.isValid()) return false;
    m_hardwareBoardId = board.id;
    if (!m_controllerSerial.isEmpty()) {
        auto *settings = ICore::instance()->settings();
        const QString key = "ControllerHardware/" + m_controllerSerial + "/board";
        if (board.isValid()) settings->setValue(key, board.id);
        else settings->remove(key);
        settings->sync();
    }
    if (board.isValid()) {
        m_controllerRecovery.boardId = board.id;
        appendConnectionLog(tr("Selected hardware model: %1").arg(board.name));
    } else m_controllerRecovery.boardId = FirmwareCatalog::instance().board(m_firmwareBoardTarget).id;
    emit firmwareStateChanged();
    return true;
}

void ConnectionManager::setFirmwareStatus(const QString &message)
{
    if (m_firmwareStatus == message && m_firmwareError.message.isEmpty()
        && m_firmwareError.details.isEmpty() && m_firmwareError.command.isEmpty()) return;
    m_firmwareStatus = message;
    m_firmwareError = {};
    if (isConnected() || m_firmwareInstalling || !m_detectedBootloader.isEmpty()) appendConnectionLog(message);
    emit firmwareStateChanged();
}

void ConnectionManager::setFirmwareDetectionActive(bool active)
{
    if (active && !m_controllerRecovery.problem.message.isEmpty()) return;
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
    if (!isConnected() && m_controllerRecovery.problem.message.isEmpty() && m_startupFailurePath.isEmpty()) {
        setFirmwareIdentity({}, {});
        setFirmwareStatus(tr("Connect a controller to detect its board and firmware."));
    }
    emit firmwareStateChanged();
    resumePolling();
}

void ConnectionManager::reportFirmwareDescription(const QByteArray &description)
{
    m_receivedDescriptor = description;
    confirmVerifiedInstallation();
    emit firmwareDescriptionReceived(description);
    emit firmwareStateChanged();
}

VerifiedInstallation ConnectionManager::verifiedInstallation() const
{
    auto *settings = ICore::instance()->settings();
    VerifiedInstallation result{settings->value("FirmwareInstallation/board").toString(),
        settings->value("FirmwareInstallation/usbPath").toString(),
        settings->value("FirmwareInstallation/log").toString(),
        QByteArray::fromHex(settings->value("FirmwareInstallation/descriptor").toByteArray())};
    if (!FirmwareCatalog::instance().board(result.boardId).isValid() || result.descriptor.size() != 100)
        return {};
    return result;
}

void ConnectionManager::recordVerifiedInstallation(const QString &boardId, const QString &usbPath,
                                                  const QByteArray &descriptor, const QString &log)
{
    if (!FirmwareCatalog::instance().board(boardId).isValid() || descriptor.size() != 100) return;
    auto *settings = ICore::instance()->settings();
    settings->setValue("FirmwareInstallation/board", boardId);
    settings->setValue("FirmwareInstallation/usbPath", usbPath);
    settings->setValue("FirmwareInstallation/descriptor", descriptor.toHex());
    settings->setValue("FirmwareInstallation/log", log);
    settings->sync();
    emit firmwareStateChanged();
}

void ConnectionManager::clearVerifiedInstallation()
{
    auto *settings = ICore::instance()->settings();
    settings->remove("FirmwareInstallation");
    settings->sync();
    emit firmwareStateChanged();
}

void ConnectionManager::reportFirmwareProfile(int schema, const QString &profile, const QString &layout, const QByteArray &hash)
{
    const auto pending = verifiedInstallation();
    const auto board = FirmwareCatalog::instance().board(pending.boardId);
    m_receivedMatchingProfile = schema == 1 && profile == pending.boardId && layout == board.layoutId
        && hash == FirmwareInstallImage::importedProfileHash(pending.boardId);
    confirmVerifiedInstallation();
    emit firmwareProfileReceived(schema, profile, layout, hash);
}

void ConnectionManager::confirmVerifiedInstallation()
{
    const auto pending = verifiedInstallation();
    if (pending.boardId.isEmpty() || !isConnected() || !m_flightDataAvailable
        || !m_connectionDevice.device || m_connectionDevice.device->usbTopologyPath() != pending.usbPath
        || m_receivedDescriptor != pending.descriptor) return;
    if (!FirmwareCatalog::instance().board(pending.boardId).layoutId.isEmpty() && !m_receivedMatchingProfile) return;
    clearVerifiedInstallation();
}

void ConnectionManager::telemetryHandshakeCompleted()
{
    if (!m_ioDev || !m_controllerRecovery.problem.message.isEmpty()) return;
    appendConnectionLog(tr("UAVTalk handshake completed."));
    m_flightDataAvailable = false;
    setControllerSetupRequired(false);
    setFirmwareDetectionActive(true);
    setFirmwareIdentity({}, {});
    setFirmwareStatus(tr("Reading controller data…"));
    m_connectBtn->setToolTip(tr("Reading controller data."));
}

void ConnectionManager::reportFlightDataReceived()
{
    m_controllerRecovery.problem = {};
    if (m_connectionDevice.device)
        resetAutomaticConnectionFailures(m_connectionDevice.device->usbTopologyPath());
    m_flightDataAvailable = true;
    setFirmwareDetectionActive(false);
    setFirmwareStatus(tr("Connected. The controller is sending flight data."));
    confirmVerifiedInstallation();
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
    if (device.device && !m_ioDev)
        resetAutomaticConnectionFailures(device.device->usbTopologyPath());
    return connectDeviceInternal(device, false);
}

bool ConnectionManager::automaticConnectionBlocked(const QString &usbPath) const
{
    return m_blockedUsbPaths.contains(usbPath);
}

void ConnectionManager::resetAutomaticConnectionFailures(const QString &usbPath)
{
    m_blockedUsbPaths.remove(usbPath);
    m_installationOpenFailures.remove(usbPath);
    if (m_startupFailurePath == usbPath) m_startupFailurePath.clear();
}

bool ConnectionManager::autoConnectDevice(DevListItem device)
{
    if (!device.device || automaticConnectionBlocked(device.device->usbTopologyPath())) return false;
    if (!m_controllerRecovery.problem.message.isEmpty()
        && (device.device->usbTopologyPath().isEmpty() || m_controllerRecovery.usbPath.isEmpty())
        && device.device->getDisplayName().section(" - ", 0, 0)
            == m_controllerRecovery.deviceName.section(" - ", 0, 0)) return false;
    return connectDeviceInternal(device, true);
}

bool ConnectionManager::connectDeviceInternal(DevListItem device, bool automatic)
{
    if (!m_ioDev) {
        rememberController(device);
        m_controllerRecovery.problem = {};
    }
    m_startupFailurePath.clear();
    m_connectionError = {};
    const QString name = device.getConName();
    if (!m_ioDev) {
        if (!automatic) m_connectionLog.clear();
        appendConnectionLog(tr("Connecting to %1.").arg(name.isEmpty() ? tr("controller") : name));
    }
    const auto fail = [&](const ConnectionError &error, bool openingTransport = false) {
        m_connectionError = error;
        m_firmwareError = error;
        const QString path = device.device ? device.device->usbTopologyPath() : QString();
        const auto pending = verifiedInstallation();
        // DFU exit can enumerate HID before its node and udev permissions are
        // ready. Retry only a bounded number of failed opens for this verified
        // install; an opened transport resetting before telemetry still stops
        // immediately in transportLost().
        const bool settling = openingTransport && automatic && m_firmwareInstalling
            && !path.isEmpty() && path == pending.usbPath
            && !pending.boardId.isEmpty()
            && ++m_installationOpenFailures[path] <= 6;
        if (settling) {
            m_firmwareStatus = tr("Waiting for the controller's USB connection…");
            appendConnectionLog(tr("USB is reconnecting after installation. Retrying the connection…")
                + (error.details.isEmpty() ? QString() : '\n' + error.details));
            qInfo().noquote() << "Post-install USB open retry" << m_installationOpenFailures[path]
                             << "of 6:" << error.details;
            emit firmwareStateChanged();
            return false;
        }
        m_firmwareStatus = tr("Connection failed");
        m_firmwareDetecting = false;
        if (!m_ioDev) reportControllerProblem(error);
        appendConnectionLog(error.message + (error.details.isEmpty() ? QString() : '\n' + error.details));
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
        return fail(error, true);
    }

    // Serial plugins configure and open their device before returning it.
    if ((!io_dev->isOpen() && !io_dev->open(QIODevice::ReadWrite))
        || !io_dev->isReadable() || !io_dev->isWritable()) {
        const QString detail = io_dev->errorString();
        device.connection->closeDevice(name);
        return fail({tr("The controller connection could not be opened. "
                        "Reconnect the controller to the same port, then retry the connection."), {},
                     tr("The device could not be opened for reading and writing: %1").arg(detail)}, true);
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
    appendConnectionLog(tr("Transport opened. Waiting for the UAVTalk handshake."));
    setFirmwareStatus(tr("Detecting board and firmware…"));
    qInfo().noquote() << "Transport opened for" << name << "- waiting for the UAVTalk handshake";

    const quint64 generation = ++m_connectionGeneration;
    m_connectionDestroyed = connect(device.connection, &QObject::destroyed, this,
                                   [this, generation] {
        // An old queued notification must not disconnect a subsequent device.
        if (generation != m_connectionGeneration)
            return;
        m_connectionDevice.connection = nullptr;
        transportLost();
    }, Qt::QueuedConnection);
    m_transportDisconnected = connect(device.connection, &IConnection::transportDisconnected, this,
                                      [this, generation](IConnection *) {
        if (generation == m_connectionGeneration) transportLost();
    });
    m_deviceClosed = connect(io_dev, &QIODevice::aboutToClose, this, [this, generation] {
        if (generation == m_connectionGeneration) transportLost();
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

void ConnectionManager::transportLost()
{
    if (!m_ioDev) return;
    const QString path = m_connectionDevice.device ? m_connectionDevice.device->usbTopologyPath() : m_controllerRecovery.usbPath;
    const bool startupInterrupted = !m_flightDataAvailable;
    const auto transportError = m_connectionDevice.connection ? m_connectionDevice.connection->transportError() : ConnectionError{};
    const bool failedTransport = !transportError.message.isEmpty();
    if (failedTransport) {
        reportControllerProblem(transportError);
        appendConnectionLog(transportError.message + '\n' + transportError.details);
        qWarning().noquote() << transportError.details;
    } else if (startupInterrupted) {
        const QString detail = tr("%1 disconnected before live data was ready.%2")
            .arg(m_controllerRecovery.deviceName, path.isEmpty() ? QString() : tr(" USB path: %1.").arg(path));
        reportControllerProblem({tr("The controller disconnected before it could send live data. "
            "Automatic retries have stopped. Disconnect USB and the flight battery, then reconnect USB to the same port. "
            "Retry the connection, or reinstall the firmware below."), {}, detail});
        appendConnectionLog(m_firmwareError.message + '\n' + detail);
        qWarning().noquote() << detail;
    }
    disconnectDevice();
    if (startupInterrupted || failedTransport) showFirmwarePage();
}

/**
 *   Method called by plugins who want to force a disconnection.
 *   Used by Uploader gadget for instance.
 */
bool ConnectionManager::disconnectDevice()
{
    if (m_ioDev) appendConnectionLog(tr("Disconnected from %1.").arg(m_connectionDevice.getConName()));
    m_flightDataAvailable = false;
    m_receivedDescriptor.clear(); m_receivedMatchingProfile = false;
    setControllerSetupRequired(false);
    setFirmwareDetectionActive(false);
    setFirmwareIdentity({}, {});
    if (!m_firmwareInstalling && m_controllerRecovery.problem.message.isEmpty() && m_startupFailurePath.isEmpty()) {
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
    QObject::disconnect(m_transportDisconnected);
    QObject::disconnect(m_deviceClosed);

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
    m_availableDevList->setEnabled(polling);

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
    m_devList.removeIf([connection](const DevListItem &item) { return item.connection == connection; });
    updateConnectionDropdown();
    emit availableDevicesChanged(m_devList);
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
    appendConnectionLog(tr("Controller data retrieved."));
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

void ConnectionManager::appendConnectionLog(const QString &message)
{
    if (message.trimmed().isEmpty()) return;
    const QString entry = QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + message;
    if (!m_connectionLog.isEmpty() && m_connectionLog.last().mid(10) == message) return;
    m_connectionLog.append(entry);
    while (m_connectionLog.size() > 200) m_connectionLog.removeFirst();
    emit connectionLogChanged();
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
    m_availableDevList->setEnabled(!isConnected());
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
                transportLost();
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
                if (autoConnectDevice(d)) qDebug() << "Automatically connected USB device";
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
