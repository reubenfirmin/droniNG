/**
 ******************************************************************************
 *
 * @file       uploadergadgetwidget.h
 * @author     dRonin, http://dRonin.org/, Copyright (C) 2016
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

#ifndef UPLOADERGADGETWIDGET_H
#define UPLOADERGADGETWIDGET_H

#include <QPointer>
#include <QDialogButtonBox>
#include "tl_dfu.h"
#include <coreplugin/iboardtype.h>
#include "uploader_global.h"
#include "uavobjectutil/devicedescriptorstruct.h"
#include "uavobjectutil/uavobjectutilmanager.h"
#include "uavtalk/telemetrymanager.h"
#include "coreplugin/connectionmanager.h"
#include "uavsettingsimportexport/uavsettingsimportexportmanager.h"
#include "upgradeassistantdialog.h"

class QGridLayout;
class Ui_UploaderWidget;
namespace Core { class FirmwarePage; }

using namespace tl_dfu;

namespace uploader {

typedef enum { STATUSICON_OK, STATUSICON_RUNNING, STATUSICON_FAIL, STATUSICON_INFO } StatusIcon;

class UPLOADER_EXPORT UploaderGadgetWidget : public QWidget
{
    Q_OBJECT

public:
    UploaderGadgetWidget(QWidget *parent = nullptr);
    ~UploaderGadgetWidget();

    /**
     * @brief active
     * @return true if the uploader is currently doing something (e.g. upgrader)
     */
    bool active() const;
protected:
    bool eventFilter(QObject *object, QEvent *event) override;

signals:
    void newBoardSeen(deviceInfo board, deviceDescriptorStruct device);
    void enteredLoader();
private slots:
    void onAutopilotConnect();
    void onAutopilotDisconnect();
    void onAutopilotReady();
    void onFirmwareUpdateRequested();
    void onFirmwareBootRequested();
    void onIAPUpdated();
    void onLoadFirmwareButtonClick();
    void onFlashButtonClick();
    void onRescueButtonClick();
    void onExportButtonClick();
    void onBootloaderDetected();
    void onBootloaderRemoved();
    void onRescueTimer();
    void onStatusUpdate(QString, int);
    void onPartitionSave();
    void onPartitionFlash();
    void onPartitionErase();
    void onBootButtonClick();
    void openHelp();
    void onDeviceDiscovered();

private:
    void FirmwareOnDeviceClear(bool clear);
    void FirmwareLoadedClear(bool clear);
    void PartitionBrowserClear();
    void DeviceInformationClear();
    void DeviceInformationUpdate(deviceInfo board);
    void FirmwareOnDeviceUpdate(deviceDescriptorStruct firmware, QString crc);
    void FirmwareLoadedUpdate(QByteArray firmwareArray);
    QString LoadFirmwareFileDialog(QString);
    uploader::UploaderStatus getUploaderStatus() const;
    void setUploaderStatus(const uploader::UploaderStatus &value);
    void CheckAutopilotReady();
    bool CheckInBootloaderState();
    /* XXX TODO: make capitalization consistent */
    void setStatusInfo(QString str, uploader::StatusIcon ic);
    QString getImagePath(QString boardName, QString imageType = QString("fw"));
    bool FirmwareLoadFromFile(QString filename, QByteArray *contents);
    bool FirmwareCheckForUpdate(deviceDescriptorStruct device, bool blank = false);
    bool triggerPartitionDownload(int index);
    void haltOrReset(bool halting);
    void startRescue();
    int promptFirmware(const QString &text, const QString &details,
                       QDialogButtonBox::StandardButtons buttons, const QString &action);
    bool m_promptActive = false;
    bool downloadSettings();
    void doUpgradeOperation(bool blankFC, tl_dfu::device &dev);
    void finishUpgradeTransfer();
    void verifyUpgradeSettings(const QList<FirmwareSettingRecord> &records);
    void verifyNextUpgradeSetting();
    bool flashFirmware(QByteArray &firmwareImage);
    bool haveSettingsPart() const;

    Ui_UploaderWidget *m_widget;
    Core::FirmwarePage *m_firmwarePage = nullptr;
    QWidget *m_advancedTools = nullptr;
    QGridLayout *m_recoveryActions = nullptr;
    QList<QWidget *> m_recoveryButtons;

    UpgradeAssistantDialog m_dialog;
    FirmwareUpgradeController *m_upgrade = nullptr;
    enum UpgradeTransfer { NoUpgradeTransfer, BackupTransfer, FirmwareTransfer, MetadataTransfer,
        ResetTransfer, SettingsConversionTransfer, SettingsConversionReadbackTransfer };
    UpgradeTransfer m_upgradeTransfer = NoUpgradeTransfer;
    QByteArray m_upgradeBuffer, m_upgradeMetadata;
    bool m_upgradeTransferSuccess = false;
    QList<FirmwareSettingRecord> m_upgradeVerification;
    QMetaObject::Connection m_upgradeReadbackConnection;
    QTimer m_upgradeReadbackTimer;

    bool telemetryConnected;
    bool iapUpdated;

    QByteArray loadedFile;

    DFUObject dfu;
    USBSignalFilter *usbFilterBL;
    USBSignalFilter *usbFilterUP;
    ExtensionSystem::PluginManager *pm;
    TelemetryManager *telMngr;
    UAVObjectUtilManager *utilMngr;
    Core::ConnectionManager *conMngr = nullptr;

    FirmwareIAPObj *firmwareIap;
    deviceInfo currentBoard;
    deviceDescriptorStruct m_bootloaderDescription{};
    bool m_bootloaderFirmwareValid = false;

    uploader::UploaderStatus uploaderStatus;
    QByteArray tempArray;
    QTimer rescueTimer;
    bool upgraderActive = false;

};
}
#endif // UPLOADERGADGETWIDGET_H
