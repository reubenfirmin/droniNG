/**
 ******************************************************************************
 *
 * @file       uploadergadgetwidget.cpp
 * @author     dRonin, http://dronin.org Copyright (C) 2015-2016
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2014
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup  Uploader Uploader Plugin
 * @{
 * @brief The Tau Labs uploader plugin main widget
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
 *
 * Additional note on redistribution: The copyright and license notices above
 * must be maintained in each individual source file that is a derivative work
 * of this source file; otherwise redistribution is prohibited.
 */

#include <QFileDialog>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QResizeEvent>
#include <QToolButton>
#include <coreplugin/firmwarepage.h>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QUrl>

#include "uploadergadgetwidget.h"
#include "firmwareiapobj.h"
#include <coreplugin/coreconstants.h>
#include <coreplugin/icore.h>
#include <coreplugin/firmwarecatalog.h>
#include <coreplugin/firmwareinstallimage.h>
#include <coreplugin/modemanager.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include "rawhid/rawhidplugin.h"
#include "ui_uploader.h"
#include <extensionsystem/pluginmanager.h>

using namespace uploader;

/**
 * @brief Class constructor, sets signal to slot connections,
 * creates actions, creates utility classes instances, etc
 */
UploaderGadgetWidget::UploaderGadgetWidget(QWidget *parent)
    : QWidget(parent)
    , telemetryConnected(false)
    , iapUpdated(false)
{
    m_widget = new Ui_UploaderWidget();
    m_widget->setupUi(this);
    setUploaderStatus(uploader::DISCONNECTED);
    m_widget->partitionBrowserTW->setSelectionMode(QAbstractItemView::SingleSelection);
    m_widget->partitionBrowserTW->setSelectionBehavior(QAbstractItemView::SelectRows);

    // these widgets will be set to retain their position in the layout even when hidden
    QList<QWidget *> hideable = { m_widget->progressBar };
    foreach (QWidget *widget, hideable) {
        QSizePolicy sp = widget->sizePolicy();
        sp.setRetainSizeWhenHidden(true);
        widget->setSizePolicy(sp);
    }

    // Setup partition manager actions
    QAction *action = new QAction("Save to file", this);
    connect(action, &QAction::triggered, this, &UploaderGadgetWidget::onPartitionSave);
    m_widget->partitionBrowserTW->addAction(action);
    action = new QAction("Flash from file", this);
    connect(action, &QAction::triggered, this, &UploaderGadgetWidget::onPartitionFlash);
    m_widget->partitionBrowserTW->addAction(action);
    action = new QAction("Erase", this);
    connect(action, &QAction::triggered, this, &UploaderGadgetWidget::onPartitionErase);
    m_widget->partitionBrowserTW->addAction(action);
    m_widget->partitionBrowserTW->setContextMenuPolicy(Qt::ActionsContextMenu);

    // Clear widgets to defaults
    FirmwareOnDeviceClear(true);
    FirmwareLoadedClear(true);

    PartitionBrowserClear();
    DeviceInformationClear();

    pm = ExtensionSystem::PluginManager::instance();
    telMngr = pm->getObject<TelemetryManager>();
    utilMngr = pm->getObject<UAVObjectUtilManager>();
    UAVObjectManager *obm = pm->getObject<UAVObjectManager>();

    /* These (and firmwareIAP) are queued connections.  We may do things
     * that ultimately stop the telemetry manager, so we don't want to be
     * invoked from its components and return into it.
     */
    connect(telMngr, &TelemetryManager::connected, this, &UploaderGadgetWidget::onAutopilotConnect,
            Qt::QueuedConnection);
    connect(telMngr, &TelemetryManager::disconnected, this,
            &UploaderGadgetWidget::onAutopilotDisconnect, Qt::QueuedConnection);
    firmwareIap = FirmwareIAPObj::GetInstance(obm);

    connect(firmwareIap, &UAVObject::objectUpdated, this, &UploaderGadgetWidget::onIAPUpdated,
            Qt::QueuedConnection);

    // Connect button signals to slots
    connect(m_widget->openButton, &QAbstractButton::clicked, this,
            &UploaderGadgetWidget::onLoadFirmwareButtonClick);
    connect(m_widget->rescueButton, &QAbstractButton::clicked, this,
            &UploaderGadgetWidget::onRescueButtonClick);
    connect(m_widget->flashButton, &QAbstractButton::clicked, this,
            &UploaderGadgetWidget::onFlashButtonClick);
    connect(m_widget->bootButton, &QAbstractButton::clicked, this,
            &UploaderGadgetWidget::onBootButtonClick);
    connect(m_widget->safeBootButton, &QAbstractButton::clicked, this,
            &UploaderGadgetWidget::onBootButtonClick);
    connect(m_widget->exportConfigButton, &QAbstractButton::clicked, this,
            &UploaderGadgetWidget::onExportButtonClick);

    connect(m_widget->pbHelp, &QAbstractButton::clicked, this, &UploaderGadgetWidget::openHelp);
    Core::BoardManager *brdMgr = Core::ICore::instance()->boardManager();

    // Setup usb discovery signals for boards in bl state
    usbFilterBL = new USBSignalFilter(brdMgr->getKnownVendorIDs(), -1, -1, USBMonitor::Bootloader);
    usbFilterUP = new USBSignalFilter(brdMgr->getKnownVendorIDs(), -1, -1, USBMonitor::Upgrader);

    connect(usbFilterBL, &USBSignalFilter::deviceRemoved, this,
            &UploaderGadgetWidget::onBootloaderRemoved, Qt::QueuedConnection);

    connect(usbFilterBL, &USBSignalFilter::deviceDiscovered, this,
            &UploaderGadgetWidget::onBootloaderDetected, Qt::QueuedConnection);

    connect(usbFilterUP, &USBSignalFilter::deviceRemoved, this,
            &UploaderGadgetWidget::onBootloaderRemoved, Qt::QueuedConnection);

    connect(usbFilterUP, &USBSignalFilter::deviceDiscovered, this,
            &UploaderGadgetWidget::onBootloaderDetected, Qt::QueuedConnection);

    connect(&dfu, &tl_dfu::DFUObject::operationProgress, this,
            &UploaderGadgetWidget::onStatusUpdate);

    conMngr = Core::ICore::instance()->connectionManager();
    m_firmwarePage = new Core::FirmwarePage(conMngr, this);
    m_widget->verticalLayout->insertWidget(0, m_firmwarePage, 1);
    m_dialog.hide();
    m_firmwarePage->addOperationWidget(&m_dialog);
    m_upgrade = new FirmwareUpgradeController(obm, this);
    m_dialog.bind(m_upgrade);
    connect(m_upgrade, &FirmwareUpgradeController::readSettingsRequested, this, [this](int size) {
        m_upgradeBuffer.clear(); m_upgradeTransferSuccess = false;
        m_upgradeTransfer = BackupTransfer;
        setUploaderStatus(uploader::BL_BUSY);
        if (!dfu.DownloadPartitionThreaded(&m_upgradeBuffer, DFU_PARTITION_SETTINGS, size)) {
            m_upgradeTransfer = NoUpgradeTransfer; m_upgrade->settingsRead({}, false);
        }
    });
    connect(m_upgrade, &FirmwareUpgradeController::cancelReadRequested, this, [this] { dfu.requestInterruption(); });
    connect(m_upgrade, &FirmwareUpgradeController::writeRequested, this, [this](const QByteArray &image) {
        m_upgradeBuffer = image; m_upgradeMetadata = image.right(100);
        m_upgradeTransferSuccess = false; m_upgradeTransfer = FirmwareTransfer;
        setUploaderStatus(uploader::BL_BUSY);
        if (!dfu.UploadPartitionThreaded(m_upgradeBuffer, DFU_PARTITION_FW, m_upgrade->context().codeSize)) {
            m_upgradeTransfer = NoUpgradeTransfer; m_upgrade->writeFinished(false);
        }
    });
    connect(m_upgrade, &FirmwareUpgradeController::eraseSettingsRequested, this, [this] {
        m_upgradeBuffer.clear(); m_upgradeTransferSuccess = false; m_upgradeTransfer = ResetTransfer;
        setUploaderStatus(uploader::BL_BUSY);
        if (!dfu.WipePartitionThreaded(&m_upgradeBuffer, DFU_PARTITION_SETTINGS, m_upgrade->context().settingsSize)) {
            m_upgradeTransfer = NoUpgradeTransfer; m_upgrade->settingsErased(false);
        }
    });
    connect(m_upgrade, &FirmwareUpgradeController::writeSettingsRequested, this, [this](const QByteArray &partition) {
        m_upgradeBuffer = partition; m_upgradeTransferSuccess = false; m_upgradeTransfer = SettingsConversionTransfer;
        setUploaderStatus(uploader::BL_BUSY);
        if (!dfu.UploadPartitionThreaded(m_upgradeBuffer, DFU_PARTITION_SETTINGS, m_upgrade->context().settingsSize)) {
            m_upgradeTransfer = NoUpgradeTransfer; m_upgrade->convertedSettingsWritten(false);
        }
    });
    connect(m_upgrade, &FirmwareUpgradeController::readConvertedSettingsRequested, this, [this](int size) {
        m_upgradeBuffer.clear(); m_upgradeTransferSuccess = false; m_upgradeTransfer = SettingsConversionReadbackTransfer;
        setUploaderStatus(uploader::BL_BUSY);
        if (!dfu.DownloadPartitionThreaded(&m_upgradeBuffer, DFU_PARTITION_SETTINGS, size)) {
            m_upgradeTransfer = NoUpgradeTransfer; m_upgrade->convertedSettingsRead({}, false);
        }
    });
    connect(&dfu, &DFUObject::downloadFinished, this, [this](bool success) {
        if (m_upgradeTransfer != NoUpgradeTransfer) m_upgradeTransferSuccess = success;
    });
    connect(&dfu, &DFUObject::uploadFinished, this, [this](tl_dfu::Status status) {
        if (m_upgradeTransfer != NoUpgradeTransfer) m_upgradeTransferSuccess = status == Last_operation_Success;
    });
    // Completion signals emitted from run() precede QThread::finished. Start the next job only here.
    connect(&dfu, &QThread::finished, this, &UploaderGadgetWidget::finishUpgradeTransfer);
    connect(m_upgrade, &FirmwareUpgradeController::bootRequested, this, [this](bool loader) {
        setUploaderStatus(loader ? uploader::UPGRADING_CATCHLOADER : uploader::DISCONNECTED);
        const bool started = dfu.JumpToApp(false) > 0;
        dfu.CloseBootloaderComs();
        if (!started) { m_upgrade->fail(tr("The controller could not start after the write. Disconnect external power and USB, reconnect USB, then reopen Firmware maintenance.")); return; }
        if (!loader) conMngr->resumePolling();
    });
    connect(m_upgrade, &FirmwareUpgradeController::verifySettingsRequested, this, &UploaderGadgetWidget::verifyUpgradeSettings);
    m_upgradeReadbackTimer.setSingleShot(true);
    connect(&m_upgradeReadbackTimer, &QTimer::timeout, this, [this] {
        disconnect(m_upgradeReadbackConnection);
        m_upgrade->settingsVerified(false, tr("The controller did not return the requested setting."));
    });
    connect(m_upgrade, &FirmwareUpgradeController::connectionCheckRequested, this, [this] {
        conMngr->resumePolling();
        if (telemetryConnected) onAutopilotReady();
        else onBootloaderDetected();
    });
    connect(m_upgrade, &FirmwareUpgradeController::retryRequested, this, [this] {
        if (dfu.isRunning()) return;
        if (uploaderStatus != uploader::BL_SITTING) {
            setUploaderStatus(uploader::ENTERING_LOADER);
            onBootloaderDetected();
        }
        if (uploaderStatus != uploader::BL_SITTING) {
            m_upgrade->fail(tr("Reconnect USB with the controller in its native bootloader, then choose Retry preparation.")); return;
        }
        if (!FirmwareUpgradeController::sameControllerSerial(currentBoard.cpu_serial, m_upgrade->context().serial)) {
            m_upgrade->fail(tr("Connect the controller you were updating before retrying.")); return;
        }
        auto capabilities = dfu.findCapabilities();
        if (m_upgrade->canRetrySettingsConversion()) {
            upgraderActive = true;
            m_firmwarePage->setOperationActive(true);
            m_upgrade->retrySettingsConversion(capabilities.ID, currentBoard.cpu_serial,
                capabilities.SizeOfCode, capabilities.PartitionSizes.value(DFU_PARTITION_SETTINGS, 0),
                dfu.DownloadDescriptionAsByteArray(capabilities.SizeOfDesc));
            return;
        }
        doUpgradeOperation(!m_bootloaderFirmwareValid, capabilities);
    });
    connect(m_upgrade, &FirmwareUpgradeController::settled, this, [this] {
        upgraderActive = false;
        m_upgradeReadbackTimer.stop(); disconnect(m_upgradeReadbackConnection);
        if (m_upgrade->state() == FirmwareUpgradeController::Complete && m_upgrade->setupRequired())
            conMngr->setControllerSetupRequired(true);
        m_advancedTools->setEnabled(!conMngr->firmwareInstallationActive());
        setUploaderStatus(uploaderStatus == uploader::BL_BUSY ? uploader::BL_SITTING : uploaderStatus);
    });
    connect(&m_dialog, &UpgradeAssistantDialog::configureRequested, this, [this] {
        m_dialog.hide();
        if (conMngr->controllerSetupRequired()) conMngr->requestControllerSetup();
        else Core::ModeManager::instance()->activateModeByWorkspaceName("Configuration");
    });
    connect(m_firmwarePage, &Core::FirmwarePage::legacyFirmwareUpdateRequested,
            this, &UploaderGadgetWidget::onFirmwareUpdateRequested);
    connect(m_firmwarePage, &Core::FirmwarePage::legacyFirmwareBootRequested,
            this, &UploaderGadgetWidget::onFirmwareBootRequested);
    auto *advanced = new QWidget(this);
    m_advancedTools = advanced;
    auto *advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    while (m_widget->verticalLayout->count() > 1) {
        auto *item = m_widget->verticalLayout->takeAt(1);
        if (auto *widget = item->widget()) { advancedLayout->addWidget(widget); delete item; }
        else if (auto *childLayout = item->layout()) { childLayout->setParent(nullptr); advancedLayout->addLayout(childLayout); }
        else advancedLayout->addItem(item);
    }
    m_recoveryActions = new QGridLayout;
    while (auto *item = m_widget->bootloaderButtons->takeAt(0)) {
        if (auto *button = item->widget()) m_recoveryButtons.append(button);
        delete item;
    }
    advancedLayout->removeItem(m_widget->bootloaderButtons);
    delete m_widget->bootloaderButtons;
    advancedLayout->insertLayout(0, m_recoveryActions);
    advanced->installEventFilter(this);
    m_firmwarePage->setRecoveryTools(advanced);
    connect(conMngr, &Core::ConnectionManager::firmwareStateChanged, advanced, [this, advanced] {
        advanced->setEnabled(!conMngr->firmwareInstallationActive() && !m_promptActive && !upgraderActive);
    });

    /* Enter the loader if it's available */
    setUploaderStatus(uploader::ENTERING_LOADER);
    onBootloaderDetected();

    /* Else begin our normal actions waitin' for a board */
    if (getUploaderStatus() != uploader::BL_SITTING) {
        setUploaderStatus(uploader::DISCONNECTED);
        setStatusInfo(tr("GCS ready for flight board connection..."), uploader::STATUSICON_OK);
    }
}

bool UploaderGadgetWidget::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_advancedTools && event->type() == QEvent::Resize) {
        const int available = static_cast<QResizeEvent *>(event)->size().width();
        for (auto *button : m_recoveryButtons) m_recoveryActions->removeWidget(button);
        int row = 0, column = 0, used = 0;
        for (auto *button : m_recoveryButtons) {
            const int required = qMax(button->minimumWidth(), button->sizeHint().width());
            if (column > 0 && used + required + m_recoveryActions->horizontalSpacing() > available) {
                ++row; column = 0; used = 0;
            }
            m_recoveryActions->addWidget(button, row, column++, Qt::AlignLeft);
            used += required + m_recoveryActions->horizontalSpacing();
        }
    }
    return QWidget::eventFilter(object, event);
}

/**
 * @brief Class destructor, nothing needs to be destructed ATM
 */
UploaderGadgetWidget::~UploaderGadgetWidget()
{
    // TODO remove this when addObject removed from factory
    ExtensionSystem::PluginManager::instance()->removeObject(this);
}

/**
 * @brief Hides or unhides the firmware on device information set
 * when hidden it presents an information not available string
 * @param clear set to true to hide information and false to unhide
 */
void UploaderGadgetWidget::FirmwareOnDeviceClear(bool clear)
{
    const QRegularExpression rx(QRegularExpression::wildcardToRegularExpression("*OD_lbl"));
    foreach (QLabel *l, findChildren<QLabel *>(rx)) {
        if (clear)
            l->clear();
        l->setVisible(!clear);
    }
    m_widget->firmwareOnDevicePic->setVisible(!clear);
    m_widget->noInfoOnDevice->setVisible(clear);
    m_widget->onDeviceGB->setEnabled(!clear);
}

/**
 * @brief Hides or unhides the firmware loaded (from file) information set
 * when hidden it presents an information not available string
 * @param clear set to true to hide information and false to unhide
 */
void UploaderGadgetWidget::FirmwareLoadedClear(bool clear)
{
    const QRegularExpression rx(QRegularExpression::wildcardToRegularExpression("*LD_lbl"));
    foreach (QLabel *l, findChildren<QLabel *>(rx)) {
        if (clear)
            l->clear();
        l->setVisible(!clear);
    }
    m_widget->firmwareLoadedPic->setVisible(!clear);
    m_widget->noFirmwareLoaded->setVisible(clear);
    m_widget->loadedGB->setEnabled(!clear);
    if (clear)
        m_widget->userDefined_LD_lbl->clear();
    m_widget->userDefined_LD_lbl->setVisible(!clear);
}

/**
 * @brief Clears and disables the partition browser table
 */
void UploaderGadgetWidget::PartitionBrowserClear()
{
    m_widget->partitionBrowserTW->clearContents();
    m_widget->partitionBrowserGB->setEnabled(false);
}

void UploaderGadgetWidget::DeviceInformationClear()
{
    m_widget->deviceInformationMainLayout->setVisible(false);
    m_widget->deviceInformationNoInfo->setVisible(true);
    m_widget->boardName_lbl->setVisible(false);
    currentBoard.board = NULL;
}

/**
 * @brief Fills out the device information widgets
 * @param board structure describing the connected hardware
 */
void UploaderGadgetWidget::DeviceInformationUpdate(deviceInfo board)
{
    if (!board.board)
        return;
    conMngr->setControllerSerial(board.cpu_serial);
    currentBoard = board;
    m_widget->boardName_lbl->setText(board.board->boardDescription());
    m_widget->devName_lbl->setText(
        board.board->getBoardNameFromID(board.board->getBoardType() << 8));
    m_widget->hwRev_lbl->setText(board.hw_revision);
    const bool imported = !Core::FirmwareCatalog::instance().board(board.board->shortName()).layoutId.isEmpty();
    m_widget->blVer_lbl->setText(imported ? tr("STM32 ROM") : board.bl_version);
    bool bl_version_ok;
    int bl_version = board.bl_version.toInt(&bl_version_ok, 16);
    conMngr->setNativeFirmwareIdentity(board.board->shortName(), {},
        imported ? tr("STM32 system bootloader")
            : bl_version_ok ? tr("dRonin %1").arg(board.bl_version) : QString());
    m_widget->blVer_lbl->setStyleSheet(
        !imported && bl_version_ok && bl_version < board.board->minBootLoaderVersion() ? "color: red;" : "");
    m_widget->maxCode_lbl->setText(board.max_code_size);
    m_widget->deviceInformationMainLayout->setVisible(true);
    m_widget->deviceInformationNoInfo->setVisible(false);
    m_widget->boardPic->setPixmap(board.board->getBoardPicture());
    FirmwareLoadedUpdate(loadedFile);
}

/**
 * @brief Fills out the firmware on device information widgets
 * @param firmware structure describing the firmware
 * @param crc firmware crc
 */
void UploaderGadgetWidget::FirmwareOnDeviceUpdate(deviceDescriptorStruct firmware, QString crc)
{
    conMngr->setNativeFirmwareIdentity(Core::IBoardType::getBoardNameFromID(firmware.boardID()),
        Core::FirmwareCatalog::firmwareName(firmware.description));
    m_widget->builtForOD_lbl->setText(Core::IBoardType::getBoardNameFromID(firmware.boardID()));
    m_widget->crcOD_lbl->setText(crc);
    m_widget->gitHashOD_lbl->setText(firmware.gitHash);
    m_widget->ancestorHashOD_lbl->setText(firmware.nextAncestor);
    m_widget->firmwareDateOD_lbl->setText(firmware.gitDate);
    m_widget->firmwareTagOD_lbl->setText(firmware.gitTag);
    m_widget->uavosSHA_OD_lbl->setText(firmware.uavoHash.toHex().toUpper());
    m_widget->userDefined_OD_lbl->setText(firmware.userDefined);

    if (firmware.certified) {
        QPixmap pix = QPixmap(QString(":uploader/images/application-certificate.svg"));
        m_widget->firmwareOnDevicePic->setPixmap(pix);
        m_widget->firmwareOnDevicePic->setToolTip(tr("Tagged officially released firmware build"));
    } else {
        QPixmap pix = QPixmap(QString(":uploader/images/warning.svg"));
        m_widget->firmwareOnDevicePic->setPixmap(pix);
        m_widget->firmwareOnDevicePic->setToolTip(tr("Custom Firmware Build"));
    }
    FirmwareOnDeviceClear(false);
}

/**
 * @brief Fills out the loaded firmware (from file) information widgets
 * @param firmwareArray array containing the loaded firmware
 */
void UploaderGadgetWidget::FirmwareLoadedUpdate(QByteArray firmwareArray)
{
    if (firmwareArray.isEmpty())
        return;
    deviceDescriptorStruct firmware;
    if (!utilMngr->descriptionToStructure(firmwareArray.right(100), firmware)) {
        setStatusInfo(tr("Error parsing file metadata"), uploader::STATUSICON_FAIL);
        QPixmap pix = QPixmap(QString(":uploader/images/error.svg"));
        m_widget->firmwareLoadedPic->setPixmap(pix);
        m_widget->firmwareLoadedPic->setToolTip(
            tr("Error unrecognized file format, proceed at your own risk"));
        m_widget->gitHashLD_lbl->setText(
            tr("Error unrecognized file format, proceed at your own risk"));
        FirmwareLoadedClear(false);
        m_widget->userDefined_LD_lbl->setVisible(false);
        return;
    }
    setStatusInfo(tr("File loaded successfully"), uploader::STATUSICON_INFO);
    m_widget->builtForLD_lbl->setText(Core::IBoardType::getBoardNameFromID(firmware.boardID()));
    bool ok;
    currentBoard.max_code_size.toLong(&ok);
    if (!ok)
        m_widget->crcLD_lbl->setText("Not Available");
    else if (firmwareArray.length() > currentBoard.max_code_size.toLong()) {
        m_widget->crcLD_lbl->setText("Not Available");
    } else {
        quint32 crc = dfu.CRCFromQBArray(firmwareArray, currentBoard.max_code_size.toLong());
        m_widget->crcLD_lbl->setText(QString::number(crc));
    }
    m_widget->gitHashLD_lbl->setText(firmware.gitHash);
    m_widget->ancestorHashLD_lbl->setText(firmware.nextAncestor);
    m_widget->firmwareDateLD_lbl->setText(firmware.gitDate);
    m_widget->firmwareTagLD_lbl->setText(firmware.gitTag);
    m_widget->uavosSHA_LD_lbl->setText(firmware.uavoHash.toHex().toUpper());
    m_widget->userDefined_LD_lbl->setText(firmware.userDefined);
    if (currentBoard.board && (currentBoard.board->getBoardType() != firmware.boardType)) {
        QPixmap pix = QPixmap(QString(":uploader/images/error.svg"));
        m_widget->firmwareLoadedPic->setPixmap(pix);
        m_widget->firmwareLoadedPic->setToolTip(
            tr("Error firmware loaded firmware doesn't match connected board"));
    } else if (firmware.certified) {
        QPixmap pix = QPixmap(QString(":uploader/images/application-certificate.svg"));
        m_widget->firmwareLoadedPic->setPixmap(pix);
        m_widget->firmwareLoadedPic->setToolTip(tr("Tagged officially released firmware build"));
    } else {
        QPixmap pix = QPixmap(QString(":uploader/images/warning.svg"));
        m_widget->firmwareLoadedPic->setPixmap(pix);
        m_widget->firmwareLoadedPic->setToolTip(tr("Custom Firmware Build"));
    }
    FirmwareLoadedClear(false);
}

/**
 * @brief slot called by the telemetryManager when the autopilot connects
 */
void UploaderGadgetWidget::onAutopilotConnect()
{
    telemetryConnected = true;
    CheckAutopilotReady();
}

/**
 * @brief slot called by the telemetryManager when the autopilot disconnects
 */
void UploaderGadgetWidget::onAutopilotDisconnect()
{
    FirmwareOnDeviceClear(true);
    DeviceInformationClear();
    PartitionBrowserClear();
    telemetryConnected = false;
    iapUpdated = false;
    if (getUploaderStatus() == uploader::ENTERING_LOADER)
        return;
    if (getUploaderStatus() == uploader::UPGRADING_CATCHLOADER)
        return;
    setUploaderStatus(uploader::DISCONNECTED);
    setStatusInfo(tr("Telemetry disconnected"), uploader::STATUSICON_INFO);
}

/**
 * @brief Called when the autopilot is ready to be queried for information
 * regarding running firmware and hardware specs
 */
void UploaderGadgetWidget::onAutopilotReady()
{
    setUploaderStatus(uploader::CONNECTED_TO_TELEMETRY);
    setStatusInfo(tr("Telemetry connected"), uploader::STATUSICON_INFO);
    deviceInfo board;
    board.bl_version = tr("Not available");
    board.board = utilMngr->getBoardType();
    board.cpu_serial = utilMngr->getBoardCPUSerial().toHex();
    board.hw_revision = QString::number(utilMngr->getBoardRevision());
    board.max_code_size = tr("Not available");
    DeviceInformationUpdate(board);
    deviceDescriptorStruct device;
    if (utilMngr->getBoardDescriptionStruct(device)) {
        FirmwareOnDeviceUpdate(device, QString::number(utilMngr->getFirmwareCRC()));
        if (!conMngr->firmwareInstallationActive()) FirmwareCheckForUpdate(device);
    }
    emit newBoardSeen(board, device);
    if (m_upgrade) m_upgrade->firmwareConnected(device.description, board.cpu_serial);
}

void UploaderGadgetWidget::onFirmwareUpdateRequested()
{
    if (uploaderStatus == uploader::BL_SITTING && !upgraderActive && !m_promptActive) {
        if (!FirmwareCheckForUpdate(m_bootloaderDescription, !m_bootloaderFirmwareValid)) return;
        m_firmwarePage->setOperationActive(true);
        setStatusInfo(tr("Checking the bootloader before updating…"), uploader::STATUSICON_RUNNING);
        auto device = dfu.findCapabilities();
        if (device.ID != m_bootloaderDescription.boardID() || device.SizeOfCode <= 0) {
            setUploaderStatus(uploader::BL_SITTING);
            setStatusInfo(tr("The bootloader is not responding. Reconnect USB, then retry the update."), uploader::STATUSICON_FAIL);
            return;
        }
        doUpgradeOperation(!m_bootloaderFirmwareValid, device);
        return;
    }
    if (active() || !conMngr->isConnected() || conMngr->firmwareInstallationActive()) return;
    deviceDescriptorStruct device;
    if (!utilMngr->getBoardDescriptionStruct(device) || !FirmwareCheckForUpdate(device)) return;
    onRescueButtonClick();
    conMngr->showFirmwarePage();
    setUploaderStatus(uploader::UPGRADING);
}

void UploaderGadgetWidget::onFirmwareBootRequested()
{
    if (uploaderStatus == uploader::BL_SITTING && m_bootloaderFirmwareValid && !upgraderActive && !m_promptActive)
        m_widget->bootButton->click();
}

/**
 * @brief slot called by the iap object when it is updated
 */
void UploaderGadgetWidget::onIAPUpdated()
{
    if ((getUploaderStatus() == uploader::ENTERING_LOADER)
        || (getUploaderStatus() == uploader::UPGRADING))
        return;
    iapUpdated = true;
    CheckAutopilotReady();
}

/**
 * @brief slot called when the user presses the load firmware button
 * It loads the file and calls the information display functions
 */
void UploaderGadgetWidget::onLoadFirmwareButtonClick()
{
    QString board;
    if (currentBoard.board)
        board = currentBoard.board->shortName().toLower();
    QString filename = LoadFirmwareFileDialog(board);
    if (filename.isEmpty()) {
        setStatusInfo(tr("Open cancelled"), uploader::STATUSICON_FAIL);
        return;
    }
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatusInfo(tr("Could not open file"), uploader::STATUSICON_FAIL);
        return;
    }
    loadedFile = file.readAll();

    FirmwareLoadedClear(true);
    FirmwareLoadedUpdate(loadedFile);
    setUploaderStatus(getUploaderStatus());
}

// Would be nice to do the work here required to constify firmwareImage
bool UploaderGadgetWidget::flashFirmware(QByteArray &firmwareImage)
{
    QEventLoop loop;

    QTimer timeout;

    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeout.setSingleShot(true);
    timeout.start(120000); // Very long out of caution. Current Brain
    // FW takes 36-37 seconds on some machines to
    // flash.

    tl_dfu::Status operationSuccess = DFUidle;

    setStatusInfo("", uploader::STATUSICON_RUNNING);
    setUploaderStatus(uploader::BL_BUSY);
    onStatusUpdate(QString("Starting upload..."), 0); // set progress bar to 0 while erasing
    dfu.UploadPartitionThreaded(firmwareImage, DFU_PARTITION_FW,
                                currentBoard.max_code_size.toInt());

    /* disconnects when loop comes out of scope */
    connect(&dfu, &DFUObject::uploadFinished, &loop, [&](tl_dfu::Status status) {
        operationSuccess = status;
        loop.exit();
    });

    loop.exec(); /* Wait for timeout or download complete */

    timeout.stop();

    if (operationSuccess != Last_operation_Success) {
        setUploaderStatus(uploader::BL_SITTING);
        setStatusInfo(tr("Firmware upload failed"), uploader::STATUSICON_FAIL);

        return false;
    }

    setStatusInfo(tr("Firmware upload success"), uploader::STATUSICON_OK);

    if ((!firmwareImage.right(100).startsWith("TlFw"))
        && (!firmwareImage.right(100).startsWith("OpFw"))) {
        setUploaderStatus(uploader::BL_SITTING);

        return true;
    }

    tempArray.clear();
    tempArray.append(firmwareImage.right(100));
    tempArray.chop(12);
    QString user("            ");
    user = user.replace(0, m_widget->userDefined_LD_lbl->text().length(),
                        m_widget->userDefined_LD_lbl->text());
    tempArray.append(user.toLatin1());
    setStatusInfo(tr("Starting firmware metadata upload"), uploader::STATUSICON_INFO);
    dfu.UploadPartitionThreaded(tempArray, DFU_PARTITION_DESC, 100);

    operationSuccess = DFUidle;

    timeout.start(10000);
    loop.exec();
    timeout.stop();

    if (operationSuccess != Last_operation_Success) {
        setStatusInfo(tr("Firmware metadata upload failed"), uploader::STATUSICON_FAIL);

        setUploaderStatus(uploader::BL_SITTING);

        return false;
    }

    setUploaderStatus(uploader::BL_SITTING);
    setStatusInfo(tr("Firmware and firmware metadata upload success"), uploader::STATUSICON_OK);

    // uploaded succeeded so we can assume the loaded file is on the board
    deviceDescriptorStruct descStructure;
    if (UAVObjectUtilManager::descriptionToStructure(tempArray, descStructure)) {
        quint32 crc = dfu.CRCFromQBArray(firmwareImage, currentBoard.max_code_size.toLong());
        FirmwareOnDeviceUpdate(descStructure, QString::number(crc));
    }
    setUploaderStatus(uploader::BL_SITTING);

    return true;
}

/**
 * @brief slot called when the user presses the flash firmware button
 * It start a non blocking firmware upload
 */
void UploaderGadgetWidget::onFlashButtonClick()
{
    if (flashFirmware(loadedFile)) {
        this->activateWindow();
        m_widget->bootButton->setFocus();
    }
}

void UploaderGadgetWidget::haltOrReset(bool halting)
{
    if (!firmwareIap->getIsPresentOnHardware())
        return;

    if (halting) {
        setUploaderStatus(uploader::ENTERING_LOADER);
    } else {
        setUploaderStatus(uploader::DISCONNECTED);
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    firmwareIap->setBoardRevision(0);
    firmwareIap->setBoardType(0);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(firmwareIap, QOverload<UAVObject *, bool>::of(&FirmwareIAPObj::transactionCompleted),
            &loop, &QEventLoop::quit);
    int magicValue = 1122;
    int magicStep = 1111;
    for (int i = 0; i < 3; ++i) {
        // Firmware IAP module specifies that the timing between iap commands must be
        // between 500 and 5000ms
        timeout.start(600);
        loop.exec();
        firmwareIap->setCommand(magicValue);
        magicValue += magicStep;
        if ((!halting) && (magicValue == 3344))
            magicValue = 4455;

        setStatusInfo(QString(tr("Sending IAP Step %0").arg(i + 1)), uploader::STATUSICON_INFO);
        firmwareIap->updated();
        timeout.start(1000);
        loop.exec();
        if (!timeout.isActive()) {
            setStatusInfo(QString(tr("Sending IAP Step %0 failed").arg(i + 1)),
                          uploader::STATUSICON_FAIL);
            return;
        }
        timeout.stop();
    }

    Core::IConnection *conn = conMngr->getCurrentDevice().connection;
    if (conn && conn->shortName() == "USB") {
        conMngr->disconnectDevice();
        timeout.start(200);
        loop.exec();
        timeout.stop();
        conMngr->suspendPolling();
        startRescue();
    } else {
        setUploaderStatus(uploader::DISCONNECTED);
        conMngr->disconnectDevice();
    }
}

/**
 * @brief slot called when the user presses the rescue button
 * It enables the bootloader detection and starts a detection timer
 */
void UploaderGadgetWidget::onRescueButtonClick()
{
    if (telMngr->isConnected()) {
        // This means "halt" to enter the loader.
        haltOrReset(true);

        return;
    }

    if (conMngr->hasMspFirmware() && conMngr->firmwareVariant() != "DRON") {
        conMngr->showFirmwarePage();
        return;
    }

    // Otherwise, this means "begin rescue".
    conMngr->suspendPolling();
    setUploaderStatus(uploader::ENTERING_LOADER);
    setStatusInfo(tr("Please connect the board with USB with no external power applied"),
                  uploader::STATUSICON_INFO);
    startRescue();
}

bool UploaderGadgetWidget::downloadSettings()
{
    QEventLoop loop;

    QTimer timeout;
    timeout.setSingleShot(true);

    bool operationSuccess = false;
    bool timedOut = false;

    connect(&timeout, &QTimer::timeout, &loop, [&] { timedOut = true; dfu.requestInterruption(); });
    connect(&dfu, &QThread::finished, &loop, &QEventLoop::quit);

    /* disconnects when loop comes out of scope */
    connect(&dfu, &DFUObject::downloadFinished, &loop, [&](bool status) {
        qDebug() << "downloadFinished in dSettings: " << status;
        operationSuccess = status;
    });

    timeout.start(100000); /* 100 secs is a long time; unfortunately
                            * revo settings part is HUUUUGE and takes
                            * forever to download. */

    if (!triggerPartitionDownload(DFU_PARTITION_SETTINGS)) return false;
    loop.exec(); /* Wait for timeout or download complete */
    timeout.stop();

    return operationSuccess && !timedOut;
}

int UploaderGadgetWidget::promptFirmware(const QString &text, const QString &details,
                                        QDialogButtonBox::StandardButtons buttons, const QString &action)
{
    if (m_promptActive) return QDialogButtonBox::Cancel;
    m_promptActive = true;
    m_advancedTools->setEnabled(false);
    conMngr->showFirmwarePage();
    m_firmwarePage->setOperationActive(true);
    auto *prompt = new QWidget(this);
    prompt->setObjectName("firmwarePrompt");
    auto *layout = new QVBoxLayout(prompt);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(16);
    auto *heading = new QLabel(text, prompt); heading->setWordWrap(true); heading->setTextFormat(Qt::PlainText);
    heading->setProperty("role", "section");
    auto *body = new QLabel(details, prompt); body->setWordWrap(true); body->setTextFormat(Qt::PlainText);
    body->setProperty("role", "muted");
    layout->addWidget(heading); layout->addWidget(body);
    auto *actions = new QDialogButtonBox(buttons, prompt); layout->addWidget(actions);
    for (auto *button : actions->buttons()) {
        button->setIcon(QIcon());
        if (actions->standardButton(button) == QDialogButtonBox::Yes) {
            button->setText(action); button->setProperty("primary", true);
        } else button->setText(tr("Cancel"));
    }
    m_firmwarePage->addOperationWidget(prompt);
    prompt->show();
    QEventLoop loop;
    int result = QDialogButtonBox::Cancel;
    connect(actions, &QDialogButtonBox::clicked, &loop, [&](QAbstractButton *button) {
        result = actions->standardButton(button); loop.quit();
    });
    connect(conMngr, &Core::ConnectionManager::deviceDisconnected, &loop, &QEventLoop::quit);
    connect(&m_dialog, &UpgradeAssistantDialog::finished, &loop, &QEventLoop::quit);
    loop.exec();
    delete prompt;
    m_promptActive = false;
    m_advancedTools->setEnabled(!conMngr->firmwareInstallationActive());
    setUploaderStatus(uploaderStatus);
    return result;
}

void UploaderGadgetWidget::doUpgradeOperation(bool blankFC, tl_dfu::device &dev)
{
    if (dfu.isRunning() || upgraderActive || !currentBoard.board) return;
    upgraderActive = true;
    m_advancedTools->setEnabled(false);
    m_firmwarePage->setOperationActive(true);
    Core::ModeManager::instance()->activateModeByWorkspaceName("Firmware");
    m_dialog.show();
    if (uploaderStatus != uploader::BL_SITTING) {
        m_upgrade->fail(tr("The controller is not in its native bootloader. Reconnect USB and retry."));
        return;
    }
    FirmwareUpgradeContext context;
    context.name = currentBoard.board->shortName();
    context.target = Core::FirmwareCatalog::instance().board(context.name).target;
    context.serial = currentBoard.cpu_serial;
    context.boardId = dev.ID; context.codeSize = dev.SizeOfCode; context.descriptionSize = dev.SizeOfDesc;
    context.settingsSize = dev.PartitionSizes.value(DFU_PARTITION_SETTINGS, 0);
    context.needsBootloader = !context.settingsSize || currentBoard.board->minBootLoaderVersion() > dev.BL_Version;
    if (!blankFC) context.sourceDescriptor = m_bootloaderDescription.description;
    FirmwareLoadFromFile(getImagePath(context.name), &context.firmware);
    if (context.needsBootloader) FirmwareLoadFromFile(getImagePath(context.name, "bu"), &context.bootloader);
    m_upgrade->start(context);
}

void UploaderGadgetWidget::finishUpgradeTransfer()
{
    const auto job = m_upgradeTransfer;
    m_upgradeTransfer = NoUpgradeTransfer;
    if (job == NoUpgradeTransfer) return;
    const bool success = m_upgradeTransferSuccess && !m_upgrade->transferDisconnected();
    if (job == FirmwareTransfer && success) {
        m_upgradeBuffer = m_upgradeMetadata;
        m_upgradeTransferSuccess = false; m_upgradeTransfer = MetadataTransfer;
        if (!dfu.UploadPartitionThreaded(m_upgradeBuffer, DFU_PARTITION_DESC, 100)) {
            m_upgradeTransfer = NoUpgradeTransfer; m_upgrade->writeFinished(false);
        }
        return;
    }
    if (!success) {
        dfu.CloseBootloaderComs();
        setUploaderStatus(uploader::DISCONNECTED);
        conMngr->resumePolling();
    } else setUploaderStatus(uploader::BL_SITTING);
    if (job == BackupTransfer) m_upgrade->settingsRead(m_upgradeBuffer, success);
    else if (job == SettingsConversionTransfer) m_upgrade->convertedSettingsWritten(success);
    else if (job == SettingsConversionReadbackTransfer) m_upgrade->convertedSettingsRead(m_upgradeBuffer, success);
    else if (job == ResetTransfer) {
        bool erased = success && m_upgradeBuffer.size() == m_upgrade->context().settingsSize;
        for (char byte : m_upgradeBuffer) if (uchar(byte) != 0xff) erased = false;
        m_upgrade->settingsErased(erased);
    } else m_upgrade->writeFinished(success);
}

void UploaderGadgetWidget::verifyUpgradeSettings(const QList<FirmwareSettingRecord> &records)
{
    m_upgradeVerification = records;
    verifyNextUpgradeSetting();
}

void UploaderGadgetWidget::verifyNextUpgradeSetting()
{
    m_upgradeReadbackTimer.stop(); disconnect(m_upgradeReadbackConnection);
    if (m_upgradeVerification.isEmpty()) { m_upgrade->settingsVerified(true); return; }
    const auto record = m_upgradeVerification.takeFirst();
    auto *objects = pm->getObject<UAVObjectManager>();
    auto *object = objects->getObject(record.id, record.instance);
    if (!object) { m_upgrade->settingsVerified(false, tr("A saved setting is unavailable in the new firmware.")); return; }
    m_upgradeReadbackConnection = connect(object, &UAVObject::objectUnpacked, this, [this, record](UAVObject *received) {
        m_upgradeReadbackTimer.stop(); disconnect(m_upgradeReadbackConnection);
        QByteArray actual(received->getNumBytes(), 0);
        received->pack(reinterpret_cast<quint8 *>(actual.data()));
        if (actual != record.data) {
            m_upgrade->settingsVerified(false, tr("%1 differs from the saved backup.").arg(received->getName())); return;
        }
        QTimer::singleShot(0, this, &UploaderGadgetWidget::verifyNextUpgradeSetting);
    });
    m_upgradeReadbackTimer.start(10000);
    object->requestUpdate();
}

/**
 * @brief slot called when the user selects the Export Config button.
 * Exports settings locally, retaining the raw partition when its schema is unknown.
 */
void UploaderGadgetWidget::onExportButtonClick()
{
    if (telMngr->isConnected()) {
        /* select the UAV-oriented export thing */
        Core::ActionManager *am = Core::ICore::instance()->actionManager();

        if (!am) {
            qWarning() << "Could not get ActionManager instance!";
            return;
        }

        Core::Command *cmd = am->command("UAVSettingsImportExportPlugin.UAVSettingsExport");

        if (cmd)
            cmd->action()->trigger();
        else
            qWarning() << "Could find UAVSettingsExport action!";

        return;
    }

    /* make sure there's a setting partition */
    if (!haveSettingsPart()) {
        setStatusInfo(tr("No settings partition is accessible. Upgrade the bootloader to export settings."), uploader::STATUSICON_FAIL);
        return;
    }

    /* pull down settings partition to ram */
    if (!downloadSettings()) {
        setStatusInfo(tr("Error, unable to pull settings partition"), uploader::STATUSICON_FAIL);

        return;
    }

    const auto target = Core::FirmwareCatalog::instance().board(currentBoard.board->shortName()).target;
    const auto result = FirmwareSettingsBackup::inspect(tempArray, target,
        m_bootloaderFirmwareValid,
        pm->getObject<UAVObjectManager>());
    QString error;
    const auto path = FirmwareSettingsBackup::save(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("firmware-backups"), tempArray,
        {{"target", target}, {"controllerSerial", currentBoard.cpu_serial},
         {"sourceDescriptor", QString::fromLatin1(m_bootloaderDescription.description.toHex())},
         {"compatible", result.compatible}, {"compatibilityReport", result.reason}}, result.xml, &error);
    setUploaderStatus(uploader::BL_SITTING);
    setStatusInfo(path.isEmpty() ? error : tr("Settings backup saved to %1. %2").arg(path,
        result.compatible ? tr("A local XML export is included when settings are present.") : result.reason),
        path.isEmpty() ? uploader::STATUSICON_FAIL : uploader::STATUSICON_OK);
}

/**
 * @brief slot called by the USBSignalFilter when it detects a board in bootloader state
 * Fills out partition info, device info, and firmware info
 */
void UploaderGadgetWidget::onBootloaderDetected()
{
    if (conMngr->firmwareInstallationActive()) return;
    if (dfu.isRunning()) return;
    if (upgraderActive && m_upgrade->state() != FirmwareUpgradeController::WaitingBootloader
        && m_upgrade->state() != FirmwareUpgradeController::WaitingFirmware
        && m_upgrade->state() != FirmwareUpgradeController::Reconnect) return;
    Core::BoardManager *brdMgr = Core::ICore::instance()->boardManager();
    QList<USBPortInfo> devices;

    // If we are already connected to a bootloader, skip
    if (uploaderStatus == uploader::BL_SITTING) {
        return;
    }

    foreach (int vendorID, brdMgr->getKnownVendorIDs()) {
        devices.append(
            USBMonitor::instance()->availableDevices(vendorID, -1, -1, USBMonitor::Upgrader));
        devices.append(
            USBMonitor::instance()->availableDevices(vendorID, -1, -1, USBMonitor::Bootloader));
    }

    if (devices.length() > 1) {
        setStatusInfo(tr("More than one device was detected in bootloader state"),
                      uploader::STATUSICON_INFO);
        return;
    } else if (devices.length() == 0) {
        setStatusInfo("No devices in bootloader state detected", uploader::STATUSICON_FAIL);
        return;
    }
    conMngr->showFirmwarePage();

    USBPortInfo device = devices.first();

    bool triggerUpgrading = false;
    bool inUpgrader = false;
    bool validDescription = false;

    if (device.getRunState() == USBMonitor::Upgrader) {
        inUpgrader = true;
    }

    if (dfu.OpenBootloaderComs(device)) {
        conMngr->setControllerSerial(device.serialNumber);
        tl_dfu::device dev = dfu.findCapabilities();
        if (upgraderActive && (!FirmwareUpgradeController::sameControllerSerial(device.serialNumber, m_upgrade->context().serial)
            || dev.ID != m_upgrade->context().boardId)) {
            dfu.CloseBootloaderComs(); m_upgrade->fail(tr("A different controller connected during the upgrade. Reconnect the controller you were updating.")); return;
        }

        QByteArray description = dfu.DownloadDescriptionAsByteArray(dev.SizeOfDesc);
        deviceDescriptorStruct descStructure{};
        if (!UAVObjectUtilManager::descriptionToStructure(description, descStructure))
            setStatusInfo(tr("Could not parse firmware metadata"), uploader::STATUSICON_INFO);
        else {
            FirmwareOnDeviceUpdate(descStructure, QString::number((dev.FW_CRC)));
            validDescription = true;
        }

        if (!inUpgrader) {
            switch (uploaderStatus) {
            case uploader::UPGRADING:
                triggerUpgrading = true;
            case uploader::UPGRADING_CATCHLOADER:
            case uploader::ENTERING_LOADER:
                break;
            case uploader::DISCONNECTED: {
                // look for completed bootloader update (last 2 chars of TlFw string are nulled)
                if (QString(description.left(4)) == "Tl")
                    break;

                if (!validDescription) break; /* Offer installation in the controller card. */
            }
                Q_FALLTHROUGH();
            default:
                dfu.JumpToApp(false);
                dfu.CloseBootloaderComs();
                return;
            }
        }

        // Bootloader has new cap extensions, query partitions and fill out browser
        if (dev.CapExt) {
            QTableWidgetItem *label;
            QTableWidgetItem *size;
            int index = 0;
            int row_count = 0;

            QString name;

            foreach (int i, dev.PartitionSizes) {
                if (i <= 0) {
                    /* PartitionSizes is now sparse; there may be 0 sized parts
                     * in the middle.
                     */

                    ++index;
                    continue;
                }

                switch (index) {
                case DFU_PARTITION_FW:
                    name = "Firmware";
                    break;
                case DFU_PARTITION_DESC:
                    name = "Metadata";
                    break;
                case DFU_PARTITION_BL:
                    name = "Bootloader";
                    break;
                case DFU_PARTITION_SETTINGS:
                    name = "Settings";
                    break;
                case DFU_PARTITION_AUTOTUNE:
                    name = "AutoTune";
                    break;
                case DFU_PARTITION_LOG:
                    name = "Log";
                    break;
                case DFU_PARTITION_LOADABLE_EXTENSION:
                    name = "Extension";
                    break;
                default:
                    name = QString::number(index);
                    break;
                }
                label = new QTableWidgetItem(name);
                size = new QTableWidgetItem(QString::number(i));

                label->setData(Qt::UserRole, index);
                size->setData(Qt::UserRole, index);

                size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

                m_widget->partitionBrowserTW->setRowCount(row_count + 1);

                m_widget->partitionBrowserTW->setItem(row_count, 0, label);
                m_widget->partitionBrowserTW->setItem(row_count, 1, size);
                ++index;
                ++row_count;
            }

            m_widget->partitionBrowserGB->setEnabled(true);
        } else {
            m_widget->partitionBrowserTW->setRowCount(0);
        }

        deviceInfo info;
        info.board = brdMgr->getBoard(dev.ID >> 8);
        if (!info.board) {
            setStatusInfo(tr("Board not supported!"), uploader::STATUSICON_FAIL);
            return;
        }
        info.bl_version = QString::number(dev.BL_Version, 16);
        info.cpu_serial = device.serialNumber;
        info.hw_revision = QString::number(dev.HW_Rev);
        info.max_code_size = QString::number(dev.SizeOfCode);
        DeviceInformationUpdate(info);
        m_bootloaderFirmwareValid = validDescription;
        m_bootloaderDescription = descStructure;
        if (!validDescription) {
            m_bootloaderDescription.boardType = dev.ID >> 8;
            m_bootloaderDescription.boardRevision = dev.ID & 0xff;
        }
        const bool updateAvailable = !inUpgrader && !triggerUpgrading
            && FirmwareCheckForUpdate(m_bootloaderDescription, !validDescription);

        iapUpdated = false;

        if (!inUpgrader) {
            setStatusInfo(tr("Connection to bootloader successful"), uploader::STATUSICON_OK);

            loadedFile.clear();
            if (FirmwareLoadFromFile(getImagePath(info.board->shortName()), &loadedFile)) {
                FirmwareLoadedClear(true);
                FirmwareLoadedUpdate(loadedFile);
                setStatusInfo(tr("Ready to flash firmware"), uploader::STATUSICON_OK);
                this->activateWindow();
                m_widget->flashButton->setFocus();
            }

            setUploaderStatus(uploader::BL_SITTING);
            if (!triggerUpgrading) {
                setStatusInfo(validDescription
                    ? updateAvailable ? tr("Update firmware, or choose Start controller to connect to the installed firmware.")
                        : tr("Choose Start controller to connect to the installed firmware.")
                    : updateAvailable ? tr("Install DroniNG firmware to start this controller.")
                        : tr("No compatible firmware image is included in this build. Open Maintenance & recovery to load a firmware file."), uploader::STATUSICON_INFO);
            }
        } else {
            setStatusInfo(tr("Connected to upgrader-loader"), uploader::STATUSICON_OK);
            setUploaderStatus(uploader::BL_SITTING);
        }

        if (triggerUpgrading) {
            if (currentBoard.board) {
                bool canBeUpgraded = currentBoard.board->queryCapabilities(
                    Core::IBoardType::BOARD_CAPABILITIES_UPGRADEABLE);

                if (canBeUpgraded) {
                    doUpgradeOperation(!validDescription, dev);
                    return;
                }
            }
        }

        emit enteredLoader();
        if (upgraderActive && m_upgrade->state() == FirmwareUpgradeController::WaitingBootloader)
            m_upgrade->bootloaderReady(dev.ID, device.serialNumber, dev.SizeOfCode,
                dev.PartitionSizes.value(DFU_PARTITION_SETTINGS, 0),
                dev.BL_Version >= info.board->minBootLoaderVersion());
    } else {
        setStatusInfo(tr("Could not open coms with the bootloader"), uploader::STATUSICON_FAIL);
        setUploaderStatus((uploader::DISCONNECTED));
        conMngr->resumePolling();
    }
}

/**
 * @brief slot called by the USBSignalFilter when it detects a board in bootloader state as been
 * removed
 */
void UploaderGadgetWidget::onBootloaderRemoved()
{
    if (m_upgrade) m_upgrade->deviceRemoved();
    m_bootloaderFirmwareValid = false;
    m_bootloaderDescription = {};
    m_firmwarePage->setLegacyBootloaderState(false);
    m_firmwarePage->setLegacyFirmwareUpdate();
    if (!conMngr->isConnected() && !upgraderActive) conMngr->setFirmwareIdentity({}, {});
    conMngr->resumePolling();
    setStatusInfo(tr("Bootloader disconnection detected"), uploader::STATUSICON_INFO);
    DeviceInformationClear();
    FirmwareOnDeviceClear(true);
    PartitionBrowserClear();

    if (uploaderStatus != uploader::UPGRADING_CATCHLOADER) {
        setUploaderStatus(uploader::DISCONNECTED);
    }
}

void UploaderGadgetWidget::startRescue()
{
    setStatusInfo(tr("Trying to detect bootloader"), uploader::STATUSICON_INFO);
    m_widget->progressBar->setValue(100);

    connect(&rescueTimer, &QTimer::timeout, this, &UploaderGadgetWidget::onRescueTimer,
            Qt::UniqueConnection);
    rescueTimer.start(200);

    connect(usbFilterBL, &USBSignalFilter::deviceDiscovered, this,
            &UploaderGadgetWidget::onDeviceDiscovered, Qt::UniqueConnection);
    connect(usbFilterUP, &USBSignalFilter::deviceDiscovered, this,
            &UploaderGadgetWidget::onDeviceDiscovered, Qt::UniqueConnection);
}

/**
 * @brief slot called by a timer created by itself or by the onRescueButton
 * @param start true if the rescue timer is to be started
 */
void UploaderGadgetWidget::onRescueTimer()
{
    m_widget->progressBar->setValue(m_widget->progressBar->value() - 1);

    if (m_widget->progressBar->value() <= 0) {
        disconnect(usbFilterBL, &USBSignalFilter::deviceDiscovered, this,
                   &UploaderGadgetWidget::onDeviceDiscovered);
        disconnect(usbFilterUP, &USBSignalFilter::deviceDiscovered, this,
                   &UploaderGadgetWidget::onDeviceDiscovered);
        rescueTimer.disconnect();
        setStatusInfo(tr("Failed to detect bootloader"), uploader::STATUSICON_FAIL);
        setUploaderStatus(uploader::DISCONNECTED);
        conMngr->resumePolling();
        // Native rescue needs an existing dRonin bootloader. Offer ROM DFU first-install
        // when that loader never appears, including boards already held in BOOT mode.
        conMngr->showFirmwarePage();
    }
}

void UploaderGadgetWidget::onDeviceDiscovered()
{
    rescueTimer.stop();
    m_widget->progressBar->setValue(0);
    disconnect(usbFilterBL, &USBSignalFilter::deviceDiscovered, this,
               &UploaderGadgetWidget::onDeviceDiscovered);
    disconnect(usbFilterUP, &USBSignalFilter::deviceDiscovered, this,
               &UploaderGadgetWidget::onDeviceDiscovered);
}

/**
 * @brief slot called by the DFUObject to update operations status
 * @param text text corresponding to the status
 * @param progress 0 to 100 corresponding the the current operation progress
 */
void UploaderGadgetWidget::onStatusUpdate(QString text, int progress)
{
    if (upgraderActive && progress >= 0) m_upgrade->transferProgress(progress);
    if (!text.isEmpty())
        setStatusInfo(text, uploader::STATUSICON_RUNNING);
    if (progress != -1)
        m_widget->progressBar->setValue(progress);
}

bool UploaderGadgetWidget::triggerPartitionDownload(int index)
{
    if (!CheckInBootloaderState())
        return false;

    tl_dfu::device dev = dfu.findCapabilities();
    const int size = dev.PartitionSizes.value(index, 0);
    if (size <= 0 || size > 16 * 1024 * 1024 || size % 4) {
        setStatusInfo(tr("The bootloader reported an invalid partition size."), uploader::STATUSICON_FAIL); return false;
    }

    setStatusInfo("", uploader::STATUSICON_RUNNING);
    setUploaderStatus(uploader::BL_BUSY);
    tempArray.clear();
    return dfu.DownloadPartitionThreaded(&tempArray, (dfu_partition_label)index, size);
}

/**
 * @brief slot called when the user clicks save on the partition browser
 */
void UploaderGadgetWidget::onPartitionSave()
{
    int index = m_widget->partitionBrowserTW->selectedItems().first()->data(Qt::UserRole).toInt();

    QEventLoop loop;

    bool operationSuccess = false;

    /* disconnects when loop comes out of scope */
    connect(&dfu, &DFUObject::downloadFinished, &loop, [&](bool status) {
        operationSuccess = status;
        loop.exit();
    });

    if (!triggerPartitionDownload(index)) return;

    loop.exec();

    if (operationSuccess) {
        setStatusInfo(tr("Partition download success"), uploader::STATUSICON_OK);
        QString filename =
            QFileDialog::getSaveFileName(this, tr("Save File"), QDir::homePath(), "*.bin");
        if (filename.isEmpty()) {
            setStatusInfo(tr("Save cancelled"), uploader::STATUSICON_FAIL);
            setUploaderStatus(uploader::BL_SITTING);
            return;
        }

        if (!filename.endsWith(".bin", Qt::CaseInsensitive))
            filename.append(".bin");
        QFile file(filename);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(tempArray);
            file.close();
            setStatusInfo(tr("Partition written to file"), uploader::STATUSICON_OK);
        } else
            setStatusInfo(tr("Error could not open file for save"), uploader::STATUSICON_FAIL);
    } else {
        setStatusInfo(tr("Partition download failed"), uploader::STATUSICON_FAIL);
    }

    setUploaderStatus(uploader::BL_SITTING);
}

/**
 * @brief slot called when the user clicks flash on the partition browser
 */
void UploaderGadgetWidget::onPartitionFlash()
{
    if (!CheckInBootloaderState())
        return;
    QString filename =
        QFileDialog::getOpenFileName(this, tr("Open File"), QDir::homePath(), "*.bin");
    if (filename.isEmpty()) {
        setStatusInfo(tr("Flash cancelled"), uploader::STATUSICON_FAIL);
        return;
    }
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatusInfo(tr("Could not open file for read"), uploader::STATUSICON_FAIL);
        return;
    }
    tempArray.clear();
    tempArray = file.readAll();

    int index = m_widget->partitionBrowserTW->selectedItems().first()->data(Qt::UserRole).toInt();
    tl_dfu::device dev = dfu.findCapabilities();
    int size = dev.PartitionSizes[index];

    if (tempArray.length() > size) {
        setStatusInfo(tr("File bigger than partition size"), uploader::STATUSICON_FAIL);
        return;
    }
    if (promptFirmware(tr("Flash the selected partition?"),
        tr("This replaces the partition's contents."), QDialogButtonBox::Yes | QDialogButtonBox::No, tr("Flash partition"))
        != QDialogButtonBox::Yes)
        return;
    setStatusInfo("", uploader::STATUSICON_RUNNING);
    setUploaderStatus(uploader::BL_BUSY);

    tl_dfu::Status operationSuccess = DFUidle;

    QEventLoop loop;

    /* disconnects when loop comes out of scope */
    connect(&dfu, &DFUObject::uploadFinished, &loop, [&](tl_dfu::Status status) {
        operationSuccess = status;
        loop.exit();
    });

    dfu.UploadPartitionThreaded(tempArray, (dfu_partition_label)index, size);

    loop.exec();

    if (operationSuccess == Last_operation_Success)
        setStatusInfo(tr("Partition upload success"), uploader::STATUSICON_OK);
    else
        setStatusInfo(tr("Partition upload failed"), uploader::STATUSICON_FAIL);
    setUploaderStatus(uploader::BL_SITTING);
}

/**
 * @brief slot called when the user clicks erase on the partition browser
 */
void UploaderGadgetWidget::onPartitionErase()
{
    int index = m_widget->partitionBrowserTW->selectedItems().first()->data(Qt::UserRole).toInt();

    if (promptFirmware(tr("Erase the selected partition?"),
        tr("This removes the partition's contents."), QDialogButtonBox::Yes | QDialogButtonBox::No, tr("Erase partition"))
        != QDialogButtonBox::Yes)
        return;
    if (dfu.WipePartition((dfu_partition_label)index))
        setStatusInfo(tr("Partition erased"), uploader::STATUSICON_OK);
    else
        setStatusInfo(tr("Partition erasure failed"), uploader::STATUSICON_FAIL);
}

/**
 * @brief slot called when the user clicks the boot button
 * Atempts to Boot the board and starts a timeout timer
 */
void UploaderGadgetWidget::onBootButtonClick()
{
    bool safeboot = (sender() == m_widget->safeBootButton);

    if (telMngr->isConnected()) {
        haltOrReset(false);
        return;
    }

    if (!CheckInBootloaderState())
        return;

    if (dfu.JumpToApp(safeboot) < 1) {
        setStatusInfo(tr("The controller could not start. Reconnect USB, then choose Start controller again."), uploader::STATUSICON_FAIL);
        return;
    }
    dfu.CloseBootloaderComs();
    conMngr->resumePolling();
    setUploaderStatus(uploader::DISCONNECTED);
    setStatusInfo(tr("Starting controller. Waiting for its USB connection…"), uploader::STATUSICON_INFO);
}

/**
 * @brief Checks if all conditions are met for the autopilot to be ready for required operations
 */
void UploaderGadgetWidget::CheckAutopilotReady()
{
    if (telemetryConnected && iapUpdated) {
        onAutopilotReady();
    }
}

/**
 * @brief Check if the board is in bootloader state
 * @return true if in bootloader state
 */
bool UploaderGadgetWidget::CheckInBootloaderState()
{
    if (uploaderStatus != uploader::BL_SITTING) {
        setStatusInfo(tr("Cannot perform the selected operation if not in bootloader state"),
                      uploader::STATUSICON_FAIL);
        return false;
    } else
        return true;
}

/**
 *Opens an open file dialog pointing to the file which corresponds to the connected board if
 *possible
 */
QString UploaderGadgetWidget::LoadFirmwareFileDialog(QString boardName)
{
    QFileDialog::Options options;
    QString selectedFilter;
    boardName = boardName.toLower();

    QString fwDirectoryStr = getImagePath(boardName);

    QString fileName = QFileDialog::getOpenFileName(
        this, tr("Select firmware file"), fwDirectoryStr, tr("Firmware (fw_*.tlfw);;"
                                                             "Bootloader Update (bu_*.tlfw);;"
                                                             "All (*.tlfw *.opfw *.bin)"),
        &selectedFilter, options);
    return fileName;
}

/**
 * @brief Updates the status message
 * @param str string containing the text which describes the current status
 * @param ic icon which describes the current status
 */
void UploaderGadgetWidget::setStatusInfo(QString str, uploader::StatusIcon ic)
{
    qDebug() << str;

    QPixmap px;
    m_widget->statusLabel->setText(str);
    if (conMngr && !str.isEmpty()) conMngr->setFirmwareStatus(str);
    switch (ic) {
    case uploader::STATUSICON_RUNNING:
        px.load(QString(":/uploader/images/system-run.svg"));
        break;
    case uploader::STATUSICON_OK:
        px.load(QString(":/uploader/images/dialog-apply.svg"));
        break;
    case uploader::STATUSICON_FAIL:
        px.load(QString(":/uploader/images/process-stop.svg"));
        break;
    default:
        px.load(QString(":/uploader/images/gtk-info.svg"));
    }
    m_widget->statusPic->setPixmap(px);
}

/**
 * @brief Returns the current uploader status
 */
uploader::UploaderStatus UploaderGadgetWidget::getUploaderStatus() const
{
    return uploaderStatus;
}

bool UploaderGadgetWidget::haveSettingsPart() const
{
    /* Would be nice to determine this in a nicer way */
    if (m_widget->partitionBrowserTW->rowCount() >= DFU_PARTITION_SETTINGS) {
        return true;
    }

    return false;
}

/**
 * @brief Sets the current uploader status
 * Enables, disables, hides, unhides widgets according to the new status
 * @param value value of the new status
 */
void UploaderGadgetWidget::setUploaderStatus(const uploader::UploaderStatus &value)
{
    uploaderStatus = value;
    if (m_firmwarePage) {
        const bool idleBootloader = value == uploader::BL_SITTING && !upgraderActive && !m_promptActive;
        m_firmwarePage->setOperationActive(m_promptActive || (active() && !idleBootloader));
        m_firmwarePage->setLegacyBootloaderState(idleBootloader, m_bootloaderFirmwareValid);
    }
    switch (uploaderStatus) {
    case uploader::DISCONNECTED:
        m_widget->progressBar->setVisible(false);

        m_widget->rescueButton->setEnabled(true);
        m_widget->rescueButton->setText(tr("Recover DroniNG firmware"));
        m_widget->bootButton->setText(tr("Boot"));

        m_widget->openButton->setEnabled(true);
        m_widget->bootButton->setEnabled(false);
        m_widget->safeBootButton->setEnabled(false);
        m_widget->flashButton->setEnabled(false);
        m_widget->exportConfigButton->setEnabled(false);
        m_widget->partitionBrowserTW->setContextMenuPolicy(Qt::NoContextMenu);
        break;
    case uploader::ENTERING_LOADER:
        m_widget->progressBar->setVisible(true);

        m_widget->rescueButton->setText(tr("Recover DroniNG firmware"));
        m_widget->bootButton->setText(tr("Boot"));

        m_widget->rescueButton->setEnabled(false);
        m_widget->openButton->setEnabled(true);
        m_widget->bootButton->setEnabled(false);
        m_widget->safeBootButton->setEnabled(false);
        m_widget->flashButton->setEnabled(false);
        m_widget->exportConfigButton->setEnabled(false);
        m_widget->partitionBrowserTW->setContextMenuPolicy(Qt::NoContextMenu);
        break;
    case uploader::BL_SITTING:
        m_widget->progressBar->setVisible(false);

        m_widget->rescueButton->setText(tr("Recover DroniNG firmware"));
        m_widget->bootButton->setText(tr("Boot"));

        m_widget->rescueButton->setEnabled(false);
        m_widget->openButton->setEnabled(true);
        m_widget->bootButton->setEnabled(true);
        m_widget->safeBootButton->setEnabled(true);
        if (!loadedFile.isEmpty())
            m_widget->flashButton->setEnabled(true);
        else
            m_widget->flashButton->setEnabled(false);

        m_widget->exportConfigButton->setEnabled(haveSettingsPart());

        m_widget->partitionBrowserTW->setContextMenuPolicy(Qt::ActionsContextMenu);
        break;
    case uploader::CONNECTED_TO_TELEMETRY:
        m_widget->progressBar->setVisible(false);

        m_widget->rescueButton->setText(tr("Enter bootloader"));
        m_widget->bootButton->setText(tr("Reboot"));

        m_widget->rescueButton->setEnabled(true);
        m_widget->openButton->setEnabled(true);
        m_widget->bootButton->setEnabled(true);
        m_widget->safeBootButton->setEnabled(false);
        m_widget->flashButton->setEnabled(false);
        m_widget->exportConfigButton->setEnabled(true);

        m_widget->partitionBrowserTW->setContextMenuPolicy(Qt::NoContextMenu);
        break;
    case uploader::UPGRADING:
    case uploader::UPGRADING_CATCHLOADER:
    case uploader::BL_BUSY:
        m_widget->progressBar->setVisible(true);

        m_widget->rescueButton->setEnabled(false);
        m_widget->openButton->setEnabled(false);
        m_widget->bootButton->setEnabled(false);
        m_widget->safeBootButton->setEnabled(false);
        m_widget->flashButton->setEnabled(false);
        m_widget->exportConfigButton->setEnabled(false);
        m_widget->partitionBrowserTW->setContextMenuPolicy(Qt::NoContextMenu);
        break;
    default:
        break;
    }
}

/**
 * @brief Opens the plugin help page on the default browser
 */
void UploaderGadgetWidget::openHelp()
{
    QDesktopServices::openUrl(QUrl(
        "https://github.com/d-ronin/dRonin/wiki/OnlineHelp:-Uploader-Plugin", QUrl::StrictMode));
}

QString UploaderGadgetWidget::getImagePath(QString boardName, QString imageType)
{
    if (imageType != "fw" && imageType != "bu")
        return {};
    const auto board = Core::FirmwareCatalog::instance().board(boardName);
    return Core::FirmwareCatalog::imagePath(board, imageType == "bu"
        ? Core::FirmwareCatalog::ImageKind::BootloaderUpdate
        : Core::FirmwareCatalog::ImageKind::Application);
}

bool UploaderGadgetWidget::FirmwareLoadFromFile(QString filename, QByteArray *contents)
{
    QFileInfo fileinfo = QFileInfo(filename);

    if (!fileinfo.exists())
        return false;

    QFile file(fileinfo.filePath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *contents = file.readAll();

    return true;
}

bool UploaderGadgetWidget::FirmwareCheckForUpdate(deviceDescriptorStruct device, bool blank)
{
    m_firmwarePage->setLegacyFirmwareUpdate();
    if (!currentBoard.board || !Core::FirmwareCatalog::instance().board(currentBoard.board->shortName()).layoutId.isEmpty()
        || !currentBoard.board->queryCapabilities(Core::IBoardType::BOARD_CAPABILITIES_UPGRADEABLE))
        return false;
    QByteArray candidate;
    deviceDescriptorStruct available;
    if (!FirmwareLoadFromFile(getImagePath(currentBoard.board->shortName()), &candidate)
        || !utilMngr->descriptionToStructure(candidate.right(100), available)
        || available.boardID() != device.boardID()
        || (!blank && available.description.mid(40, 40) == device.description.mid(40, 40)))
        return false;
    m_firmwarePage->setLegacyFirmwareUpdate(blank ? tr("not installed") : device.gitHash, available.gitHash);
    return true;
}

bool UploaderGadgetWidget::active() const
{
    switch (uploaderStatus) {
    case DISCONNECTED:
    case CONNECTED_TO_TELEMETRY:
        return upgraderActive;
    case ENTERING_LOADER:
    case BL_SITTING:
    case BL_BUSY:
    case UPGRADING:
    case UPGRADING_CATCHLOADER:
        break;
    }
    return true;
}
