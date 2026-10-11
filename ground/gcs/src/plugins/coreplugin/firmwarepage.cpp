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
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Core;

FirmwarePage::FirmwarePage(ConnectionManager *manager, QWidget *parent)
    : QWidget(parent), m_manager(manager), m_probe(new QProcess(this))
{
    setObjectName("firmwarePage");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setProperty("pageLayout", true);
    setAutoFillBackground(true);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto *canvas = new QWidget;
    canvas->setObjectName("firmwareCanvas");
    scroll->setWidget(canvas);
    auto *row = new QHBoxLayout(canvas);
    row->setContentsMargins(32, 32, 32, 32);
    row->addStretch();
    auto *content = new QWidget(canvas);
    content->setMaximumWidth(760);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row->addWidget(content, 1, Qt::AlignTop);
    row->addStretch();
    m_contentLayout = new QVBoxLayout(content);
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->setSpacing(24);
    m_title = FirmwareUi::label({}, "title", content);
    m_title->setObjectName("firmwarePageTitle");
    m_subtitle = FirmwareUi::label({}, "muted", content);
    auto *intro = new QVBoxLayout;
    intro->setSpacing(8); intro->addWidget(m_title); intro->addWidget(m_subtitle);
    m_contentLayout->addLayout(intro);
    m_connectionCard = FirmwareUi::card(content);
    m_connectionCard->setObjectName("firmwareConnectCard");
    auto *connection = qobject_cast<QVBoxLayout *>(m_connectionCard->layout());
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
    connection->addWidget(m_connect, 0, Qt::AlignLeft);
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
    auto *device = qobject_cast<QVBoxLayout *>(m_deviceCard->layout());
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
    auto *model = FirmwareUi::label(tr("Board model"), "muted", m_boardIdentity);
    boardIdentity->addWidget(model); boardIdentity->addWidget(m_board);
    m_boards = new QComboBox(m_boardIdentity);
    m_boards->setView(new QListView(m_boards)); m_boards->setMaxVisibleItems(7);
    m_boards->setMaximumWidth(360); m_boards->setMinimumWidth(220);
    m_boards->setObjectName("firmwareBoardModel"); m_boards->setAccessibleName(tr("Board model")); model->setBuddy(m_boards);
    m_boards->addItem(tr("Select board…"), QString());
    for (const auto &board : FirmwareCatalog::instance().boards()) m_boards->addItem(board.name, board.id);
    boardIdentity->addWidget(m_boards); boardIdentity->addStretch();
    m_identity->addWidget(m_boardIdentity, 0, 0);
    m_firmwareIdentity = new QWidget(m_deviceCard);
    auto *firmwareIdentity = new QVBoxLayout(m_firmwareIdentity);
    firmwareIdentity->setContentsMargins(0, 0, 0, 0); firmwareIdentity->setSpacing(8);
    m_currentHeading = FirmwareUi::label(tr("Firmware"), "section", m_firmwareIdentity);
    m_currentHeading->setObjectName("firmwareCurrentHeading");
    firmwareIdentity->addWidget(m_currentHeading); firmwareIdentity->addWidget(m_firmware);
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
    selection->addWidget(FirmwareUi::label(tr("Remove propellers before installing. Existing firmware and settings will be replaced."), "muted", m_selection));
    m_install = new QPushButton(tr("Install DroniNG"), m_deviceCard);
    m_install->setObjectName("firmwareInstallAction"); m_install->setProperty("primary", true);
    device->addWidget(m_selection);
    auto *actions = new QHBoxLayout;
    m_back = new QToolButton(m_deviceCard);
    m_back->setText(tr("Back to connection"));
    m_back->setObjectName("firmwareBackAction"); actions->addWidget(m_back);
    connect(m_back, &QToolButton::clicked, this, [this] {
        m_manualBoot = false; m_boards->setCurrentIndex(0); m_manager->disconnectDevice(); refresh();
    });
    m_flight = new QPushButton(tr("Flight data"), m_deviceCard);
    m_flight->setObjectName("firmwareFlightAction");
    m_configure = new QPushButton(tr("Tune && configure"), m_deviceCard); m_configure->setProperty("primary", true);
    m_configure->setObjectName("firmwareConfigureAction");
    m_scopes = new QPushButton(tr("Live graphs"), m_deviceCard);
    m_scopes->setObjectName("firmwareScopesAction");
    m_disconnect = new QPushButton(tr("Disconnect"), m_deviceCard);
    m_disconnect->setObjectName("firmwareDisconnectAction");
    actions->addWidget(m_install); actions->addWidget(m_configure); actions->addWidget(m_flight); actions->addWidget(m_scopes);
    actions->addStretch(); actions->addWidget(m_disconnect);
    device->addLayout(actions);
    connect(m_disconnect, &QPushButton::clicked, manager, &ConnectionManager::disconnectDevice);
    connect(m_flight, &QPushButton::clicked, this, [] { if (auto *modes = ModeManager::instance()) modes->activateModeByWorkspaceName("Flight data"); });
    connect(m_configure, &QPushButton::clicked, this, [this] {
        if (m_manager->controllerSetupRequired()) m_manager->requestControllerSetup();
        else if (auto *modes = ModeManager::instance()) modes->activateModeByWorkspaceName("Configuration");
    });
    connect(m_scopes, &QPushButton::clicked, this, [] { if (auto *modes = ModeManager::instance()) modes->activateModeByWorkspaceName("Scopes"); });
    m_contentLayout->addWidget(m_deviceCard);

    m_errorCard = FirmwareUi::card(content);
    auto *errorLayout = qobject_cast<QVBoxLayout *>(m_errorCard->layout());
    errorLayout->addWidget(FirmwareUi::label(tr("Connection needs attention"), "heading", m_errorCard));
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
    m_contentLayout->insertWidget(m_contentLayout->indexOf(m_deviceCard), m_errorCard);
    auto *utilities = new QHBoxLayout;
    m_diagnostics = new QWidget(content);
    m_diagnostics->setObjectName("firmwareConnectionDiagnostics");
    auto *diagnostics = new QVBoxLayout(m_diagnostics); diagnostics->setContentsMargins(0, 0, 0, 0);
    m_toggle = new QToolButton(content);
    m_toggle->setObjectName("connectionErrorDetailsToggle"); m_toggle->setText(tr("Connection log"));
    m_toggle->setCheckable(true); m_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); m_toggle->setArrowType(Qt::RightArrow);
    m_details = new QPlainTextEdit(m_diagnostics);
    m_details->setObjectName("connectionErrorDetails"); m_details->setReadOnly(true); m_details->setMaximumHeight(140);
    diagnostics->addWidget(m_details);
    connect(m_toggle, &QToolButton::toggled, this, [this](bool expanded) {
        m_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_details->setVisible(expanded && !m_details->toPlainText().isEmpty());
        m_diagnostics->setVisible(expanded);
    });

    m_recoveryCard = FirmwareUi::card(content);
    m_recoveryCard->setObjectName("firmwareRecoveryCard");
    m_recoveryCard->hide();
    auto *maintenance = qobject_cast<QVBoxLayout *>(m_recoveryCard->layout());
    maintenance->addWidget(FirmwareUi::label(tr("Firmware maintenance"), "heading", m_recoveryCard));
    maintenance->addWidget(FirmwareUi::label(tr("Install DroniNG on a controller or replace its current firmware."), "muted", m_recoveryCard));
    auto *installEntry = new QPushButton(tr("Install DroniNG…"), m_recoveryCard);
    installEntry->setObjectName("firmwareMaintenanceInstallAction");
    maintenance->addWidget(installEntry, 0, Qt::AlignLeft);
    connect(installEntry, &QPushButton::clicked, this, [this] {
        m_manager->disconnectDevice();
        m_manualBoot = true; m_recoveryToggle->setChecked(false);
        m_manager->setFirmwareStatus(tr("Select your board. The installer will guide you into BOOT mode."));
        refresh(); detectBootloader();
    });
    m_recoveryToggle = new QToolButton(content);
    m_recoveryToggle->setObjectName("firmwareRecoveryToolsToggle");
    m_recoveryToggle->setText(tr("Maintenance && recovery"));
    m_recoveryToggle->setCheckable(true); m_recoveryToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_recoveryToggle->setArrowType(Qt::RightArrow);
    utilities->addWidget(m_recoveryToggle); utilities->addStretch(); utilities->addWidget(m_toggle);
    m_contentLayout->addLayout(utilities);
    m_contentLayout->addWidget(m_diagnostics);
    m_contentLayout->addWidget(m_recoveryCard);
    connect(m_recoveryToggle, &QToolButton::toggled, this, [this](bool expanded) {
        m_recoveryCard->setVisible(expanded);
        m_recoveryToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(m_boards, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
    connect(manager, &ConnectionManager::firmwareStateChanged, this, [this] { refresh(); });
    connect(manager, &ConnectionManager::firmwarePageRequested, this, [this] { refresh(); });
    connect(manager, &ConnectionManager::availableDevicesChanged, this, [this] { refreshDevices(); });
    connect(manager, &ConnectionManager::deviceConnected, this, [this] { m_manualBoot = false; m_romPath.clear(); refresh(); });
    connect(manager, &ConnectionManager::deviceDisconnected, this, [this] { refreshDevices(); refresh(); });
    connect(m_install, &QPushButton::clicked, this, [this] {
        if (m_installation || m_externalBusy) return;
        const auto board = FirmwareCatalog::instance().board(m_boards->currentData().toString());
        if (!board.isValid()) return;
        m_probe->kill();
        m_installation = new FirmwareInstallWidget(m_manager, board,
            FirmwareCatalog::imagePath(board, FirmwareCatalog::ImageKind::FullInstall), this);
        m_contentLayout->insertWidget(m_contentLayout->indexOf(m_deviceCard), m_installation);
        connect(m_installation, &FirmwareInstallWidget::workflowChanged, this, [this] { refresh(); });
        connect(m_installation, &FirmwareInstallWidget::closed, this, [this] {
            auto *installation = m_installation.data(); m_installation.clear();
            delete installation; refresh();
        });
        refresh();
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
            m_manager->setFirmwareStatus(tr("Connect a controller to detect its board and firmware."));
        }
    });
    auto *poll = new QTimer(this); poll->setInterval(2000);
    connect(poll, &QTimer::timeout, this, &FirmwarePage::detectBootloader); poll->start();
    refreshDevices(); refresh();
}

void FirmwarePage::setRecoveryTools(QWidget *tools)
{
    auto *layout = qobject_cast<QVBoxLayout *>(m_recoveryCard->layout());
    layout->addWidget(FirmwareUi::label(tr("DroniNG bootloader recovery"), "heading", m_recoveryCard));
    layout->addWidget(FirmwareUi::label(tr("Recover firmware on a controller with the dRonin bootloader, or work with individual flash partitions."), "muted", m_recoveryCard));
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
    m_devices->setPlaceholderText(m_candidates.isEmpty() ? tr("Waiting for a controller…") : tr("Select your controller…"));
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
    if (m_installation || m_externalBusy || m_manager->isConnected() || m_manager->firmwareDetectionActive()
        || m_probe->state() != QProcess::NotRunning) return;
    const auto helper = DfuInstaller::bundledHelper();
    if (helper.isEmpty()) return;
    m_probe->start(helper, {"-d", "0483:df11", "-l"});
    QTimer::singleShot(1500, m_probe, [this] { if (m_probe->state() != QProcess::NotRunning) m_probe->kill(); });
}

void FirmwarePage::refresh()
{
    const auto &catalog = FirmwareCatalog::instance();
    const bool detecting = m_manager->firmwareDetectionActive();
    const bool ready = m_manager->flightDataAvailable() && (!m_installation || m_installation->complete());
    const bool needsSetup = ready && m_manager->controllerSetupRequired();
    const bool identified = !m_manager->detectedFirmware().isEmpty() || !m_manager->detectedBootloader().isEmpty();
    const bool mspTelemetryPort = m_manager->hasMspFirmware() && m_manager->firmwareVariant() == "DRON";
    const bool journey = m_installation || m_manualBoot || m_manager->isConnected() || detecting || identified || m_externalBusy;
    const bool nativeLoader = !m_manager->detectedBootloader().isEmpty() && !m_manager->hasMspFirmware()
        && m_manager->detectedFirmware().isEmpty()
        && !m_manager->detectedBootloader().contains("STM32", Qt::CaseInsensitive);
    if (detecting && !m_detecting) { QSignalBlocker blocker(m_boards); m_boards->setCurrentIndex(0); }
    m_detecting = detecting;
    const QString target = m_manager->firmwareBoardTarget();
    if (!m_installation && target != m_target) {
        m_target = target;
        const auto candidates = catalog.candidatesForMspTarget(target);
        QSignalBlocker blocker(m_boards);
        m_boards->setCurrentIndex(candidates.size() == 1 ? m_boards->findData(candidates.first().id) : 0);
    }
    const auto board = catalog.board(m_boards->currentData().toString());
    const bool installOffered = m_manualBoot || !m_romPath.isEmpty() || (m_manager->hasMspFirmware() && !mspTelemetryPort);
    const bool choosingBoard = installOffered && !m_installation && !detecting && !ready && !nativeLoader && !m_externalBusy;
    m_title->setText(!journey ? tr("Connect your controller") : m_installation ? tr("Install DroniNG")
        : needsSetup ? tr("Set up controller?") : ready ? tr("Your controller is ready") : installOffered ? tr("Install DroniNG")
        : detecting ? tr("Connecting to your controller") : tr("Your controller"));
    m_subtitle->setText(tr("Connect by USB to tune, configure or view flight data."));
    m_subtitle->setVisible(!journey);
    m_connectionCard->setVisible(!journey);
    m_deviceCard->setVisible(journey && !m_installation);
    m_board->setText(board.isValid() ? board.name : m_manager->detectedBoard().isEmpty()
        ? detecting ? tr("Detecting…") : tr("Not detected") : m_manager->detectedBoard());
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
    m_status->setVisible(!installOffered && !ready && m_manager->firmwareError().message.isEmpty());
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
    m_installSettings->setVisible(board.isValid() && !board.settings.isEmpty());
    m_identity->removeWidget(m_boardIdentity); m_identity->removeWidget(m_firmwareIdentity); m_identity->removeWidget(m_installIdentity);
    m_identity->addWidget(m_boardIdentity, 0, 0, 1, choosingBoard ? 2 : 1);
    m_identity->addWidget(m_firmwareIdentity, choosingBoard ? 1 : 0, choosingBoard ? 0 : 1);
    m_identity->addWidget(m_installIdentity, 1, 1);
    m_detectionProgress->setVisible(detecting && !m_installation);
    m_disconnect->setVisible(m_manager->isConnected() && !m_installation);
    m_back->setVisible(m_manualBoot && !m_manager->isConnected() && m_romPath.isEmpty() && !m_installation);
    m_disconnect->setEnabled(!m_externalBusy);
    m_flight->setVisible(ready); m_configure->setVisible(ready);
    m_configure->setText(needsSetup ? tr("Launch setup wizard") : tr("Tune && configure"));
    m_setupIntro->setVisible(needsSetup && !m_installation);
    bool hasScopes = false;
    if (auto *modes = ModeManager::instance())
        for (auto *mode : modes->modes()) if (mode->name() == "Scopes") hasScopes = true;
    m_scopes->setVisible(ready && hasScopes);
    const auto error = m_manager->firmwareError();
    // Foreign firmware is a supported installation path; avoid duplicating that explanation as an error.
    const bool explainError = !m_installation && (!m_manager->hasMspFirmware() || mspTelemetryPort || !error.command.isEmpty());
    m_errorCard->setVisible(explainError && (!error.message.isEmpty() || !error.command.isEmpty()));
    m_guidance->setText(error.message); m_guidance->setVisible(!error.message.isEmpty());
    if (m_command->text() != error.command) m_copy->setText(tr("Copy command"));
    m_command->setText(error.command); m_command->setVisible(!error.command.isEmpty()); m_copy->setVisible(!error.command.isEmpty());
    m_details->setPlainText(error.details);
    m_toggle->setVisible(!m_installation && (identified || !error.details.isEmpty()));
    m_diagnostics->setVisible(!m_installation && (identified || !error.details.isEmpty()) && m_toggle->isChecked());
    m_details->setVisible(m_toggle->isChecked() && !error.details.isEmpty());
    m_selection->setVisible(installOffered && !m_installation && !detecting && !ready && !nativeLoader && !m_externalBusy);
    m_boards->setVisible(choosingBoard); m_board->setVisible(!choosingBoard);
    m_install->setVisible(choosingBoard);
    m_boards->setEnabled(!m_externalBusy && !detecting);
    const auto profileId = m_boards->currentData().toString();
    if (!m_imageChecked || profileId != m_profileId) {
        m_profileId = profileId; m_imageChecked = true;
        const auto image = board.isValid() ? FirmwareInstallImage::load(board,
            FirmwareCatalog::imagePath(board, FirmwareCatalog::ImageKind::FullInstall)) : FirmwareInstallImage();
        m_imageAvailable = image.valid(); m_imageError = image.error;
    }
    m_install->setEnabled(board.isValid() && m_imageAvailable && !DfuInstaller::bundledHelper().isEmpty() && !m_externalBusy && !detecting);
    m_notes->setText(!catalog.error().isEmpty() ? tr("The firmware catalog could not be loaded: %1").arg(catalog.error())
        : !board.isValid() ? tr("Select the model printed on your board. Unlisted boards have no compatible firmware candidate.")
        : !m_imageAvailable ? m_imageError : DfuInstaller::bundledHelper().isEmpty() ? tr("The Linux firmware installer is missing from this build.") : board.notes);
    m_notes->setVisible(!catalog.error().isEmpty() || !board.isValid() || !m_imageAvailable || DfuInstaller::bundledHelper().isEmpty());
    m_recoveryToggle->setEnabled(!m_installation && !detecting && !m_externalBusy);
    m_recoveryToggle->setVisible(!m_installation && !m_manualBoot);
    if (m_installation) m_recoveryToggle->setChecked(false);
}
