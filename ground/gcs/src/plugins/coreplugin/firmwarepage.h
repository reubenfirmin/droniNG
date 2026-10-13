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
class QStackedLayout;
class QScrollArea;
class QTimer;
namespace Core {
class ConnectionManager;
class FirmwareInstallWidget;
class DfuCommandRunner;
class CORE_EXPORT FirmwarePage : public QWidget {
    Q_OBJECT
public:
    explicit FirmwarePage(ConnectionManager *manager, QWidget *parent = nullptr, DfuCommandRunner *installRunner = nullptr);
    void setOperationActive(bool active);
    void setRecoveryTools(QWidget *tools);
    void setFlightPanel(int index, QWidget *panel);
    void setLegacyFirmwareUpdate(const QString &installedRevision = {}, const QString &availableRevision = {});
    void setLegacyBootloaderState(bool connected, bool canBoot = false);
    void addOperationWidget(QWidget *widget);
signals:
    void legacyFirmwareUpdateRequested();
    void legacyFirmwareBootRequested();
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    void updateDashboardLayout();
    void updateControllerLayout();
    void refresh();
    void refreshDevices();
    void detectBootloader();
    void startInstallation(const FirmwareBoardProfile &board);
    DevListItem recoveryDevice() const;
    void retryRecoveryConnection();
    ConnectionManager *m_manager;
    DfuCommandRunner *m_installRunner;
    QLabel *m_board, *m_firmware, *m_bootloader, *m_status, *m_guidance, *m_command, *m_notes;
    QLabel *m_modelCaption, *m_hardwareHint, *m_targetCaption, *m_firmwareTarget;
    QLabel *m_title, *m_deviceHint, *m_setupIntro;
    QWidget *m_updateNotice;
    QLabel *m_updateDetails;
    QLabel *m_usbTitle, *m_usbHint;
    QLabel *m_currentHeading, *m_bootloaderCaption, *m_installFirmware, *m_installSettings;
    QComboBox *m_devices;
    QList<DevListItem> m_candidates;
    QPushButton *m_connect, *m_disconnect, *m_configure, *m_scopes, *m_bootController;
    QWidget *m_connectionCard, *m_deviceCard, *m_errorCard, *m_recoveryCard, *m_diagnostics;
    QToolButton *m_recoveryToggle;
    QToolButton *m_back;
    QVBoxLayout *m_contentLayout;
    QGridLayout *m_identity;
    QGridLayout *m_deviceActions;
    QGridLayout *m_dashboard;
    QScrollArea *m_scroll;
    QWidget *m_content, *m_overview, *m_recoveryBody, *m_logCard;
    QList<QWidget *> m_flightCards, m_flightPanels, m_flightOverlays;
    QList<QLabel *> m_flightHints;
    QList<QStackedLayout *> m_flightLayouts;
    QComboBox *m_boards;
    QComboBox *m_hardwareBoards;
    QPushButton *m_install, *m_copy;
    QToolButton *m_toggle;
    QPlainTextEdit *m_details;
    QWidget *m_selection;
    QWidget *m_pendingNotice;
    QLabel *m_pendingMessage;
    QLabel *m_recoveryHint;
    QPushButton *m_retryConnection, *m_recoverInstallation, *m_otherController;
    QWidget *m_boardIdentity, *m_firmwareIdentity, *m_installIdentity;
    QPointer<FirmwareInstallWidget> m_installation;
    QList<QPointer<QWidget>> m_operationWidgets;
    QProcess *m_probe;
    QTimer *m_retryWait;
    QProgressBar *m_detectionProgress;
    QString m_target, m_romPath;
    QString m_profileId, m_imageError;
    QString m_legacyInstalledRevision, m_legacyAvailableRevision;
    bool m_imageChecked = false, m_imageAvailable = false;
    QByteArray m_imageDescriptor;
    bool m_externalBusy = false;
    bool m_legacyBootloader = false, m_legacyBootable = false;
    bool m_detecting = false, m_bootloaderScanned = false;
    bool m_manualBoot = false;
    bool m_pendingDismissed = false;
    bool m_retryWaitExpired = false;
};
}
#endif
