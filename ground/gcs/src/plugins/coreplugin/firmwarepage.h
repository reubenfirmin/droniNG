/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FIRMWAREPAGE_H
#define FIRMWAREPAGE_H
#include "core_global.h"
#include "connectionmanager.h"
#include <QWidget>
#include <QPointer>
class QLabel;
class QComboBox;
class QPushButton;
class QPlainTextEdit;
class QToolButton;
class QProcess;
class QProgressBar;
class QVBoxLayout;
class QGridLayout;
namespace Core {
class ConnectionManager;
class FirmwareInstallWidget;
class CORE_EXPORT FirmwarePage : public QWidget {
    Q_OBJECT
public:
    explicit FirmwarePage(ConnectionManager *manager, QWidget *parent = nullptr);
    void setOperationActive(bool active);
    void setRecoveryTools(QWidget *tools);
private:
    void refresh();
    void refreshDevices();
    void detectBootloader();
    ConnectionManager *m_manager;
    QLabel *m_board, *m_firmware, *m_bootloader, *m_status, *m_guidance, *m_command, *m_notes;
    QLabel *m_title, *m_subtitle, *m_deviceHint, *m_setupIntro;
    QLabel *m_usbTitle, *m_usbHint;
    QLabel *m_currentHeading, *m_bootloaderCaption, *m_installFirmware, *m_installSettings;
    QComboBox *m_devices;
    QList<DevListItem> m_candidates;
    QPushButton *m_connect, *m_disconnect, *m_flight, *m_configure, *m_scopes;
    QWidget *m_connectionCard, *m_deviceCard, *m_errorCard, *m_recoveryCard, *m_diagnostics;
    QToolButton *m_recoveryToggle;
    QToolButton *m_back;
    QVBoxLayout *m_contentLayout;
    QGridLayout *m_identity;
    QComboBox *m_boards;
    QPushButton *m_install, *m_copy;
    QToolButton *m_toggle;
    QPlainTextEdit *m_details;
    QWidget *m_selection;
    QWidget *m_boardIdentity, *m_firmwareIdentity, *m_installIdentity;
    QPointer<FirmwareInstallWidget> m_installation;
    QProcess *m_probe;
    QProgressBar *m_detectionProgress;
    QString m_target, m_romPath;
    QString m_profileId, m_imageError;
    bool m_imageChecked = false, m_imageAvailable = false;
    bool m_externalBusy = false;
    bool m_detecting = false, m_bootloaderScanned = false;
    bool m_manualBoot = false;
};
}
#endif
