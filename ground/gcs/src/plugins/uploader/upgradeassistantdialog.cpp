/**
 ******************************************************************************
 * @file       upgradeassistantdialog.cpp
 * @author     dRonin, http://dRonin.org/, Copyright (C) 2016
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup  Uploader Uploader Plugin
 * @{
 * @brief The upgrade assistant dialog box
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

#include <QCloseEvent>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <coreplugin/firmwareuicomponents.h>

#include "upgradeassistantdialog.h"

UpgradeAssistantDialog::UpgradeAssistantDialog(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("firmwareUpgradeCard");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(16);
    m_identity = Core::FirmwareUi::label({}, "muted", this);
    m_status = Core::FirmwareUi::label({}, "section", this);
    m_status->setObjectName("firmwareUpgradeStatus");
    m_detail = Core::FirmwareUi::label({}, "muted", this);
    m_detail->setObjectName("firmwareUpgradeDetail");
    m_progress = new QProgressBar(this); m_progress->setObjectName("firmwareUpgradeProgress");
    m_progress->setTextVisible(false);
    m_percentage = Core::FirmwareUi::label({}, "muted", this);
    m_percentage->setObjectName("firmwareUpgradePercentage");
    m_backup = Core::FirmwareUi::label({}, "support", this);
    m_backup->setObjectName("firmwareUpgradeBackup");
    m_backup->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_openBackup = Core::FirmwareUi::label(tr("<a href=\"backup\">Open backup folder</a>"), "support", this);
    m_openBackup->setTextFormat(Qt::RichText);
    m_openBackup->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    m_openBackup->setObjectName("firmwareUpgradeOpenBackup");
    layout->addWidget(m_identity); layout->addWidget(m_status); layout->addWidget(m_detail);
    auto *progressRow = new QHBoxLayout;
    progressRow->addWidget(m_progress, 1); progressRow->addWidget(m_percentage);
    layout->addLayout(progressRow); layout->addWidget(m_backup); layout->addWidget(m_openBackup);
    auto *footer = new QHBoxLayout; footer->setObjectName("firmwareUpgradeActions");
    m_cancel = new QPushButton(tr("Cancel"), this); m_cancel->setObjectName("firmwareUpgradeCancel");
    m_primary = new QPushButton(this); m_primary->setObjectName("firmwareUpgradePrimary");
    m_primary->setProperty("primary", true);
    footer->addStretch(); footer->addWidget(m_cancel); footer->addWidget(m_primary);
    layout->addLayout(footer);
    connect(m_cancel, &QPushButton::clicked, this, &UpgradeAssistantDialog::reject);
    connect(m_primary, &QPushButton::clicked, this, [this] {
        if (!m_controller) return;
        if (m_controller->canConfigure()) emit configureRequested();
        else m_controller->proceed();
    });
    connect(m_openBackup, &QLabel::linkActivated, this, [this] {
        if (m_controller) QDesktopServices::openUrl(QUrl::fromLocalFile(m_controller->backupPath()));
    });
}

void UpgradeAssistantDialog::bind(uploader::FirmwareUpgradeController *controller)
{
    if (m_controller) disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    connect(controller, &uploader::FirmwareUpgradeController::changed, this, &UpgradeAssistantDialog::refresh);
    refresh();
}

void UpgradeAssistantDialog::refresh()
{
    if (!m_controller) return;
    using Controller = uploader::FirmwareUpgradeController;
    const auto state = m_controller->state();
    const bool terminal = state == Controller::Failed || state == Controller::Cancelled || state == Controller::Complete;
    const auto context = m_controller->context();
    m_identity->setText(tr("%1 · Controller %2 · Local firmware").arg(context.name, context.serial));
    m_status->setText(m_controller->title()); m_detail->setText(m_controller->detail());
    m_cancel->setText(terminal ? tr("Close") : tr("Cancel"));
    m_cancel->setEnabled(terminal || m_controller->canCancel());
    m_primary->setText(m_controller->primaryText()); m_primary->setVisible(!m_primary->text().isEmpty());
    const bool hasBackup = !m_controller->backupPath().isEmpty();
    m_backup->setVisible(hasBackup); m_openBackup->setVisible(hasBackup);
    m_backup->setText(tr("Settings backup saved to:\n%1").arg(m_controller->backupPath()));
    const bool active = state == Controller::ReadingSettings || state == Controller::WritingBootloader
        || state == Controller::WaitingBootloader || state == Controller::WritingFirmware
        || state == Controller::WritingSettings || state == Controller::ReadingConvertedSettings
        || state == Controller::ErasingSettings || state == Controller::WaitingFirmware || state == Controller::VerifyingSettings;
    m_progress->setVisible(active);
    m_percentage->setVisible(active && m_controller->progress() >= 0);
    m_percentage->setText(tr("%1%").arg(m_controller->progress()));
    m_progress->setRange(0, m_controller->progress() < 0 ? 0 : 100);
    m_progress->setValue(qMax(0, m_controller->progress()));
}

void UpgradeAssistantDialog::reject()
{
    if (m_controller && m_controller->canCancel()) { m_controller->cancel(); return; }
    if (m_controller && m_controller->state() != uploader::FirmwareUpgradeController::Failed
        && m_controller->state() != uploader::FirmwareUpgradeController::Cancelled
        && m_controller->state() != uploader::FirmwareUpgradeController::Complete) return;
    hide(); emit finished(0);
}

void UpgradeAssistantDialog::closeEvent(QCloseEvent *event)
{
    event->ignore(); reject();
}
