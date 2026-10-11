/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firmwareinstallwidget.h"
#include "connectionmanager.h"
#include "dfuinstaller.h"
#include "firmwareuicomponents.h"
#include "modemanager.h"
#include "iconnection.h"
#include "idevice.h"
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Core;

FirmwareInstallWidget::FirmwareInstallWidget(ConnectionManager *manager,
        const FirmwareBoardProfile &board, const QString &imagePath, QWidget *parent, DfuCommandRunner *runner)
    : QWidget(parent), m_manager(manager), m_installer(new DfuInstaller(this, runner))
{
    setObjectName("firmwareInstallWidget");
    setProperty("pageLayout", true);
    setAutoFillBackground(true);
    auto *outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
    auto *card = FirmwareUi::card(this); outer->addWidget(card);
    card->setObjectName("firmwareInstallTaskCard");
    auto *layout = qobject_cast<QVBoxLayout *>(card->layout());
    auto *heading = FirmwareUi::label(tr("Restart in BOOT mode"), "heading", card);
    heading->setObjectName("firmwareInstallHeading"); layout->addWidget(heading);
    layout->addWidget(FirmwareUi::label(board.name, "muted", card));
    const QStringList steps = board.bootInstructions.isEmpty()
        ? QStringList{tr("Disconnect USB and the flight battery."),
                      tr("Find the BOOT button or pads described in your board's manual."),
                      tr("Follow the manual's USB bootloader procedure, reconnecting USB to the same port.")}
        : board.bootInstructions.split('\n', Qt::SkipEmptyParts);
    auto *bootInstructions = new QWidget(card);
    bootInstructions->setObjectName("firmwareInstallBootInstructions");
    auto *bootLayout = new QVBoxLayout(bootInstructions);
    bootLayout->setContentsMargins(0, 8, 0, 8); bootLayout->setSpacing(16);
    for (int i = 0; i < steps.size(); ++i) {
        auto *row = new QHBoxLayout; row->setSpacing(12);
        auto *number = FirmwareUi::label(QString::number(i + 1), "stepNumber", bootInstructions);
        number->setFixedSize(28, 28); number->setAlignment(Qt::AlignCenter);
        row->addWidget(number, 0, Qt::AlignTop);
        auto *instruction = FirmwareUi::label(steps[i], "step", bootInstructions);
        instruction->setObjectName(QString("firmwareInstallBootStep%1").arg(i + 1));
        row->addWidget(instruction, 1, Qt::AlignVCenter); bootLayout->addLayout(row);
    }
    layout->addWidget(bootInstructions);
    auto *support = new QWidget(this);
    support->setObjectName("firmwareInstallSupport");
    auto *supportLayout = new QHBoxLayout(support);
    supportLayout->setContentsMargins(0, 4, 0, 0); supportLayout->setSpacing(12);
    auto *manual = new QLabel(support);
    manual->setObjectName("firmwareInstallBoardManual");
    manual->setTextFormat(Qt::RichText); manual->setOpenExternalLinks(true);
    manual->setTextInteractionFlags(Qt::TextBrowserInteraction);
    manual->setWordWrap(true);
    if (!board.manualUrl.isEmpty()) manual->setText(QString("<a href=\"%1\">%2</a>")
        .arg(board.manualUrl.toHtmlEscaped(), tr("Board manual").toHtmlEscaped()));
    manual->setProperty("role", "support");
    manual->setAccessibleName(tr("Board manual"));
    manual->setToolTip(tr("Open the %1 manual").arg(board.name));
    manual->setVisible(!board.manualUrl.isEmpty());
    supportLayout->addWidget(manual); supportLayout->addStretch();
    auto *status = new QLabel(this);
    status->setObjectName("firmwareInstallStatus");
    status->setProperty("role", "muted");
    status->setWordWrap(true); status->setTextFormat(Qt::PlainText);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    auto *accessRow = new QWidget(this);
    auto *accessLayout = new QHBoxLayout(accessRow);
    accessLayout->setContentsMargins(0, 0, 0, 0);
    auto *accessCommand = new QLabel(accessRow);
    accessCommand->setObjectName("firmwareInstallAccessCommand");
    accessCommand->setAccessibleName(tr("Terminal command"));
    accessCommand->setTextFormat(Qt::PlainText); accessCommand->setWordWrap(true);
    accessCommand->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    auto *copyAccessCommand = new QPushButton(tr("Copy command"), accessRow);
    copyAccessCommand->setObjectName("copyFirmwareInstallAccessCommand");
    accessLayout->addWidget(accessCommand, 1); accessLayout->addWidget(copyAccessCommand);
    accessRow->hide(); layout->addWidget(accessRow);
    connect(copyAccessCommand, &QPushButton::clicked, this, [accessCommand, copyAccessCommand] {
        QApplication::clipboard()->setText(accessCommand->text()); copyAccessCommand->setText(tr("Copied"));
    });
    auto *progressLabel = FirmwareUi::label({}, "muted", card);
    progressLabel->setObjectName("firmwareInstallProgressLabel");
    layout->addWidget(progressLabel);
    auto *progress = new QProgressBar(this);
    progress->setObjectName("firmwareInstallProgress");
    progress->setTextVisible(false);
    progress->setAccessibleName(tr("Installation progress"));
    layout->addWidget(progress);
    auto *reconnectHint = FirmwareUi::label(tr("If no live data appears, disconnect USB and the flight battery, then reconnect USB to the same port with BOOT released."), "muted", card);
    reconnectHint->setObjectName("firmwareInstallReconnectHint");
    reconnectHint->hide(); layout->addWidget(reconnectHint);
    auto *details = new QPlainTextEdit(this);
    details->setObjectName("firmwareInstallDetails"); details->setReadOnly(true);
    details->setAccessibleName(tr("Installation log"));
    details->setMaximumHeight(180); details->hide();
    auto *detailsToggle = new QToolButton(this);
    detailsToggle->setObjectName("firmwareInstallDetailsToggle");
    detailsToggle->setText(tr("View log")); detailsToggle->setCheckable(true);
    detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    detailsToggle->setArrowType(Qt::RightArrow);
    supportLayout->addWidget(detailsToggle);
    outer->addWidget(support); outer->addWidget(details);
    connect(detailsToggle, &QToolButton::toggled, this, [details, detailsToggle](bool expanded) {
        detailsToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        details->setVisible(expanded);
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Cancel)->setIcon(QIcon());
    auto *retry = buttons->addButton(tr("Retry"), QDialogButtonBox::ActionRole);
    retry->setProperty("primary", true);
    retry->setObjectName("firmwareInstallRetry"); retry->setEnabled(false);
    auto *startController = buttons->addButton(tr("Start controller"), QDialogButtonBox::ActionRole);
    startController->setObjectName("firmwareInstallStartController");
    startController->setProperty("primary", true); startController->hide();
    auto *readyActions = new QWidget(card);
    auto *readyLayout = new QHBoxLayout(readyActions); readyLayout->setContentsMargins(0, 0, 0, 0);
    auto *configure = new QPushButton(tr("Configure aircraft"), readyActions); configure->setProperty("primary", true);
    configure->setObjectName("firmwareInstallConfigureAction");
    auto *flight = new QPushButton(tr("Open flight data"), readyActions);
    readyLayout->addWidget(configure); readyLayout->addWidget(flight); readyLayout->addStretch();
    readyActions->hide();
    auto *actions = new QHBoxLayout;
    actions->addWidget(readyActions); actions->addStretch(); actions->addWidget(buttons);
    layout->addLayout(actions);
    const auto finish = [this](const QString &workspace) {
        emit closed();
        if (auto *modes = ModeManager::instance()) modes->activateModeByWorkspaceName(workspace);
    };
    connect(configure, &QPushButton::clicked, this, [this, finish] {
        if (!m_manager->controllerSetupRequired()) { finish("Configuration"); return; }
        auto manager = m_manager;
        manager->endFirmwareInstallation();
        emit closed();
        if (manager) manager->requestControllerSetup();
    });
    const auto refreshSetup = [this, heading, configure, status] {
        if (!complete()) return;
        const bool needsSetup = m_manager->controllerSetupRequired();
        heading->setText(needsSetup ? tr("Set up controller?") : tr("Your controller is ready"));
        configure->setText(needsSetup ? tr("Launch setup wizard") : tr("Configure aircraft"));
        status->setText(needsSetup
            ? tr("The setup wizard guides you through your aircraft, receiver and motor output settings.")
            : tr("DroniNG is connected and sending live data."));
    };
    connect(manager, &ConnectionManager::firmwareStateChanged, this, refreshSetup);
    connect(flight, &QPushButton::clicked, this, [finish] { finish("Flight data"); });
    connect(buttons, &QDialogButtonBox::rejected, this, &FirmwareInstallWidget::cancelInstallation);
    const auto current = manager->getCurrentDevice();
    const QString originalPath = current.device ? current.device->usbTopologyPath() : QString();
    manager->beginFirmwareInstallation();
    const auto start = [this, board, imagePath, originalPath] {
        m_autoStartAttempted = false;
        m_manager->disconnectDevice(); m_manager->suspendPolling();
        m_installer->start(board, imagePath, originalPath);
    };
    connect(retry, &QPushButton::clicked, this, start);
    auto *reconnect = new QTimer(this);
    reconnect->setInterval(500);
    auto *reconnectElapsed = new QElapsedTimer;
    connect(this, &QObject::destroyed, [reconnectElapsed] { delete reconnectElapsed; });
    auto *receivedDescription = new QByteArray;
    auto *receivedFlightData = new bool(false);
    connect(this, &QObject::destroyed, [receivedDescription, receivedFlightData] {
        delete receivedDescription; delete receivedFlightData;
    });
    const auto checkTelemetry = [this, receivedDescription, receivedFlightData] {
        if (*receivedFlightData && !receivedDescription->isEmpty())
            m_installer->telemetryReceived(*receivedDescription);
    };
    connect(manager, &ConnectionManager::firmwareDescriptionReceived, this,
            [receivedDescription, checkTelemetry](const QByteArray &description) {
        *receivedDescription = description; checkTelemetry();
    });
    connect(manager, &ConnectionManager::flightDataReady, this, [receivedFlightData, checkTelemetry] {
        *receivedFlightData = true; checkTelemetry();
    });
    connect(manager, &ConnectionManager::deviceDisconnected, this, [receivedDescription, receivedFlightData] {
        receivedDescription->clear(); *receivedFlightData = false;
    });
    connect(m_installer, &DfuInstaller::boardSettingsRequested, manager, &ConnectionManager::boardSettingsRequested);
    connect(manager, &ConnectionManager::boardSettingsFinished, this, [this](bool success, const QString &message) {
        m_installer->boardSettingsApplied(success, message);
        if (success) m_manager->disconnectDevice();
    });
    connect(m_installer, &DfuInstaller::nativeFirmwareStartRequested,
            manager, &ConnectionManager::firmwareBootRequested);
    connect(manager, &ConnectionManager::firmwareBootFinished,
            m_installer, &DfuInstaller::nativeFirmwareStarted);
    const auto startControllerFirmware = [this] {
        if (m_installer->stage() != DfuInstaller::Stage::AwaitingTelemetry) return;
        m_manager->disconnectDevice(); m_manager->suspendPolling();
        m_installer->startInstalledFirmware();
    };
    connect(startController, &QPushButton::clicked, this, startControllerFirmware);
    connect(m_installer, &DfuInstaller::firmwareWritten, this, [this, reconnect, reconnectElapsed](const QString &) {
        m_manager->resumePolling(); reconnectElapsed->start(); reconnect->start();
        if (!m_autoStartAttempted) {
            m_autoStartAttempted = true;
            QTimer::singleShot(5000, this, [this] {
                if (m_installer->stage() == DfuInstaller::Stage::AwaitingTelemetry && !m_manager->isConnected()) {
                    m_manager->suspendPolling(); m_installer->startInstalledFirmware();
                }
            });
        }
    });
    connect(reconnect, &QTimer::timeout, this, [this, reconnect, reconnectElapsed, status, progress] {
        if (m_installer->stage() == DfuInstaller::Stage::Restarting || m_installer->stage() == DfuInstaller::Stage::Leaving) return;
        if (m_installer->stage() != DfuInstaller::Stage::AwaitingTelemetry) { reconnect->stop(); return; }
        if (reconnectElapsed->elapsed() > 60000) {
            status->setText(tr("The controller has not reconnected yet."));
            progress->hide();
        }
        if (m_manager->isConnected()) return;
        const auto devices = m_manager->getAvailableDevices();
        QList<DevListItem> matches;
        for (const auto &device : devices) {
            if (device.device && device.device->usbTopologyPath() == m_installer->device().path
                && device.connection && device.connection->shortName() == "USB") matches.append(device);
        }
        if (matches.size() == 1) m_manager->connectDevice(matches.first());
    });
    connect(m_installer, &DfuInstaller::progress, progress, &QProgressBar::setValue);
    connect(m_installer, &DfuInstaller::transferProgress, this,
            [progressLabel, progress](const QString &task, int percent) {
        progressLabel->setText(tr("%1 · %2%").arg(task).arg(percent));
        progress->setAccessibleName(task);
        progress->setValue(percent);
    });
    connect(m_installer, &DfuInstaller::changed, this,
            [this, heading, readyActions, bootInstructions, manual, status, accessRow, accessCommand, copyAccessCommand,
             progress, progressLabel, reconnectHint, startController, details, retry, buttons,
             reconnect, reconnectElapsed, refreshSetup](DfuInstaller::Stage stage, const QString &message) {
        if (stage != DfuInstaller::Stage::Restarting) emit m_manager->firmwareBootCancelled();
        heading->setText(stage == DfuInstaller::Stage::Complete ? tr("Your controller is ready")
            : stage == DfuInstaller::Stage::Failed ? tr("Installation needs attention")
            : stage == DfuInstaller::Stage::Waiting || stage == DfuInstaller::Stage::NeedsBoot ? tr("Restart in BOOT mode")
            : stage == DfuInstaller::Stage::AwaitingTelemetry || stage == DfuInstaller::Stage::Leaving
                || stage == DfuInstaller::Stage::Restarting ? tr("Starting your controller")
            : stage == DfuInstaller::Stage::Configuring ? tr("Setting up your controller")
            : stage == DfuInstaller::Stage::Reading ? tr("Verifying installation")
            : stage == DfuInstaller::Stage::Checking || stage == DfuInstaller::Stage::Ready ? tr("Preparing installation")
            : tr("Installing DroniNG"));
        readyActions->setVisible(stage == DfuInstaller::Stage::Complete);
        const QString summary = stage == DfuInstaller::Stage::Waiting || stage == DfuInstaller::Stage::NeedsBoot
            ? tr("Installation starts automatically when your controller reconnects in BOOT mode.")
            : stage == DfuInstaller::Stage::Writing || stage == DfuInstaller::Stage::Reading ? tr("Keep USB connected.")
            : stage == DfuInstaller::Stage::AwaitingTelemetry || stage == DfuInstaller::Stage::Leaving
                || stage == DfuInstaller::Stage::Restarting ? tr("Firmware installed and verified.")
            : stage == DfuInstaller::Stage::Configuring ? tr("Applying your board's hardware settings…")
            : stage == DfuInstaller::Stage::Ready || stage == DfuInstaller::Stage::Checking ? tr("Checking the connection…")
            : stage == DfuInstaller::Stage::Complete ? tr("DroniNG is connected and sending live data.") : message;
        status->setText(summary); details->setPlainText(m_installer->log());
        if (stage == DfuInstaller::Stage::AwaitingTelemetry) {
            m_manager->resumePolling(); reconnectElapsed->start(); reconnect->start();
            if (!m_installer->startError().isEmpty()) status->setText(m_installer->startError());
        }
        startController->setVisible(stage == DfuInstaller::Stage::AwaitingTelemetry || stage == DfuInstaller::Stage::Restarting);
        startController->setEnabled(stage == DfuInstaller::Stage::AwaitingTelemetry);
        reconnectHint->setVisible(stage == DfuInstaller::Stage::AwaitingTelemetry);
        const QString task = stage == DfuInstaller::Stage::Waiting || stage == DfuInstaller::Stage::NeedsBoot
            ? tr("Detecting BOOT mode…")
            : stage == DfuInstaller::Stage::Writing ? tr("Erasing previous firmware…")
            : stage == DfuInstaller::Stage::Reading ? tr("Verifying firmware…")
            : stage == DfuInstaller::Stage::Leaving ? tr("Starting installed firmware…")
            : stage == DfuInstaller::Stage::Restarting ? tr("Sending start command…")
            : stage == DfuInstaller::Stage::AwaitingTelemetry ? tr("Waiting for live data…")
            : stage == DfuInstaller::Stage::Configuring ? tr("Saving board hardware settings…")
            : tr("Checking the controller…");
        progressLabel->setText(task); progress->setAccessibleName(task);
        const bool usbBlocked = m_installer->usbAccessDenied();
        accessCommand->setText(m_installer->accessCommand()); copyAccessCommand->setText(tr("Copy command"));
        accessRow->setVisible(!accessCommand->text().isEmpty());
        const bool showBootGuidance = !usbBlocked && (stage == DfuInstaller::Stage::Waiting
            || stage == DfuInstaller::Stage::NeedsBoot
            || (stage == DfuInstaller::Stage::Failed && m_installer->device().path.isEmpty()));
        bootInstructions->setVisible(showBootGuidance);
        manual->setVisible(showBootGuidance && !manual->text().isEmpty());
        retry->setEnabled(stage == DfuInstaller::Stage::Failed);
        retry->setVisible(stage == DfuInstaller::Stage::Failed);
        buttons->button(QDialogButtonBox::Cancel)->setEnabled(!m_installer->transferring());
        buttons->button(QDialogButtonBox::Cancel)->setText(stage == DfuInstaller::Stage::Complete ? tr("Done") : tr("Cancel"));
        if (stage == DfuInstaller::Stage::Ready) {
            m_manager->setNativeFirmwareIdentity({}, {}, tr("STM32 system bootloader"));
        } else if (stage == DfuInstaller::Stage::Complete) {
            m_manager->setNativeFirmwareIdentity({}, {}, tr("DroniNG"));
        }
        const bool uncertain = stage == DfuInstaller::Stage::Waiting || stage == DfuInstaller::Stage::NeedsBoot
            || stage == DfuInstaller::Stage::Ready
            || stage == DfuInstaller::Stage::Checking
            || stage == DfuInstaller::Stage::Restarting
            || stage == DfuInstaller::Stage::Leaving || stage == DfuInstaller::Stage::AwaitingTelemetry
            || stage == DfuInstaller::Stage::Configuring;
        progress->setRange(0, uncertain ? 0 : 100);
        progress->setValue(stage == DfuInstaller::Stage::Complete ? 100 : 0);
        const bool showProgress = !usbBlocked && stage != DfuInstaller::Stage::Failed
            && stage != DfuInstaller::Stage::Complete && stage != DfuInstaller::Stage::Idle;
        progress->setVisible(showProgress); progressLabel->setVisible(showProgress);
        refreshSetup();
        emit workflowChanged();
    });
    QTimer::singleShot(0, this, start);
}

bool FirmwareInstallWidget::verifyingConnection() const {
    return m_installer->stage() == DfuInstaller::Stage::AwaitingTelemetry
        || m_installer->stage() == DfuInstaller::Stage::Configuring || complete();
}
bool FirmwareInstallWidget::complete() const { return m_installer->stage() == DfuInstaller::Stage::Complete; }

void FirmwareInstallWidget::cancelInstallation()
{
    if (m_installer->transferring()) return;
    m_installer->cancel(); emit closed();
}
FirmwareInstallWidget::~FirmwareInstallWidget() {
    if (m_manager) {
        emit m_manager->firmwareBootCancelled();
        emit m_manager->boardSettingsCancelled(); m_manager->endFirmwareInstallation();
    }
}
