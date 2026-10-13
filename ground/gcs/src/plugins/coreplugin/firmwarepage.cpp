/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firmwarepage.h"
#include "firmwarecatalog.h"
#include "firmwareinstallimage.h"
#include "firmwareinstallwidget.h"
#include "firmwareuicomponents.h"
#include "dfuinstaller.h"
#include "modemanager.h"
#include "imode.h"
#include "gcsversioninfo.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QListView>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPainter>
#include <QResizeEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

using namespace Core;

namespace {
class UnavailableFlightPanel final : public QWidget
{
public:
    explicit UnavailableFlightPanel(QWidget *parent) : QWidget(parent) {}
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QColor shade = palette().color(QPalette::Window);
        shade.setAlpha(220);
        painter.fillRect(rect(), shade);
    }
};
}

FirmwarePage::FirmwarePage(ConnectionManager *manager, QWidget *parent, DfuCommandRunner *installRunner)
    : QWidget(parent), m_manager(manager), m_installRunner(installRunner), m_probe(new QProcess(this))
{
    setObjectName("firmwarePage");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setProperty("pageLayout", true);
    setAutoFillBackground(true);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    m_scroll = scroll;
    scroll->setObjectName("firmwareScrollArea");
    scroll->setAccessibleName(tr("Firmware dashboard"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto *canvas = new QWidget;
    canvas->setObjectName("firmwareCanvas");
    canvas->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    scroll->setWidget(canvas);
    auto *row = new QHBoxLayout(canvas);
    row->setContentsMargins(24, 24, 24, 24);
    row->addStretch();
    auto *content = new QWidget(canvas);
    m_content = content;
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    row->addWidget(content, 1);
    row->addStretch();
    auto *dashboard = new QGridLayout(content);
    m_dashboard = dashboard;
    dashboard->setContentsMargins(0, 0, 0, 0);
    dashboard->setSpacing(16);
    dashboard->setColumnStretch(0, 1); dashboard->setColumnStretch(1, 1);
    dashboard->setRowStretch(0, 1); dashboard->setRowStretch(1, 1);
    auto *overview = FirmwareUi::card(content);
    m_overview = overview;
    overview->setObjectName("firmwareOverview");
    overview->setAccessibleName(tr("Controller setup"));
    overview->setMinimumHeight(224);
    dashboard->addWidget(overview, 0, 0);
    m_contentLayout = qobject_cast<QVBoxLayout *>(overview->layout());
    m_contentLayout->setContentsMargins(20, 20, 20, 20);
    m_contentLayout->setSpacing(16);
    const char *names[] = {"firmwareAttitudeCard", "firmwareModelCard", "firmwareHealthCard"};
    const QString titles[] = {tr("Attitude"), tr("Aircraft"), tr("System health")};
    for (int index = 0; index < 3; ++index) {
        auto *card = FirmwareUi::card(content);
        card->setObjectName(names[index]);
        card->setAccessibleName(titles[index]);
        card->setProperty("flightPanel", true);
        card->setMinimumHeight(224);
        auto *layout = qobject_cast<QVBoxLayout *>(card->layout());
        layout->setContentsMargins(16, 16, 16, 16);
        layout->addWidget(FirmwareUi::label(titles[index], "section", card));
        auto *body = new QWidget(card);
        auto *stack = new QStackedLayout(body);
        stack->setContentsMargins(0, 0, 0, 0);
        stack->setStackingMode(QStackedLayout::StackAll);
        auto *overlay = new UnavailableFlightPanel(body);
        auto *overlayLayout = new QVBoxLayout(overlay);
        auto *hint = FirmwareUi::label(tr("Connect a controller to view live data."), "muted", overlay);
        hint->setAlignment(Qt::AlignCenter);
        overlayLayout->addWidget(hint);
        stack->addWidget(overlay);
        layout->addWidget(body, 1);
        m_flightCards.append(card); m_flightPanels.append(nullptr);
        m_flightOverlays.append(overlay); m_flightLayouts.append(stack); m_flightHints.append(hint);
        dashboard->addWidget(card, index == 0 ? 0 : 1, index == 1 ? 0 : 1);
    }
    m_title = FirmwareUi::label({}, "title", content);
    m_title->setObjectName("firmwarePageTitle");
    m_contentLayout->addWidget(m_title);
    m_pendingNotice = new QWidget(overview);
    m_pendingNotice->setObjectName("firmwarePendingInstallation");
    auto *pendingLayout = new QVBoxLayout(m_pendingNotice);
    pendingLayout->setContentsMargins(0, 0, 0, 0);
    m_pendingMessage = FirmwareUi::label({}, "muted", m_pendingNotice);
    m_pendingMessage->setObjectName("firmwarePendingMessage");
    pendingLayout->addWidget(m_pendingMessage);
    m_recoveryHint = FirmwareUi::label({}, "muted", m_pendingNotice);
    m_recoveryHint->setObjectName("firmwareRecoveryHint");
    pendingLayout->addWidget(m_recoveryHint);
    m_retryConnection = new QPushButton(tr("Retry connection"), overview);
    m_retryConnection->setObjectName("firmwareRetryConnection");
    m_retryConnection->setProperty("primary", true);
    m_retryWait = new QTimer(this);
    m_retryWait->setSingleShot(true);
    m_retryWait->setInterval(10000);
    connect(m_retryWait, &QTimer::timeout, this, [this] { m_retryWaitExpired = true; refresh(); });
    connect(m_retryConnection, &QPushButton::clicked, this, [this] {
        if (m_externalBusy) return;
        m_pendingDismissed = false;
        m_retryWaitExpired = false;
        m_retryWait->start();
        retryRecoveryConnection();
        refresh();
    });
    m_recoverInstallation = new QPushButton(tr("Reinstall firmware…"), overview);
    m_recoverInstallation->setObjectName("firmwareRecoverInstallation");
    connect(m_recoverInstallation, &QPushButton::clicked, this, [this] {
        m_retryWait->stop();
        m_retryWaitExpired = false;
        const auto context = m_manager->controllerRecovery();
        const auto pending = m_manager->verifiedInstallation();
        const QString id = !context.problem.message.isEmpty() ? context.boardId : pending.boardId;
        const auto board = FirmwareCatalog::instance().board(id);
        if (board.isValid()) startInstallation(board);
        else {
            m_manager->disconnectDevice();
            m_manualBoot = true;
            m_boards->setCurrentIndex(0);
            refresh(); detectBootloader();
        }
    });
    m_otherController = new QPushButton(tr("Use another controller"), overview);
    m_otherController->setObjectName("firmwareOtherController");
    connect(m_otherController, &QPushButton::clicked, this, [this] {
        m_retryWait->stop();
        m_retryWaitExpired = false;
        m_pendingDismissed = true;
        m_manager->clearControllerRecovery();
        m_manager->disconnectDevice();
        m_manualBoot = false;
        refreshDevices(); refresh();
    });
    m_contentLayout->addWidget(m_pendingNotice);
    m_connectionCard = FirmwareUi::card(content);
    m_connectionCard->setObjectName("firmwareConnectCard");
    m_connectionCard->setProperty("uiCard", false);
    auto *connection = qobject_cast<QVBoxLayout *>(m_connectionCard->layout());
    connection->setContentsMargins(0, 0, 0, 0);
    connection->setSpacing(12);
    m_usbTitle = FirmwareUi::label({}, "section", m_connectionCard);
    m_usbTitle->setObjectName("firmwareUsbTitle");
    m_usbHint = FirmwareUi::label({}, "muted", m_connectionCard);
    m_usbHint->setObjectName("firmwareUsbHint");
    connection->addWidget(m_usbTitle); connection->addWidget(m_usbHint);
    connection->addSpacing(8);
    auto *choose = FirmwareUi::label(tr("Controller"), "muted", m_connectionCard);
    connection->addWidget(choose);
    m_devices = new QComboBox(m_connectionCard);
    m_devices->setView(new QListView(m_devices));
    m_devices->setMaxVisibleItems(8);
    m_devices->setObjectName("firmwareControllerList");
    m_devices->setAccessibleName(tr("Controller"));
    m_devices->setMinimumWidth(220); m_devices->setMaximumWidth(540);
    choose->setBuddy(m_devices);
    connection->addWidget(m_devices);
    m_deviceHint = FirmwareUi::label({}, "muted", m_connectionCard);
    m_deviceHint->setObjectName("firmwareControllerHint"); connection->addWidget(m_deviceHint);
    m_connect = new QPushButton(tr("Connect controller"), m_connectionCard);
    m_connect->setObjectName("firmwareConnectAction"); m_connect->setProperty("primary", true);
    connect(m_devices, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        m_connect->setEnabled(m_devices->currentIndex() >= 0 && !m_candidates.isEmpty() && !m_externalBusy);
    });
    connect(m_connect, &QPushButton::clicked, this, [this] {
        const int index = m_devices->currentIndex();
        if (index < 0 || index >= m_candidates.size() || m_externalBusy) return;
        m_manager->connectDevice(m_candidates[index]);
    });
    m_contentLayout->addWidget(m_connectionCard);

    m_deviceCard = FirmwareUi::card(content);
    m_deviceCard->setObjectName("firmwareControllerCard");
    m_deviceCard->setProperty("uiCard", false);
    auto *device = qobject_cast<QVBoxLayout *>(m_deviceCard->layout());
    device->setContentsMargins(0, 0, 0, 0);
    const auto value = [this](const char *name) {
        auto *label = FirmwareUi::label({}, "value", m_deviceCard);
        label->setObjectName(name);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        return label;
    };
    m_board = value("firmwareDetectedBoard");
    m_firmware = value("firmwareDetectedFirmware");
    m_bootloader = value("firmwareDetectedBootloader");
    m_identity = new QGridLayout; m_identity->setSpacing(24);
    m_identity->setColumnStretch(0, 1); m_identity->setColumnStretch(1, 1);
    m_boardIdentity = new QWidget(m_deviceCard);
    auto *boardIdentity = new QVBoxLayout(m_boardIdentity);
    boardIdentity->setContentsMargins(0, 0, 0, 0); boardIdentity->setSpacing(8);
    auto *model = m_modelCaption = FirmwareUi::label(tr("Board model"), "muted", m_boardIdentity);
    boardIdentity->addWidget(model); boardIdentity->addWidget(m_board);
    m_boards = new QComboBox(m_boardIdentity);
    m_boards->setView(new QListView(m_boards)); m_boards->setMaxVisibleItems(7);
    m_boards->setMaximumWidth(360); m_boards->setMinimumWidth(220);
    m_boards->setObjectName("firmwareBoardModel"); m_boards->setAccessibleName(tr("Board model")); model->setBuddy(m_boards);
    m_boards->addItem(tr("Select board…"), QString());
    auto boards = FirmwareCatalog::instance().boards();
    std::sort(boards.begin(), boards.end(), [](const auto &left, const auto &right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    for (const auto &board : boards) m_boards->addItem(board.name, board.id);
    boardIdentity->addWidget(m_boards);
    m_hardwareBoards = new QComboBox(m_boardIdentity);
    m_hardwareBoards->setObjectName("firmwareHardwareModel");
    m_hardwareBoards->setAccessibleName(tr("Hardware model"));
    m_hardwareBoards->setView(new QListView(m_hardwareBoards));
    m_hardwareBoards->setMaxVisibleItems(7);
    m_hardwareBoards->setMaximumWidth(360);
    m_hardwareBoards->addItem(tr("Not identified — select model…"), QString());
    for (const auto &board : boards) m_hardwareBoards->addItem(board.name, board.id);
    m_hardwareHint = FirmwareUi::label({}, "muted", m_boardIdentity);
    m_hardwareHint->setObjectName("firmwareHardwareHint");
    boardIdentity->addWidget(m_hardwareBoards); boardIdentity->addWidget(m_hardwareHint);
    boardIdentity->addStretch();
    m_identity->addWidget(m_boardIdentity, 0, 0);
    m_firmwareIdentity = new QWidget(m_deviceCard);
    auto *firmwareIdentity = new QVBoxLayout(m_firmwareIdentity);
    firmwareIdentity->setContentsMargins(0, 0, 0, 0); firmwareIdentity->setSpacing(8);
    m_currentHeading = FirmwareUi::label(tr("Firmware"), "section", m_firmwareIdentity);
    m_currentHeading->setObjectName("firmwareCurrentHeading");
    firmwareIdentity->addWidget(m_currentHeading); firmwareIdentity->addWidget(m_firmware);
    m_targetCaption = FirmwareUi::label(tr("Firmware target"), "muted", m_firmwareIdentity);
    m_firmwareTarget = value("firmwareDetectedTarget");
    firmwareIdentity->addWidget(m_targetCaption); firmwareIdentity->addWidget(m_firmwareTarget);
    m_bootloaderCaption = FirmwareUi::label(tr("Bootloader"), "muted", m_firmwareIdentity);
    firmwareIdentity->addWidget(m_bootloaderCaption);
    firmwareIdentity->addWidget(m_bootloader); firmwareIdentity->addStretch();
    m_identity->addWidget(m_firmwareIdentity, 0, 1);
    m_installIdentity = new QWidget(m_deviceCard);
    m_installIdentity->setObjectName("firmwareInstallSummary");
    auto *installIdentity = new QVBoxLayout(m_installIdentity);
    installIdentity->setContentsMargins(0, 0, 0, 0); installIdentity->setSpacing(8);
    installIdentity->addWidget(FirmwareUi::label(tr("Will install"), "section", m_installIdentity));
    m_installFirmware = FirmwareUi::label({}, "value", m_installIdentity);
    m_installFirmware->setObjectName("firmwareToInstall"); installIdentity->addWidget(m_installFirmware);
    installIdentity->addWidget(FirmwareUi::label(tr("Bootloader"), "muted", m_installIdentity));
    auto *installBootloader = FirmwareUi::label(tr("DroniNG"), "value", m_installIdentity);
    installBootloader->setObjectName("firmwareBootloaderToInstall"); installIdentity->addWidget(installBootloader);
    installIdentity->addStretch(); m_identity->addWidget(m_installIdentity, 1, 1);
    device->addLayout(m_identity);
    m_status = FirmwareUi::label({}, "muted", m_deviceCard);
    m_status->setObjectName("firmwareStatus"); device->addWidget(m_status);
    m_detectionProgress = new QProgressBar(m_deviceCard);
    m_detectionProgress->setObjectName("firmwareDetectionProgress");
    m_detectionProgress->setAccessibleName(tr("Detecting controller"));
    m_detectionProgress->setRange(0, 0); m_detectionProgress->setTextVisible(false);
    device->addWidget(m_detectionProgress);
    m_updateNotice = new QWidget(m_deviceCard);
    m_updateNotice->setObjectName("firmwareUpdateNotice");
    auto *updateNotice = new QVBoxLayout(m_updateNotice);
    updateNotice->setContentsMargins(0, 0, 0, 0); updateNotice->setSpacing(8);
    updateNotice->addWidget(FirmwareUi::label(tr("Firmware update available"), "section", m_updateNotice));
    m_updateDetails = FirmwareUi::label({}, "muted", m_updateNotice);
    m_updateDetails->setObjectName("firmwareUpdateDetails");
    updateNotice->addWidget(m_updateDetails);
    device->addWidget(m_updateNotice);
    m_setupIntro = FirmwareUi::label(tr("The setup wizard guides you through your aircraft, receiver and motor output settings."), "muted", m_deviceCard);
    m_setupIntro->setObjectName("firmwareSetupIntro");
    device->addWidget(m_setupIntro);

    m_selection = new QWidget(m_deviceCard);
    auto *selection = new QVBoxLayout(m_selection);
    selection->setContentsMargins(0, 8, 0, 0); selection->setSpacing(12);
    m_notes = FirmwareUi::label({}, "muted", m_selection);
    m_notes->setObjectName("firmwareRecommendation"); selection->addWidget(m_notes);
    m_installSettings = FirmwareUi::label(tr("Board hardware settings will be applied automatically."), "muted", m_selection);
    m_installSettings->setObjectName("firmwareSettingsToInstall"); selection->addWidget(m_installSettings);
    auto *replacement = FirmwareUi::label(tr("Remove propellers before installing. Existing firmware and settings will be replaced."), "muted", m_selection);
    replacement->setObjectName("firmwareReplacementNotice"); selection->addWidget(replacement);
    m_install = new QPushButton(tr("Install DroniNG"), m_deviceCard);
    m_install->setObjectName("firmwareInstallAction"); m_install->setProperty("primary", true);
    device->addWidget(m_selection);
    auto *actions = new QGridLayout;
    m_deviceActions = actions;
    m_back = new QToolButton(m_deviceCard);
    m_back->setText(tr("Back to connection"));
    m_back->setObjectName("firmwareBackAction");
    connect(m_back, &QToolButton::clicked, this, [this] {
        m_manualBoot = false; m_boards->setCurrentIndex(0); m_manager->disconnectDevice(); refresh();
    });
    m_configure = new QPushButton(tr("Tune && configure"), m_deviceCard); m_configure->setProperty("primary", true);
    m_configure->setObjectName("firmwareConfigureAction");
    m_scopes = new QPushButton(tr("Live graphs"), m_deviceCard);
    m_scopes->setObjectName("firmwareScopesAction");
    m_disconnect = new QPushButton(tr("Disconnect"), m_deviceCard);
    m_disconnect->setObjectName("firmwareDisconnectAction");
    m_bootController = new QPushButton(tr("Start controller"), m_deviceCard);
    m_bootController->setObjectName("firmwareBootAction");
    connect(m_bootController, &QPushButton::clicked, this, &FirmwarePage::legacyFirmwareBootRequested);
    actions->addWidget(m_back, 0, 0); actions->addWidget(m_install, 0, 1);
    actions->addWidget(m_configure, 1, 0); actions->addWidget(m_scopes, 1, 1);
    actions->addWidget(m_disconnect, 2, 0);
    connect(m_disconnect, &QPushButton::clicked, manager, &ConnectionManager::disconnectDevice);
    connect(m_configure, &QPushButton::clicked, this, [this] {
        if (m_manager->controllerSetupRequired()) m_manager->requestControllerSetup();
        else if (auto *modes = ModeManager::instance()) modes->activateModeByWorkspaceName("Configuration");
    });
    connect(m_scopes, &QPushButton::clicked, this, [] { if (auto *modes = ModeManager::instance()) modes->activateModeByWorkspaceName("Scopes"); });
    m_contentLayout->addWidget(m_deviceCard);

    m_errorCard = FirmwareUi::card(content);
    m_errorCard->setProperty("uiCard", false);
    auto *errorLayout = qobject_cast<QVBoxLayout *>(m_errorCard->layout());
    errorLayout->setContentsMargins(0, 0, 0, 0);
    m_guidance = FirmwareUi::label({}, "muted", m_errorCard);
    m_guidance->setObjectName("connectionErrorGuidance"); m_guidance->setMaximumWidth(720); errorLayout->addWidget(m_guidance);
    m_guidance->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    auto *commandRow = new QHBoxLayout;
    m_command = FirmwareUi::label({}, "muted", m_errorCard);
    m_command->setObjectName("connectionErrorCommand"); m_command->setAccessibleName(tr("Terminal command"));
    m_command->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_copy = new QPushButton(tr("Copy command"), m_errorCard);
    m_copy->setObjectName("copyConnectionErrorCommand");
    commandRow->addWidget(m_command, 1); commandRow->addWidget(m_copy);
    errorLayout->addLayout(commandRow);
    connect(m_copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_command->text()); m_copy->setText(tr("Copied"));
    });
    m_contentLayout->addWidget(m_errorCard);
    m_contentLayout->removeWidget(m_pendingNotice);
    m_contentLayout->addWidget(m_pendingNotice);
    m_logCard = FirmwareUi::card(content);
    m_logCard->setObjectName("firmwareLogCard");
    m_logCard->setAccessibleName(tr("Connection history"));
    auto *log = qobject_cast<QVBoxLayout *>(m_logCard->layout());
    m_toggle = new QToolButton(m_logCard);
    m_toggle->setObjectName("connectionErrorDetailsToggle"); m_toggle->setText(tr("Connection log"));
    m_toggle->setCheckable(true); m_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); m_toggle->setArrowType(Qt::RightArrow);
    log->addWidget(m_toggle);
    m_diagnostics = new QWidget(m_logCard);
    m_diagnostics->setObjectName("firmwareConnectionDiagnostics");
    auto *diagnostics = new QVBoxLayout(m_diagnostics); diagnostics->setContentsMargins(0, 0, 0, 0);
    m_details = new QPlainTextEdit(m_diagnostics);
    m_details->setObjectName("connectionErrorDetails"); m_details->setReadOnly(true);
    m_details->setMinimumHeight(140); m_details->setMaximumHeight(240);
    m_details->setPlaceholderText(tr("No connection events yet."));
    m_details->setAccessibleName(tr("Connection log"));
    connect(manager, &ConnectionManager::connectionLogChanged, this, &FirmwarePage::refresh);
    diagnostics->addWidget(m_details);
    connect(m_toggle, &QToolButton::toggled, this, [this](bool expanded) {
        m_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_diagnostics->setVisible(expanded);
        if (expanded) m_details->verticalScrollBar()->setValue(m_details->verticalScrollBar()->maximum());
    });
    log->addWidget(m_diagnostics);
    m_diagnostics->hide();

    m_recoveryCard = FirmwareUi::card(content);
    m_recoveryCard->setObjectName("firmwareRecoveryCard");
    m_recoveryCard->setAccessibleName(tr("Maintenance and recovery"));
    auto *recovery = qobject_cast<QVBoxLayout *>(m_recoveryCard->layout());
    m_recoveryToggle = new QToolButton(m_recoveryCard);
    m_recoveryToggle->setObjectName("firmwareRecoveryToolsToggle");
    m_recoveryToggle->setText(tr("Maintenance && recovery"));
    m_recoveryToggle->setCheckable(true); m_recoveryToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_recoveryToggle->setArrowType(Qt::RightArrow);
    recovery->addWidget(m_recoveryToggle);
    m_recoveryBody = new QWidget(m_recoveryCard);
    m_recoveryBody->setObjectName("firmwareRecoveryBody");
    auto *maintenance = new QVBoxLayout(m_recoveryBody);
    maintenance->setContentsMargins(0, 0, 0, 0); maintenance->setSpacing(16);
    maintenance->addWidget(FirmwareUi::label(tr("Firmware maintenance"), "heading", m_recoveryBody));
    maintenance->addWidget(FirmwareUi::label(tr("Install DroniNG on a controller or replace its current firmware."), "muted", m_recoveryBody));
    auto *installEntry = new QPushButton(tr("Install DroniNG…"), m_recoveryBody);
    installEntry->setObjectName("firmwareMaintenanceInstallAction");
    maintenance->addWidget(installEntry, 0, Qt::AlignLeft);
    connect(installEntry, &QPushButton::clicked, this, [this] {
        m_manager->disconnectDevice();
        m_manualBoot = true; m_recoveryToggle->setChecked(false);
        m_manager->setFirmwareStatus(tr("Select your board. The installer will guide you into BOOT mode."));
        refresh(); detectBootloader();
        QTimer::singleShot(0, this, [this] { m_scroll->verticalScrollBar()->setValue(0); });
    });
    recovery->addWidget(m_recoveryBody);
    m_recoveryBody->hide();
    dashboard->addWidget(m_recoveryCard, 2, 0, Qt::AlignTop);
    dashboard->addWidget(m_logCard, 2, 1, Qt::AlignTop);
    m_contentLayout->addStretch(1);
    m_contentLayout->addLayout(actions);
    connect(m_recoveryToggle, &QToolButton::toggled, this, [this](bool expanded) {
        m_recoveryBody->setVisible(expanded);
        m_recoveryToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        updateDashboardLayout();
        if (expanded) QTimer::singleShot(0, this, [this] {
            const int top = m_recoveryCard->mapTo(m_scroll->widget(), QPoint()).y();
            m_scroll->verticalScrollBar()->setValue(top - 24);
        });
    });
    scroll->viewport()->installEventFilter(this);
    updateDashboardLayout();
    connect(m_boards, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
    connect(m_hardwareBoards, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_externalBusy) m_manager->confirmHardwareBoard(m_hardwareBoards->currentData().toString());
    });
    connect(manager, &ConnectionManager::firmwareStateChanged, this, [this] { refresh(); });
    connect(manager, &ConnectionManager::firmwarePageRequested, this, [this] { refresh(); });
    connect(manager, &ConnectionManager::availableDevicesChanged, this, [this] {
        refreshDevices(); retryRecoveryConnection(); refresh();
    });
    connect(manager, &ConnectionManager::deviceConnected, this, [this] {
        m_manualBoot = false; m_romPath.clear(); setLegacyBootloaderState(false); refresh();
    });
    connect(manager, &ConnectionManager::deviceDisconnected, this, [this] {
        setLegacyFirmwareUpdate(); refreshDevices(); refresh();
    });
    connect(m_install, &QPushButton::clicked, this, [this] {
        const auto hardware = FirmwareCatalog::instance().board(m_manager->hardwareBoardId());
        const bool mismatch = hardware.isValid() && hardware.target != m_manager->firmwareBoardTarget();
        if (!mismatch && !m_legacyAvailableRevision.isEmpty() && (m_manager->isConnected() || m_legacyBootloader) && !m_externalBusy) {
            emit legacyFirmwareUpdateRequested();
            return;
        }
        const auto board = FirmwareCatalog::instance().board(m_boards->currentData().toString());
        startInstallation(board);
    });
    m_probe->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_probe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this](int, QProcess::ExitStatus) {
        const auto devices = DfuInstaller::devices(QString::fromLocal8Bit(m_probe->readAll()));
        if (m_installation || m_manager->isConnected() || m_manager->firmwareDetectionActive() || m_externalBusy) return;
        const bool initialScan = !m_bootloaderScanned; m_bootloaderScanned = true;
        const QString path = devices.isEmpty() ? QString() : devices.first().path;
        if (path == m_romPath) return;
        m_romPath = path;
        if (!path.isEmpty()) {
            m_manager->setNativeFirmwareIdentity({}, {}, tr("STM32 system bootloader"));
            m_manager->setFirmwareStatus(devices.size() == 1
                ? tr("Controller is in BOOT mode. Select its board model to install DroniNG.")
                : tr("Several controllers are in BOOT mode. Connect only the controller you want to install."));
            if (!initialScan) m_manager->showFirmwarePage();
        } else if (m_manager->detectedBootloader() == tr("STM32 system bootloader")) {
            m_manager->setFirmwareIdentity({}, {});
            if (m_manager->controllerRecovery().problem.message.isEmpty())
                m_manager->setFirmwareStatus(tr("Connect a controller to detect its board and firmware."));
        }
    });
    auto *poll = new QTimer(this); poll->setInterval(2000);
    connect(poll, &QTimer::timeout, this, &FirmwarePage::detectBootloader); poll->start();
    refreshDevices(); refresh();
}

void FirmwarePage::setLegacyFirmwareUpdate(const QString &installedRevision, const QString &availableRevision)
{
    m_legacyInstalledRevision = installedRevision;
    m_legacyAvailableRevision = availableRevision;
    refresh();
}

void FirmwarePage::setLegacyBootloaderState(bool connected, bool canBoot)
{
    m_legacyBootloader = connected;
    m_legacyBootable = connected && canBoot;
    if (connected) m_externalBusy = false;
    refresh();
}

void FirmwarePage::addOperationWidget(QWidget *widget)
{
    widget->setParent(m_overview);
    m_contentLayout->insertWidget(m_contentLayout->indexOf(m_deviceCard), widget);
    m_operationWidgets.append(widget);
    widget->installEventFilter(this);
}

void FirmwarePage::startInstallation(const FirmwareBoardProfile &board)
{
    if (m_installation || m_externalBusy || !board.isValid()) return;
    const auto path = FirmwareCatalog::imagePath(board, FirmwareCatalog::ImageKind::FullInstall);
    if (!FirmwareInstallImage::load(board, path).valid() || DfuInstaller::bundledHelper().isEmpty()) return;
    m_pendingDismissed = false;
    m_probe->kill();
    if (m_manager->flightDataAvailable() && (!board.layoutId.isEmpty()
        || board.target != m_manager->firmwareBoardTarget())) m_manager->disconnectDevice();
    m_installation = new FirmwareInstallWidget(m_manager, board, path, m_overview, m_installRunner, true);
    m_contentLayout->insertWidget(m_contentLayout->indexOf(m_deviceCard), m_installation, 1);
    connect(m_installation, &FirmwareInstallWidget::workflowChanged, this, [this, board] {
        if (m_installation->complete()) {
            m_manualBoot = false;
            m_manager->confirmHardwareBoard(board.id);
            m_manager->endFirmwareInstallation();
            // Finish the signal delivery before destroying the backend that emitted it.
            const QPointer<FirmwareInstallWidget> finished = m_installation;
            QTimer::singleShot(0, this, [this, finished] {
                if (!finished || m_installation != finished) return;
                m_contentLayout->removeWidget(finished);
                m_installation.clear();
                delete finished.data();
                refresh();
            });
        }
        refresh();
    });
    connect(m_installation, &FirmwareInstallWidget::closed, this, [this] {
        auto *installation = m_installation.data(); m_installation.clear();
        delete installation; refresh();
    });
    refresh();
    QTimer::singleShot(0, this, [this] { m_scroll->verticalScrollBar()->setValue(0); });
}

DevListItem FirmwarePage::recoveryDevice() const
{
    const auto context = m_manager->controllerRecovery();
    const auto pending = m_manager->verifiedInstallation();
    const QString path = !context.problem.message.isEmpty() ? context.usbPath : pending.usbPath;
    for (const auto &device : m_candidates) {
        if (!device.device) continue;
        const auto candidate = FirmwareCatalog::instance().board(device.device->getDisplayName().section(" - ", 0, 0));
        if (!context.boardId.isEmpty() && candidate.isValid() && candidate.id != context.boardId) continue;
        if (!path.isEmpty() ? device.device->usbTopologyPath() == path
            : !context.deviceName.isEmpty() && device.device->getDisplayName() == context.deviceName)
            return device;
    }
    return {};
}

void FirmwarePage::retryRecoveryConnection()
{
    if (!m_retryWait->isActive() || m_externalBusy) return;
    const auto device = recoveryDevice();
    if (!device.device) return;
    m_retryWait->stop();
    m_manager->disconnectDevice();
    m_manager->connectDevice(device);
}

void FirmwarePage::setFlightPanel(int index, QWidget *panel)
{
    if (index < 0 || index >= m_flightLayouts.size() || !panel) return;
    panel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_flightPanels[index] = panel;
    m_flightLayouts[index]->insertWidget(0, panel);
    m_flightLayouts[index]->setCurrentWidget(m_flightOverlays[index]);
    refresh();
}

bool FirmwarePage::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_scroll->viewport() && event->type() == QEvent::Resize)
        updateDashboardLayout();
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide)
        for (const auto &widget : m_operationWidgets)
            if (widget == object) { QTimer::singleShot(0, this, [this] { refresh(); }); break; }
    return QWidget::eventFilter(object, event);
}

void FirmwarePage::updateDashboardLayout()
{
    const int contentWidth = qMax(0, qMin(1280, m_scroll->viewport()->width() - 48));
    m_content->setFixedWidth(contentWidth);
    const bool narrow = contentWidth < 720;
    m_dashboard->removeWidget(m_overview);
    for (auto *card : m_flightCards) m_dashboard->removeWidget(card);
    m_dashboard->removeWidget(m_recoveryCard); m_dashboard->removeWidget(m_logCard);
    for (int row = 0; row < 6; ++row) m_dashboard->setRowStretch(row, row < (narrow ? 4 : 2) ? 1 : 0);
    m_dashboard->setColumnStretch(1, narrow ? 0 : 1);
    m_dashboard->addWidget(m_overview, 0, 0);
    for (int index = 0; index < m_flightCards.size(); ++index)
        m_dashboard->addWidget(m_flightCards[index], narrow ? index + 1 : index == 0 ? 0 : 1,
                              narrow ? 0 : index == 1 ? 0 : 1);
    const bool expandedRecovery = m_recoveryToggle->isChecked();
    m_dashboard->addWidget(m_recoveryCard, narrow ? 4 : 2, 0, 1, !narrow && expandedRecovery ? 2 : 1, Qt::AlignTop);
    m_dashboard->addWidget(m_logCard, narrow ? 5 : expandedRecovery ? 3 : 2,
                          narrow || expandedRecovery ? 0 : 1, 1, !narrow && expandedRecovery ? 2 : 1, Qt::AlignTop);
    updateControllerLayout();
}

void FirmwarePage::updateControllerLayout()
{
    const int columns = m_content->width() < 720 ? 1 : 2;
    const bool compact = (m_content->width() - (columns - 1) * 16) / columns < 540;
    const bool choosingBoard = !m_boards->isHidden() || !m_hardwareBoards->isHidden();
    m_identity->removeWidget(m_boardIdentity); m_identity->removeWidget(m_firmwareIdentity); m_identity->removeWidget(m_installIdentity);
    m_identity->addWidget(m_boardIdentity, 0, 0, 1, choosingBoard && !compact ? 2 : 1);
    m_identity->addWidget(m_firmwareIdentity, compact || choosingBoard ? 1 : 0, compact || choosingBoard ? 0 : 1);
    m_identity->addWidget(m_installIdentity, compact ? 2 : 1, compact ? 0 : 1);
    const QList<QWidget *> buttons{m_connect, m_back, m_install, m_bootController, m_configure, m_scopes,
        m_retryConnection, m_recoverInstallation, m_otherController, m_disconnect};
    for (auto *button : buttons) m_deviceActions->removeWidget(button);
    int widest = 1, count = 0;
    for (auto *button : buttons) {
        if (button->isHidden()) continue;
        widest = qMax(widest, button->sizeHint().width());
        ++count;
    }
    const int spacing = m_deviceActions->horizontalSpacing();
    const int available = (m_content->width() - (columns - 1) * 16) / columns - 40;
    const int actionColumns = qMax(1, qMin(count, (available + spacing) / (widest + spacing)));
    int position = 0;
    for (auto *button : buttons) {
        if (button->isHidden()) continue;
        m_deviceActions->addWidget(button, position / actionColumns, position % actionColumns, Qt::AlignLeft);
        ++position;
    }
}

void FirmwarePage::setRecoveryTools(QWidget *tools)
{
    auto *layout = qobject_cast<QVBoxLayout *>(m_recoveryBody->layout());
    layout->addWidget(FirmwareUi::label(tr("DroniNG bootloader recovery"), "heading", m_recoveryBody));
    layout->addWidget(FirmwareUi::label(tr("Recover firmware on a controller with the dRonin bootloader, or work with individual flash partitions."), "muted", m_recoveryBody));
    tools->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(tools); m_recoveryToggle->show();
}

void FirmwarePage::refreshDevices()
{
    const int selectedIndex = m_devices->currentIndex();
    const DevListItem selected = selectedIndex >= 0 && selectedIndex < m_candidates.size()
        ? m_candidates[selectedIndex] : DevListItem();
    QSignalBlocker blocker(m_devices); m_candidates.clear(); m_devices->clear();
    for (const auto &item : m_manager->getAvailableDevices()) {
        if (!item.device || !item.connection || !item.device->isControllerCandidate()) continue;
        // Prefer native HID when a controller exposes both HID and a serial interface.
        bool duplicate = false;
        if (item.connection->shortName() == "Serial" && !item.device->usbTopologyPath().isEmpty()) {
            for (const auto &other : m_manager->getAvailableDevices())
                if (other.device && other.connection && other.connection->shortName() == "USB"
                    && other.device->usbTopologyPath() == item.device->usbTopologyPath()) duplicate = true;
        }
        if (duplicate) continue;
        m_candidates.append(item); m_devices->addItem(item.device->getDisplayName());
    }
    const int preserved = m_candidates.indexOf(selected);
    m_devices->setCurrentIndex(preserved >= 0 ? preserved : m_candidates.size() == 1 ? 0 : -1);
    m_devices->setPlaceholderText(m_candidates.isEmpty()
        ? m_manager->verifiedInstallation().boardId.isEmpty() ? tr("Waiting for a controller…") : tr("No USB controller detected")
        : tr("Select your controller…"));
    m_devices->setEnabled(!m_candidates.isEmpty() && !m_externalBusy);
    m_connect->setEnabled(m_devices->currentIndex() >= 0 && !m_externalBusy);
    m_usbTitle->setText(m_candidates.isEmpty() ? tr("Plug in your controller") : tr("USB controller detected"));
    m_usbHint->setText(m_candidates.isEmpty()
        ? tr("Use a USB data cable to connect your controller.")
        : tr("Choose your controller below, then connect."));
    m_deviceHint->setText(m_candidates.isEmpty()
        ? tr("No controllers detected. Check that your USB cable supports data, then try another USB port.")
        : m_candidates.size() == 1 ? tr("Ready to connect.")
        : tr("Select the controller you want to connect. This list updates automatically."));
    m_deviceHint->hide();
}

void FirmwarePage::setOperationActive(bool active) { m_externalBusy = active; refreshDevices(); refresh(); }

void FirmwarePage::detectBootloader()
{
    if (m_installation || m_externalBusy || m_legacyBootloader || m_manager->isConnected() || m_manager->firmwareDetectionActive()
        || m_probe->state() != QProcess::NotRunning) return;
    const auto helper = DfuInstaller::bundledHelper();
    if (helper.isEmpty()) return;
    m_probe->start(helper, {"-d", "0483:df11", "-l"});
    QTimer::singleShot(1500, m_probe, [this] { if (m_probe->state() != QProcess::NotRunning) m_probe->kill(); });
}

void FirmwarePage::refresh()
{
    const auto &catalog = FirmwareCatalog::instance();
    const bool installing = m_installation && !m_installation->complete();
    bool maintenance = false;
    for (const auto &widget : m_operationWidgets)
        if (widget && !widget->isHidden()) maintenance = true;
    if (m_installation) m_installation->setVisible(installing);
    for (int index = 0; index < m_contentLayout->count(); ++index)
        if (m_contentLayout->itemAt(index)->spacerItem()) m_contentLayout->setStretch(index, installing ? 0 : 1);
    const bool detecting = m_manager->firmwareDetectionActive();
    const bool ready = m_manager->flightDataAvailable() && (!m_installation || m_installation->complete());
    if (ready || installing || m_manager->isConnected()) { m_retryWait->stop(); m_retryWaitExpired = false; }
    const QString target = m_manager->firmwareBoardTarget();
    const auto device = m_manager->getCurrentDevice();
    const QString product = device.device ? device.device->getDisplayName() : QString();
    const bool originalRadix = product == "BrainFPV RADIX" || product.startsWith("BrainFPV RADIX - ");
    const bool mspTelemetryPort = m_manager->hasMspFirmware() && m_manager->firmwareVariant() == "DRON";
    const bool unsupported = m_manager->hasMspFirmware() && !mspTelemetryPort && !target.isEmpty()
        && catalog.candidatesForMspTarget(target).isEmpty() && !m_manualBoot && m_romPath.isEmpty();
    const QString detectedBoard = target == "RADIX" || (target == "RADI" && originalRadix)
        ? tr("BrainFPV RADIX") : m_manager->detectedBoard();
    const auto hardware = catalog.board(m_manager->hardwareBoardId());
    const bool nativeIdentity = !m_manager->hasMspFirmware() && !target.isEmpty();
    const bool hardwareMismatch = nativeIdentity && hardware.isValid() && hardware.target != target;
    {
        QSignalBlocker blocker(m_hardwareBoards);
        m_hardwareBoards->setCurrentIndex(qMax(0, m_hardwareBoards->findData(hardware.id)));
    }
    const bool livePanels = ready && !m_manager->detectedFirmware().isEmpty()
        && !m_manager->firmwareInstallationActive();
    for (int index = 0; index < m_flightCards.size(); ++index) {
        const bool available = livePanels && m_flightPanels[index];
        m_flightCards[index]->setEnabled(available);
        m_flightLayouts[index]->setCurrentWidget(available ? m_flightPanels[index] : m_flightOverlays[index]);
        m_flightOverlays[index]->setVisible(!available);
        m_flightHints[index]->setText(unsupported ? tr("Live data is unavailable for this firmware.")
            : m_manager->hasMspFirmware() && !ready
            ? tr("Install DroniNG to view live data.") : tr("Connect a controller to view live data."));
    }
    const bool needsSetup = ready && m_manager->controllerSetupRequired();
    const auto pending = m_manager->verifiedInstallation();
    const auto recovery = m_manager->controllerRecovery();
    const bool hasProblem = !recovery.problem.message.isEmpty();
    const bool pendingStartup = !m_pendingDismissed && !pending.boardId.isEmpty() && !installing
        && (recovery.deviceName.isEmpty() || (recovery.usbPath == pending.usbPath
            && (recovery.boardId.isEmpty() || recovery.boardId == pending.boardId)))
        && !m_manager->isConnected() && !detecting && !m_manualBoot;
    const bool recoveryNeeded = (hasProblem || pendingStartup) && !ready && !detecting && !m_manualBoot && !installing;
    const auto recoveryBoard = catalog.board(hasProblem ? recovery.boardId : pending.boardId);
    m_pendingNotice->setVisible(recoveryNeeded);
    m_pendingMessage->setVisible(pendingStartup && !hasProblem);
    m_pendingMessage->setText(tr("%1 firmware was written and verified, but the controller has not sent live data. Disconnect USB and the flight battery, then reconnect USB to the same port. Retry the connection; if startup still fails, reinstall the firmware.")
        .arg(catalog.board(pending.boardId).name));
    const bool identified = !m_manager->detectedFirmware().isEmpty() || !m_manager->detectedBootloader().isEmpty();
    const bool journey = recoveryNeeded || installing || maintenance || m_legacyBootloader || m_manualBoot || m_manager->isConnected() || detecting || identified || m_externalBusy;
    const bool nativeLoader = m_legacyBootloader || (!m_manager->detectedBootloader().isEmpty() && !m_manager->hasMspFirmware()
        && m_manager->detectedFirmware().isEmpty()
        && !m_manager->detectedBootloader().contains("STM32", Qt::CaseInsensitive));
    if (detecting && !m_detecting) { QSignalBlocker blocker(m_boards); m_boards->setCurrentIndex(0); }
    m_detecting = detecting;
    if (!installing && target != m_target) {
        m_target = target;
        const auto candidates = catalog.candidatesForMspTarget(target);
        QSignalBlocker blocker(m_boards);
        // Four-byte RADI also truncates other Radix models. Only the original USB product disambiguates it.
        m_boards->setCurrentIndex(candidates.size() == 1 && (target != "RADI" || originalRadix)
            ? m_boards->findData(candidates.first().id) : 0);
    }
    if ((ready || m_legacyBootloader) && !installing) {
        // The installation profile follows a selected hardware model when known.
        // Firmware identity alone describes its target, not the physical board.
        const auto native = hardware.isValid() ? hardware : catalog.board(m_manager->detectedBoard());
        if (native.isValid()) {
            QSignalBlocker blocker(m_boards); m_boards->setCurrentIndex(m_boards->findData(native.id));
        }
    }
    if (recoveryNeeded && recoveryBoard.isValid()) {
        QSignalBlocker blocker(m_boards); m_boards->setCurrentIndex(m_boards->findData(recoveryBoard.id));
    }
    const auto board = catalog.board(m_boards->currentData().toString());
    const auto profileId = board.id;
    if (!m_imageChecked || profileId != m_profileId) {
        m_profileId = profileId; m_imageChecked = true;
        const auto image = board.isValid() ? FirmwareInstallImage::load(board,
            FirmwareCatalog::imagePath(board, FirmwareCatalog::ImageKind::FullInstall)) : FirmwareInstallImage();
        m_imageAvailable = image.valid(); m_imageError = image.error;
        m_imageDescriptor = image.descriptor;
    }
    const bool importedController = ready && !board.layoutId.isEmpty() && !hardwareMismatch;
    const bool importedUpdate = importedController && m_imageAvailable
        && m_manager->firmwareDescription().size() == 100
        && m_manager->firmwareDescription() != m_imageDescriptor;
    const bool legacyUpdate = (ready || m_legacyBootloader) && !m_legacyAvailableRevision.isEmpty() && !hardwareMismatch;
    const bool updateOffered = (importedUpdate || legacyUpdate) && !installing && !recoveryNeeded && !m_externalBusy;
    m_updateNotice->setVisible(updateOffered);
    m_updateDetails->setText(legacyUpdate
        ? m_legacyBootloader && !m_legacyBootable
            ? tr("No valid firmware is installed. Install the compatible firmware included in this build (%1).")
                .arg(m_legacyAvailableRevision)
            : tr("The installed firmware (%1) differs from the compatible firmware in this build (%2). You can update it below.")
            .arg(m_legacyInstalledRevision, m_legacyAvailableRevision)
        : tr("A different firmware image is included in this build. Updating preserves controller settings and autotune storage."));
    const bool installOffered = m_manualBoot || !m_romPath.isEmpty() || (m_manager->hasMspFirmware() && !mspTelemetryPort);
    const bool choosingBoard = installOffered && !recoveryNeeded && !unsupported && !installing && !detecting && !ready && !nativeLoader && !m_externalBusy;
    const bool choosingHardware = nativeIdentity && !choosingBoard && !recoveryNeeded && !installing && !detecting
        && m_hardwareBoards->count() > 1;
    const bool repairOffered = hardwareMismatch && !installing && !recoveryNeeded && !m_externalBusy;
    m_title->setText(recoveryNeeded ? hasProblem ? tr("Controller needs attention") : tr("Finish controller startup") : !journey
        ? m_manager->firmwareError().message.isEmpty() ? tr("Connect your controller") : m_manager->firmwareStatus()
        : installing ? m_installation->title() : maintenance ? tr("Firmware maintenance")
        : hardwareMismatch ? tr("Install compatible firmware")
        : m_legacyBootloader ? tr("Controller in bootloader mode")
        : needsSetup ? tr("Set up controller?") : ready ? tr("Your controller is ready")
        : unsupported ? tr("Firmware unavailable") : installOffered ? tr("Install DroniNG")
        : detecting ? tr("Connecting to your controller") : tr("Your controller"));
    m_connectionCard->setVisible(!journey);
    m_connect->setVisible(!journey);
    m_deviceCard->setVisible(journey && !installing && !maintenance);
    m_board->setText(recoveryNeeded ? recoveryBoard.isValid() ? recoveryBoard.name : recovery.deviceName
        : nativeIdentity ? hardware.isValid() ? hardware.name : tr("Not identified")
        : board.isValid() ? board.name : !detectedBoard.isEmpty() ? tr("Not identified")
        : !recovery.deviceName.isEmpty() && detecting ? recovery.deviceName : detecting ? tr("Detecting…") : tr("Not detected"));
    m_modelCaption->setText(recoveryNeeded ? tr("Recovery profile")
        : choosingBoard ? tr("Board to install on") : tr("Board model"));
    m_modelCaption->setBuddy(choosingHardware ? m_hardwareBoards : m_boards);
    const auto firmwareTarget = catalog.board(target);
    m_firmwareTarget->setText(firmwareTarget.isValid()
        ? firmwareTarget.aliases.value(0, firmwareTarget.name) : target);
    m_targetCaption->setVisible(!target.isEmpty()); m_firmwareTarget->setVisible(!target.isEmpty());
    m_hardwareBoards->setVisible(choosingHardware);
    m_hardwareBoards->setEnabled(!m_externalBusy);
    m_hardwareHint->setVisible(nativeIdentity && !recoveryNeeded && !detecting);
    m_hardwareHint->setText(hardwareMismatch
        ? m_imageAvailable
            ? tr("The installed firmware targets %1. Install compatible firmware for %2 to continue.")
                .arg(m_firmwareTarget->text(), hardware.name)
            : tr("The installed firmware targets %1. Firmware for %2 is not included in this build.")
                .arg(m_firmwareTarget->text(), hardware.name)
        : !hardware.isValid()
        ? tr("The firmware does not report the physical board model. Select the model printed on your board.")
        : m_manager->controllerSerial().isEmpty()
            ? tr("Model selected for this connection. A controller serial is needed to remember it.")
            : tr("Model saved for this controller."));
    m_firmware->setText(m_manager->detectedFirmware().isEmpty()
        ? detecting ? tr("Detecting…") : !m_romPath.isEmpty() ? tr("Not read in BOOT mode")
            : choosingBoard ? tr("Not connected") : tr("Not detected") : m_manager->detectedFirmware());
    m_bootloader->setText(!m_manager->detectedBootloader().isEmpty() ? m_manager->detectedBootloader()
        : detecting ? tr("Detecting…") : (m_manager->hasMspFirmware() || m_manualBoot) && board.isValid() && !board.systemBootloader.isEmpty()
            ? board.systemBootloader : m_manager->hasMspFirmware() ? tr("Not reported") : tr("Not detected"));
    m_status->setText(m_manualBoot && board.isValid() ? !m_romPath.isEmpty()
        ? tr("Controller is in BOOT mode. Install DroniNG to continue.")
        : tr("Install the recommended firmware to continue.")
        : m_manager->firmwareStatus().isEmpty() ? tr("Connect a controller to detect its board and firmware.") : m_manager->firmwareStatus());
    if (unsupported) m_status->setText(tr("No compatible DroniNG firmware is available for %1 in this build. "
        "You can keep using its current firmware and configure it with its own configurator.")
        .arg(detectedBoard.isEmpty() ? target : detectedBoard));
    if (importedController) m_status->setText(board.notes);
    m_status->setVisible(!recoveryNeeded && (importedController || unsupported || (!installOffered && !ready && m_manager->firmwareError().message.isEmpty())));
    if (detecting) m_status->setText(tr("Checking firmware and live data…"));
    m_currentHeading->setText(choosingBoard ? tr("Currently installed") : tr("Firmware"));
    m_firmwareIdentity->setVisible(choosingBoard || detecting || nativeLoader || !m_manager->detectedFirmware().isEmpty());
    const bool showBootloader = !detecting && (!m_manager->detectedBootloader().isEmpty()
        || (m_manager->hasMspFirmware() && board.isValid() && !board.systemBootloader.isEmpty()));
    m_bootloaderCaption->setVisible(showBootloader); m_bootloader->setVisible(showBootloader);
    m_installIdentity->setVisible(choosingBoard && board.isValid());
    const auto targetBoard = catalog.board(board.target);
    const QString targetName = targetBoard.aliases.value(0, targetBoard.isValid() ? targetBoard.name : board.target);
    m_installFirmware->setText(tr("%1 · %2").arg(QStringLiteral(GCS_PRODUCT_LABEL), targetName));
    findChild<QLabel *>("firmwareBootloaderToInstall")->setText(board.layoutId.isEmpty() ? tr("DroniNG") : tr("STM32 ROM · retained"));
    findChild<QLabel *>("firmwareReplacementNotice")->setText(board.layoutId.isEmpty()
        ? tr("Remove propellers before installing. Existing firmware and settings will be replaced.")
        : tr("Remove propellers before installing. Updates preserve DroniNG settings and autotune storage. First installation needs a new controller setup."));
    m_installSettings->setVisible(board.isValid() && !board.settings.isEmpty());
    m_detectionProgress->setVisible(detecting && !installing);
    m_disconnect->setVisible(m_manager->isConnected() && !installing && !recoveryNeeded && !maintenance);
    m_back->setVisible(m_manualBoot && !m_manager->isConnected() && m_romPath.isEmpty() && !installing);
    m_back->setText(hasProblem || pendingStartup ? tr("Back to recovery") : tr("Back to connection"));
    m_disconnect->setEnabled(!m_externalBusy);
    m_configure->setVisible(ready && !installing && !maintenance && !hardwareMismatch);
    m_bootController->setVisible(m_legacyBootloader && m_legacyBootable && !installing && !maintenance && !hardwareMismatch);
    m_bootController->setEnabled(!m_externalBusy);
    if (m_bootController->property("primary").toBool() != !updateOffered) {
        m_bootController->setProperty("primary", !updateOffered);
        m_bootController->style()->unpolish(m_bootController); m_bootController->style()->polish(m_bootController);
    }
    m_configure->setText(needsSetup ? tr("Launch setup wizard") : tr("Tune && configure"));
    m_setupIntro->setVisible(needsSetup && !installing);
    bool hasScopes = false;
    if (auto *modes = ModeManager::instance())
        for (auto *mode : modes->modes()) if (mode->name() == "Scopes") hasScopes = true;
    m_scopes->setVisible(ready && !installing && !maintenance && hasScopes);
    const auto error = m_manager->firmwareError();
    // Foreign firmware is a supported installation path; avoid duplicating that explanation as an error.
    const bool explainError = !installing && (!m_manager->hasMspFirmware() || mspTelemetryPort || !error.command.isEmpty());
    const auto diagnosis = recoveryNeeded && hasProblem ? recovery.problem : error;
    m_errorCard->setVisible(explainError && (!diagnosis.message.isEmpty() || !diagnosis.command.isEmpty()));
    m_guidance->setText(diagnosis.message); m_guidance->setVisible(!diagnosis.message.isEmpty());
    if (m_command->text() != diagnosis.command) m_copy->setText(tr("Copy command"));
    m_command->setText(diagnosis.command); m_command->setVisible(!diagnosis.command.isEmpty()); m_copy->setVisible(!diagnosis.command.isEmpty());
    const auto history = m_manager->connectionLog().isEmpty() && !pending.boardId.isEmpty()
        ? pending.log : m_manager->connectionLog();
    if (m_details->toPlainText() != history) {
        auto *scroll = m_details->verticalScrollBar();
        const int position = scroll->value();
        const bool following = !m_toggle->isChecked() || position == scroll->maximum();
        m_details->setPlainText(history);
        scroll->setValue(following ? scroll->maximum() : position);
    }
    m_diagnostics->setVisible(m_toggle->isChecked());
    m_selection->setVisible(installOffered && !recoveryNeeded && !unsupported && !installing && !detecting && !ready && !nativeLoader && !m_externalBusy);
    m_boards->setVisible(choosingBoard); m_board->setVisible(!choosingBoard && !choosingHardware);
    m_install->setVisible(!maintenance && !installing && !recoveryNeeded && (choosingBoard || updateOffered || repairOffered));
    m_install->setText(repairOffered ? tr("Install compatible firmware…")
        : updateOffered && !(m_legacyBootloader && !m_legacyBootable) ? tr("Update firmware…") : tr("Install DroniNG"));
    const bool installPrimary = !updateOffered || m_legacyBootloader;
    if (m_install->property("primary").toBool() != installPrimary) {
        m_install->setProperty("primary", installPrimary);
        m_install->style()->unpolish(m_install); m_install->style()->polish(m_install);
    }
    m_boards->setEnabled(!m_externalBusy && !detecting);
    m_install->setEnabled(!m_externalBusy && !detecting && (legacyUpdate
        || (board.isValid() && m_imageAvailable && !DfuInstaller::bundledHelper().isEmpty())));
    m_notes->setText(!catalog.error().isEmpty() ? tr("The firmware catalog could not be loaded: %1").arg(catalog.error())
        : !board.isValid() ? tr("Select the model printed on your board. Unlisted boards have no compatible firmware candidate.")
        : !m_imageAvailable ? m_imageError : DfuInstaller::bundledHelper().isEmpty() ? tr("The Linux firmware installer is missing from this build.") : board.notes);
    m_notes->setVisible(!board.layoutId.isEmpty() || !catalog.error().isEmpty()
        || !board.isValid() || !m_imageAvailable || DfuInstaller::bundledHelper().isEmpty());
    m_retryConnection->setVisible(recoveryNeeded);
    m_retryConnection->setText(m_retryWait->isActive() ? tr("Waiting for controller…") : tr("Retry connection"));
    m_retryConnection->setEnabled(!m_externalBusy && !m_retryWait->isActive());
    m_recoverInstallation->setVisible(recoveryNeeded);
    m_recoverInstallation->setText(recoveryBoard.isValid() ? tr("Reinstall firmware…") : tr("Select board…"));
    m_recoverInstallation->setEnabled(!m_externalBusy && (!recoveryBoard.isValid()
        || (m_imageAvailable && !DfuInstaller::bundledHelper().isEmpty())));
    const bool recoveryPrimary = !m_retryConnection->isEnabled() && m_recoverInstallation->isEnabled();
    if (m_recoverInstallation->property("primary").toBool() != recoveryPrimary) {
        m_recoverInstallation->setProperty("primary", recoveryPrimary);
        m_recoverInstallation->style()->unpolish(m_recoverInstallation);
        m_recoverInstallation->style()->polish(m_recoverInstallation);
    }
    m_otherController->setVisible(recoveryNeeded);
    m_otherController->setEnabled(!m_externalBusy);
    m_recoveryHint->setText(!recoveryBoard.isValid()
        ? tr("The board model could not be identified. Select the model printed on your controller to find compatible firmware.")
        : !m_imageAvailable ? tr("Compatible recovery firmware is missing from this build. %1").arg(m_imageError)
        : DfuInstaller::bundledHelper().isEmpty() ? tr("The firmware installer is missing from this build.")
        : m_retryWait->isActive() ? tr("Reconnect USB to the same port. The connection will retry when this controller appears.")
        : m_retryWaitExpired ? tr("No USB connection appeared. Check the USB cable, then retry, or reinstall firmware using BOOT mode.")
        : tr("Retry connection checks this controller on the same USB port. Reinstall firmware guides you through its BOOT procedure."));
    m_recoveryToggle->setEnabled(!installing && !detecting && !m_externalBusy);
    if (installing) m_recoveryToggle->setChecked(false);
    updateControllerLayout();
}
