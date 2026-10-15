/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firmwareupgradecontroller.h"
#include <coreplugin/firmwareinstallimage.h>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QStandardPaths>

using namespace uploader;

FirmwareUpgradeController::FirmwareUpgradeController(UAVObjectManager *objects, QObject *parent)
    : QObject(parent), m_objects(objects)
{
    m_wait.setSingleShot(true);
    connect(&m_wait, &QTimer::timeout, this, [this] {
        if (m_state == WaitingBootloader) {
            fail(tr("The updated bootloader did not reconnect. Disconnect external power and USB, reconnect USB, then reopen Firmware maintenance. Your settings have not been erased."));
        } else if (m_state == WaitingFirmware) {
            setState(Reconnect, tr("Reconnect the controller"),
                tr("The firmware was written, but telemetry has not returned. Disconnect external power and USB, then reconnect USB. This card will continue automatically when the expected firmware connects."));
        }
    });
}

QString FirmwareUpgradeController::validateImage(const QByteArray &image, quint16 boardId, int capacity, bool application)
{
    if (image.size() <= 100 || capacity <= 0 || image.size() > capacity || (image.size() % 4))
        return tr("The local image is missing, truncated or too large for this controller.");
    const QByteArray descriptor = image.right(100);
    if (!descriptor.startsWith("TlFw") || ((quint16(uchar(descriptor[12])) << 8) | uchar(descriptor[13])) != boardId)
        return tr("The local image does not match this controller's board identity.");
    if (QCryptographicHash::hash(image.left(image.size() - 100), QCryptographicHash::Sha1) != descriptor.mid(40, 20))
        return tr("The local image failed its integrity check. Reinstall the application package before retrying.");
    if (application && descriptor.mid(60, 20) != Core::FirmwareInstallImage::gcsUavoHash())
        return tr("The local firmware settings definitions do not match this application build.");
    return {};
}

bool FirmwareUpgradeController::sameControllerSerial(QString first, QString second)
{
    // USB adds a run-state suffix to the CPU identity reported over telemetry.
    const auto cpuSerial = [](QString serial) {
        serial = serial.trimmed();
        for (const auto &suffix : {QStringLiteral("+BL"), QStringLiteral("+FW"), QStringLiteral("+UP")})
            if (serial.endsWith(suffix, Qt::CaseInsensitive)) { serial.chop(3); break; }
        return serial;
    };
    first = cpuSerial(first); second = cpuSerial(second);
    return !first.isEmpty() && first.compare(second, Qt::CaseInsensitive) == 0;
}

void FirmwareUpgradeController::start(const FirmwareUpgradeContext &context)
{
    if (m_state != Idle && m_state != Failed && m_state != Cancelled && m_state != Complete) return;
    m_context = context; m_settings = {}; m_backupPath.clear(); m_reset = false; m_cancelRead = false; m_confirmedFirmware = false;
    m_transferDisconnected = false; m_retryConversion = false;
    setState(Preparing, tr("Preparing %1 firmware update").arg(context.name),
        tr("Checking the bundled images and the controller's storage before changing firmware."));
    QString error = validateImage(context.firmware, context.boardId, context.codeSize, true);
    if (error.isEmpty() && context.descriptionSize < 100) error = tr("The bootloader cannot store the required firmware metadata.");
    if (error.isEmpty() && context.serial.isEmpty()) error = tr("The controller identity is unavailable. Reconnect one controller and retry.");
    if (error.isEmpty() && context.needsBootloader)
        error = validateImage(context.bootloader, context.boardId, context.codeSize, false);
    if (!error.isEmpty()) { fail(error); return; }
    if (m_context.backupRoot.isEmpty()) m_context.backupRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("firmware-backups");
    if (context.settingsSize > 0) { readSettings(); return; }
    if (!context.needsBootloader) {
        fail(tr("The settings partition is unavailable. Reconnect the controller in its native bootloader and retry; settings will not be erased without a backup.")); return;
    }
    setState(BootloaderDecision, tr("Waiting for your decision"),
        tr("This bootloader cannot read the settings partition. The bundled bootloader updater must run first to capture a backup. It replaces the current application temporarily and leaves settings in place. Keep USB connected until the bootloader returns."));
}

void FirmwareUpgradeController::setState(State state, const QString &title, const QString &detail)
{
    m_wait.stop(); m_state = state; m_title = title; m_detail = detail; m_progress = -1;
    emit changed();
}

void FirmwareUpgradeController::readSettings()
{
    if (m_context.settingsSize <= 0 || m_context.settingsSize > 16 * 1024 * 1024 || m_context.settingsSize % 4) {
        fail(tr("The bootloader reported an invalid settings partition size. No further firmware or settings will be written.")); return;
    }
    setState(ReadingSettings, tr("Backing up controller settings"), tr("Reading the settings partition. A local backup is required before installation can continue."));
    emit readSettingsRequested(m_context.settingsSize);
}

void FirmwareUpgradeController::settingsRead(const QByteArray &data, bool success)
{
    if (m_state != ReadingSettings) return;
    if (m_cancelRead) { cancelled(); return; }
    if (!success || m_transferDisconnected || data.size() != m_context.settingsSize) {
        fail(tr("The settings partition could not be read completely. Reconnect USB and retry. Installation cannot continue without a complete backup.")); return;
    }
    const bool identifiedFirmware = m_context.sourceDescriptor.size() == 100
        && m_context.sourceDescriptor.startsWith("TlFw")
        && m_context.sourceDescriptor.mid(12, 2) == m_context.firmware.right(100).mid(12, 2);
    m_settings = FirmwareSettingsBackup::inspect(data, m_context.target, identifiedFirmware, m_objects);
    const QJsonObject provenance{{"target", m_context.target}, {"controllerSerial", m_context.serial},
        {"boardId", m_context.boardId}, {"sourceDescriptor", QString::fromLatin1(m_context.sourceDescriptor.toHex())},
        {"destinationDescriptor", QString::fromLatin1(m_context.firmware.right(100).toHex())},
        {"compatible", m_settings.compatible}, {"empty", m_settings.empty}, {"compatibilityReport", m_settings.reason},
        {"conversions", QJsonArray::fromStringList(m_settings.conversions)},
        {"convertedSha256", m_settings.convertedPartition.isEmpty() ? QString() : QString::fromLatin1(
            QCryptographicHash::hash(m_settings.convertedPartition, QCryptographicHash::Sha256).toHex())}};
    QString error;
    m_backupPath = FirmwareSettingsBackup::save(m_context.backupRoot, data, provenance, m_settings.xml, &error);
    if (m_backupPath.isEmpty()) { fail(error); return; }
    m_reset = !m_settings.compatible;
    if (m_context.needsBootloader) {
        setState(BootloaderDecision, tr("Waiting for your decision"),
            tr("Your settings backup is saved. The bundled bootloader updater must run before installing firmware. Keep USB connected until the bootloader returns.")); return;
    }
    setState(InstallDecision, tr("Waiting for your decision"), m_reset
        ? m_settings.reason + " " + tr("Installing will reset controller settings. The raw backup remains available; run the setup wizard after installation.")
        : m_settings.empty ? tr("The settings partition is empty. The local firmware is ready to install; set up the controller after it connects.")
        : !m_settings.conversions.isEmpty() ? tr("Your saved settings can be preserved. Receiver settings will be converted automatically with their values unchanged. Installation will verify the settings before starting firmware and again after reconnecting.")
        : tr("Your saved settings are compatible with this firmware. Installation will keep them in place and verify them after reconnecting."));
}

QString FirmwareUpgradeController::primaryText() const
{
    if (m_state == BootloaderDecision) return tr("Update bootloader");
    if (m_state == InstallDecision) return m_reset ? tr("Install and reset settings")
        : !m_settings.conversions.isEmpty() ? tr("Install and preserve settings") : tr("Install firmware");
    if (m_state == Reconnect) return tr("Check connection");
    if (m_state == Failed) return m_confirmedFirmware ? tr("Review configuration")
        : m_retryConversion ? tr("Restore saved settings") : tr("Retry preparation");
    if (m_state == Complete) return (m_reset || m_settings.empty) ? tr("Launch setup wizard") : tr("Configure aircraft");
    return {};
}

bool FirmwareUpgradeController::canCancel() const
{
    return m_state == Preparing || m_state == ReadingSettings || m_state == InstallDecision
        || m_state == BootloaderDecision || m_state == WaitingFirmware || m_state == Reconnect;
}

void FirmwareUpgradeController::proceed()
{
    if (m_state == BootloaderDecision) {
        setState(WritingBootloader, tr("Updating bootloader"), tr("Keep USB connected. Cancellation is unavailable while the bootloader updater is written and run."));
        emit writeRequested(m_context.bootloader);
    } else if (m_state == InstallDecision) {
        if (m_backupPath.isEmpty()) { fail(tr("Installation requires a saved settings backup.")); return; }
        if (m_reset) {
            setState(ErasingSettings, tr("Resetting controller settings"), tr("The backup is saved and you chose to reset settings. Keep USB connected until installation finishes."));
            emit eraseSettingsRequested();
        } else writeApplication();
    } else if (m_state == Reconnect) emit connectionCheckRequested();
    else if (m_state == Failed) emit retryRequested();
}

void FirmwareUpgradeController::writeApplication()
{
    setState(WritingFirmware, tr("Installing local firmware"), tr("Keep USB connected while firmware and settings are installed and verified."));
    emit writeRequested(m_context.firmware);
}

void FirmwareUpgradeController::settingsErased(bool success)
{
    if (m_state != ErasingSettings) return;
    if (!success || m_transferDisconnected) { fail(tr("The settings reset could not be verified. Installation stopped. Reconnect USB and retry using the saved backup.")); return; }
    writeApplication();
}

void FirmwareUpgradeController::writeFinished(bool success)
{
    if (m_state != WritingBootloader && m_state != WritingFirmware) return;
    if (!success || m_transferDisconnected) {
        fail(tr("The firmware or metadata write failed. The controller may need recovery. Reconnect USB in its native bootloader, reopen Firmware maintenance and retry.")
            + " " + (m_backupPath.isEmpty() ? tr("Your existing settings have not been erased.") : tr("Your settings backup remains available.")));
        return;
    }
    const bool loader = m_state == WritingBootloader;
    if (loader) {
        setState(WaitingBootloader, tr("Waiting for updated bootloader"), tr("Keep USB connected while the bootloader updater runs and the controller reconnects."));
        m_wait.start(30000); emit bootRequested(true);
    } else if (!m_settings.convertedPartition.isEmpty()) {
        m_retryConversion = true;
        setState(WritingSettings, tr("Updating saved settings"), tr("The firmware is written. Applying the verified receiver settings conversion while preserving all saved values. Keep USB connected."));
        emit writeSettingsRequested(m_settings.convertedPartition);
    } else bootApplication();
}

void FirmwareUpgradeController::convertedSettingsWritten(bool success)
{
    if (m_state != WritingSettings) return;
    if (!success || m_transferDisconnected) {
        fail(tr("The converted settings could not be written. The firmware will stay in its bootloader. Reconnect USB, then choose Restore saved settings to retry from the original backup.")); return;
    }
    setState(ReadingConvertedSettings, tr("Checking converted settings"), tr("Reading the settings partition back before starting the firmware."));
    emit readConvertedSettingsRequested(m_context.settingsSize);
}

void FirmwareUpgradeController::convertedSettingsRead(const QByteArray &data, bool success)
{
    if (m_state != ReadingConvertedSettings) return;
    if (!success || m_transferDisconnected || data != m_settings.convertedPartition) {
        fail(tr("The converted settings did not match on readback. The firmware will stay in its bootloader. Reconnect USB, then choose Restore saved settings to retry from the original backup.")); return;
    }
    m_retryConversion = false;
    bootApplication();
}

void FirmwareUpgradeController::retrySettingsConversion(quint16 boardId, const QString &serial,
    int codeSize, int settingsSize, const QByteArray &descriptor)
{
    if (!canRetrySettingsConversion()) return;
    if (boardId != m_context.boardId || !sameControllerSerial(serial, m_context.serial)
        || codeSize != m_context.codeSize || settingsSize != m_context.settingsSize
        || descriptor.size() != 100 || descriptor.left(80) != m_context.firmware.right(100).left(80)) {
        fail(tr("The controller, firmware or storage layout differs from the saved conversion. Reconnect the controller you updated before retrying.")); return;
    }
    m_transferDisconnected = false;
    setState(WritingSettings, tr("Restoring converted settings"),
        tr("Retrying the receiver settings conversion from the original backup. Keep USB connected until the settings are written and verified."));
    emit writeSettingsRequested(m_settings.convertedPartition);
}

void FirmwareUpgradeController::bootApplication()
{
    setState(WaitingFirmware, tr("Waiting for the new firmware"),
        tr("The image and metadata were written successfully. Waiting for this controller to reconnect and confirm its firmware and settings."));
    m_wait.start(60000); emit bootRequested(false);
}

void FirmwareUpgradeController::bootloaderReady(quint16 boardId, const QString &serial,
    int codeSize, int settingsSize, bool currentLoader)
{
    if (m_state != WaitingBootloader) return;
    if (boardId != m_context.boardId || !sameControllerSerial(serial, m_context.serial) || codeSize != m_context.codeSize || !currentLoader) {
        fail(tr("The reconnected bootloader does not match the expected controller or storage layout. No further writes will be performed.")); return;
    }
    m_context.needsBootloader = false;
    m_context.settingsSize = settingsSize;
    readSettings();
}

void FirmwareUpgradeController::firmwareConnected(const QByteArray &descriptor, const QString &serial)
{
    if (m_state != WaitingFirmware && m_state != Reconnect) return;
    const auto expected = m_context.firmware.right(100);
    if (!sameControllerSerial(serial, m_context.serial) || descriptor.size() != 100 || descriptor.left(80) != expected.left(80)) {
        fail(tr("The connected controller or firmware does not match the installed image. Reconnect the controller you updated; completion has not been confirmed.")); return;
    }
    m_confirmedFirmware = true;
    if (m_reset || m_settings.empty) { settingsVerified(true); return; }
    setState(VerifyingSettings, tr("Verifying saved settings"), tr("Reading the controller's saved settings through telemetry and comparing them with the local backup."));
    emit verifySettingsRequested(m_settings.records);
}

void FirmwareUpgradeController::settingsVerified(bool success, const QString &reason)
{
    if (m_state != VerifyingSettings && m_state != WaitingFirmware && m_state != Reconnect) return;
    if (!success) { fail(tr("The new firmware connected, but saved settings could not be verified. %1 Keep the backup and review the controller configuration before use.").arg(reason)); return; }
    setState(Complete, tr("Firmware update complete"), (m_reset || m_settings.empty)
        ? tr("The expected firmware is connected. Set up your aircraft, receiver and motor outputs before use.")
        : tr("The expected firmware is connected and your saved settings match the backup."));
    emit settled();
}

void FirmwareUpgradeController::transferProgress(int progress)
{
    if (m_state != ReadingSettings && m_state != WritingBootloader && m_state != WritingFirmware
        && m_state != ErasingSettings && m_state != WritingSettings && m_state != ReadingConvertedSettings) return;
    m_progress = qBound(0, progress, 100); emit changed();
}

void FirmwareUpgradeController::cancel()
{
    if (!canCancel()) return;
    if (m_state == ReadingSettings) {
        if (m_cancelRead) return;
        m_cancelRead = true; m_title = tr("Stopping settings backup");
        m_detail = tr("Waiting for the read operation to stop safely. No further writes will be performed.");
        emit changed(); emit cancelReadRequested(); return;
    }
    cancelled();
}

void FirmwareUpgradeController::cancelled()
{
    setState(Cancelled, tr("Firmware update stopped"), tr("No further changes will be made. You can close this card and return to Firmware maintenance."));
    emit settled();
}

void FirmwareUpgradeController::deviceRemoved()
{
    if (m_state == WaitingBootloader || m_state == WaitingFirmware || m_state == Reconnect || m_state == Idle
        || m_state == Complete || m_state == Cancelled || m_state == Failed) return;
    // In-flight transfers settle through their worker completion; never close their USB handle here.
    m_transferDisconnected = true;
    if (m_state == ReadingSettings) { m_detail = tr("USB disconnected. Waiting for the backup read to stop safely."); emit changed(); return; }
    if (m_state == WritingBootloader || m_state == WritingFirmware || m_state == ErasingSettings
        || m_state == WritingSettings || m_state == ReadingConvertedSettings) return;
    fail(tr("The controller disconnected before installation. Reconnect USB and retry; the saved backup remains available."));
}

void FirmwareUpgradeController::fail(const QString &reason)
{
    setState(Failed, tr("Firmware update stopped"), reason); emit settled();
}
