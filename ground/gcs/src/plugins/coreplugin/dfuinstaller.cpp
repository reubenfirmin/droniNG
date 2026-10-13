/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dfuinstaller.h"
#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

using namespace Core;

namespace {
QString usbAccessRules(const QString &helper)
{
    const QDir directory = QFileInfo(helper).absoluteDir();
    for (const auto &relative : {"../rules.udev", "../../../shared/usb_ids/droning.udev"}) {
        const QFileInfo file(directory.filePath(relative));
        if (file.isFile() && file.isReadable()) return file.canonicalFilePath();
    }
    return {};
}

class ProcessRunner final : public DfuCommandRunner {
public:
    explicit ProcessRunner(QObject *parent) : DfuCommandRunner(parent) {
        process.setProcessChannelMode(QProcess::MergedChannels);
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [this] { timedOut = true; process.kill(); });
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
            const QString chunk = QString::fromLocal8Bit(process.readAllStandardOutput());
            data = (data + chunk).right(256 * 1024);
            emit output(chunk);
        });
        connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus status) {
            timeout.stop();
            if (timedOut) data += "\nDFU command timed out.";
            emit finished(timedOut || status != QProcess::NormalExit ? -1 : code, data);
        });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                timeout.stop();
                emit finished(-1, process.errorString());
            }
        });
    }
    void run(const QString &program, const QStringList &arguments, int timeoutMs) override {
        data.clear(); timedOut = false;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("LC_ALL", "C");
        process.setProcessEnvironment(environment);
        process.start(program, arguments);
        timeout.start(timeoutMs);
    }
    void cancel() override { timeout.stop(); process.kill(); }
    ~ProcessRunner() override {
        QObject::disconnect(&process, nullptr, this, nullptr);
        process.kill(); process.waitForFinished(1000);
    }
private:
    QProcess process;
    QTimer timeout;
    QString data;
    bool timedOut = false;
};
}

QString DfuCommandRunner::connectedUsbDevice(const QString &usbPath) const
{
#ifdef Q_OS_LINUX
    if (!QRegularExpression("^[0-9]+-[0-9]+(?:\\.[0-9]+)*$").match(usbPath).hasMatch()) return {};
    const QDir device(QString("/sys/bus/usb/devices/%1").arg(usbPath));
    const auto read = [&device](const QString &name) {
        QFile file(device.filePath(name));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
    };
    const auto vendor = read("idVendor"), product = read("idProduct");
    if (vendor.isEmpty() || product.isEmpty() || (vendor == "0483" && product == "df11")) return {};
    QStringList names;
    for (const auto &name : {read("manufacturer"), read("product")})
        if (!name.isEmpty() && !names.contains(name)) names.append(name);
    return names.isEmpty() ? QString("%1:%2").arg(vendor, product) : names.join(" / ");
#else
    Q_UNUSED(usbPath);
    return {};
#endif
}

bool DfuFlashDevice::operator==(const DfuFlashDevice &other) const
{
    return path == other.path && serial == other.serial && layout == other.layout
        && configuration == other.configuration && interface == other.interface && alternate == other.alternate;
}

bool DfuFlashDevice::supports(quint32 base, quint32 size) const
{
    // DfuSe's a..g encode read/erase/write permissions as bits 1/2/4.
    const auto parts = layout.split('/');
    if (parts.size() != 3 || parts[0].trimmed() != "@Internal Flash") return false;
    bool ok;
    quint64 address = parts[1].trimmed().toULongLong(&ok, 0);
    if (!ok || address != base || !size) return false;
    quint64 end = quint64(base) + size;
    for (const auto &part : parts[2].split(',')) {
        const auto match = QRegularExpression("^\\s*([0-9]+)\\*([0-9]+)([BKMbkm])([a-g])\\s*$").match(part);
        if (!match.hasMatch()) return false;
        const quint64 count = match.captured(1).toULongLong();
        const quint64 unit = match.captured(3).toUpper() == "M" ? 1024 * 1024 : match.captured(3).toUpper() == "K" ? 1024 : 1;
        const quint64 page = match.captured(2).toULongLong() * unit;
        if (!count || count > 4096 || !page || page > 2 * 1024 * 1024 || count * page > 2 * 1024 * 1024) return false;
        const quint64 next = address + count * page;
        if (address < end && (match.captured(4) != "g" || (end < next && (end - address) % page))) return false;
        address = next;
    }
    return end <= address && address <= quint64(base) + 2 * 1024 * 1024;
}

static bool importedFlashGeometry(const QString &layout, int flashSize)
{
    const auto parts = layout.split('/');
    if (parts.size() != 3) return false;
    QList<quint64> sectors;
    for (const auto &part : parts[2].split(',')) {
        const auto match = QRegularExpression("^\\s*([0-9]+)\\*([0-9]+)([BKMbkm])g\\s*$").match(part);
        if (!match.hasMatch()) return false;
        const int count = match.captured(1).toInt();
        if (count < 1 || count + sectors.size() > 12) return false;
        const auto unit = match.captured(3).toUpper();
        const quint64 size = match.captured(2).toULongLong() * (unit == "M" ? 1048576 : unit == "K" ? 1024 : 1);
        for (int i = 0; i < count; ++i) sectors.append(size);
    }
    QList<quint64> expected{16384, 16384, 16384, 16384, 65536};
    if (flashSize != 524288 && flashSize != 1048576) return false;
    for (int i = 0; i < (flashSize - 131072) / 131072; ++i) expected.append(131072);
    return sectors == expected;
}

QList<DfuFlashDevice> DfuInstaller::devices(const QString &listing)
{
    QList<DfuFlashDevice> result;
    const QRegularExpression row("Found DFU: \\[0483:df11\\].*cfg=([0-9]+), intf=([0-9]+), path=\"([0-9]+-[0-9]+(?:\\.[0-9]+)*)\", alt=([0-9]+), name=\"([^\"]+)\", serial=\"([^\"]*)\"");
    for (const auto &line : listing.split('\n')) {
        const auto match = row.match(line);
        if (!match.hasMatch() || !match.captured(5).startsWith("@Internal Flash")) continue;
        DfuFlashDevice device;
        device.configuration = match.captured(1).toInt(); device.interface = match.captured(2).toInt();
        device.path = match.captured(3); device.alternate = match.captured(4).toInt();
        device.layout = match.captured(5); device.serial = match.captured(6);
        result.append(device);
    }
    return result;
}

QString DfuInstaller::bundledHelper()
{
#ifdef Q_OS_LINUX
    const QFileInfo file(QDir(QCoreApplication::applicationDirPath()).filePath("dfu-util"));
    if (file.isExecutable() && file.isFile()) return file.absoluteFilePath();
#endif
    return {};
}

QString DfuInstaller::usbAccessRulesCommand(const QString &rulesPath)
{
    if (rulesPath.isEmpty()) return {};
    QString path = QFileInfo(rulesPath).absoluteFilePath();
    path.replace("'", "'\\''");
    return QString("sudo install -m 0644 '%1' /etc/udev/rules.d/45-droning.rules && sudo udevadm control --reload-rules").arg(path);
}

QString DfuInstaller::bundledUsbAccessRules()
{
    return usbAccessRules(QDir(QCoreApplication::applicationDirPath()).filePath("dfu-util"));
}

DfuInstaller::DfuInstaller(QObject *parent, DfuCommandRunner *runner)
    : QObject(parent), m_runner(runner ? runner : new ProcessRunner(this))
{
    connect(m_runner, &DfuCommandRunner::finished, this, &DfuInstaller::commandFinished);
    connect(m_runner, &DfuCommandRunner::output, this, [this](const QString &text) {
        auto matches = QRegularExpression("([0-9]{1,3})%").globalMatch(text);
        while (matches.hasNext()) emit progress(qMin(100, matches.next().captured(1).toInt()));
        if (m_stage != Stage::Writing && m_stage != Stage::Reading) return;
        m_progressOutput = (m_progressOutput + text).right(4096);
        auto transfers = QRegularExpression("(Erase|Download|Upload)[^\\r\\n]*?([0-9]{1,3})%")
            .globalMatch(m_progressOutput);
        int consumed = 0;
        while (transfers.hasNext()) {
            const auto match = transfers.next();
            const QString task = match.captured(1) == "Erase" ? tr("Erasing previous firmware")
                : match.captured(1) == "Download" ? tr("Writing firmware") : tr("Verifying firmware");
            emit transferProgress(task, qMin(100, match.captured(2).toInt()));
            consumed = match.capturedEnd();
        }
        m_progressOutput.remove(0, consumed);
    });
}

void DfuInstaller::change(Stage stage, const QString &message)
{
    if (m_stage != stage) m_progressOutput.clear();
    m_stage = stage;
    m_log = (m_log + '\n' + message + '\n').right(256 * 1024);
    qInfo().noquote() << "Firmware installation:" << message;
    emit changed(stage, message);
}
void DfuInstaller::fail(const QString &message) {
    change(Stage::Failed, message);
    qWarning().noquote() << "Firmware installation failed:" << m_log;
}
bool DfuInstaller::transferring() const
{
    return m_stage == Stage::Checking || m_stage == Stage::Writing || m_stage == Stage::Reading
        || m_stage == Stage::Leaving || m_stage == Stage::Configuring;
}

void DfuInstaller::start(const FirmwareBoardProfile &board, const QString &imagePath,
                         const QString &expectedUsbPath, const QString &helper, bool automaticBoot)
{
    if (transferring() || m_stage == Stage::Waiting || m_stage == Stage::NeedsBoot || m_stage == Stage::Restarting) return;
    m_log.clear(); m_usbDescription.clear(); m_device = {}; m_helper = helper; m_expectedPath = expectedUsbPath;
    m_usbAccessDenied = false; m_accessCommand.clear();
    m_automaticBoot = automaticBoot;
    m_startError.clear(); m_restarting = false; ++m_startGeneration;
    m_settings = board.settings; m_configured = false;
    m_image = FirmwareInstallImage::load(board, imagePath);
    if (!m_image.valid()) { fail(m_image.error); return; }
    if (!QFileInfo(helper).isExecutable()) {
        fail(tr("The Linux firmware installer is missing from this build. Rebuild the firmware installation bundle.")); return;
    }
    if (!m_temporary.isValid()) { fail(tr("Cannot create a temporary directory for firmware verification.")); return; }
    m_segment = 0;
    for (int i = 0; i < m_image.segments.size(); ++i) {
        const auto region = m_image.segments[i];
        const auto bytes = m_image.bytes.mid(region.offset, region.size);
        QFile file(m_temporary.filePath(QString("install-%1.bin").arg(i)));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            fail(tr("Cannot prepare the validated installation image.")); return;
        }
        QFile::remove(m_temporary.filePath(QString("readback-%1.bin").arg(i)));
    }
    QFile::remove(m_temporary.filePath("leave.bin"));
    m_wait.start();
    change(Stage::Waiting, tr("Waiting for STM32 DFU…"));
    listDevices();
}

void DfuInstaller::listDevices() { m_runner->run(m_helper, {"-d", ",0483:df11", "-l"}, 10000); }
void DfuInstaller::checkDevice()
{
    if (m_stage != Stage::Ready) return;
    change(Stage::Checking, tr("Checking the selected DFU device before writing…"));
    listDevices();
}
void DfuInstaller::startInstalledFirmware()
{
    if (m_stage != Stage::AwaitingTelemetry && m_stage != Stage::StartupFailed) return;
    m_startError.clear(); m_restarting = true; ++m_startGeneration;
    m_usbAccessDenied = false; m_accessCommand.clear();
    change(Stage::Restarting, tr("Checking the installed controller's bootloader…"));
    listDevices();
}

void DfuInstaller::nativeFirmwareStarted(bool success, const QString &message)
{
    if (m_stage != Stage::Restarting) return;
    awaitTelemetry(success ? QString() : message);
}

void DfuInstaller::awaitTelemetry(const QString &startError)
{
    const bool installed = m_stage == Stage::Leaving && !m_restarting;
    m_startError = startError;
    change(Stage::AwaitingTelemetry, startError.isEmpty()
        ? tr("Firmware installed and verified. Waiting for live data…") : startError);
    if (installed) emit firmwareWritten(m_device.path);
}

void DfuInstaller::leaveDfu()
{
    change(Stage::Leaving, tr("Firmware verified. Starting DroniNG…"));
    // dfu-util creates upload files exclusively. Remove the previous one when
    // retrying this read-only start command; never download or erase flash here.
    QFile::remove(m_temporary.filePath("leave.bin"));
    transfer({"-s", QString("0x%1:1:leave").arg(m_image.base, 0, 16), "-U", m_temporary.filePath("leave.bin")});
}
void DfuInstaller::cancel()
{
    if (transferring()) return;
    const bool written = m_stage == Stage::AwaitingTelemetry || m_stage == Stage::StartupFailed
        || m_stage == Stage::Complete || m_stage == Stage::Restarting;
    m_usbAccessDenied = false; m_accessCommand.clear();
    m_startError.clear(); ++m_startGeneration;
    change(Stage::Idle, written ? tr("Installation view closed. Verified firmware remains on the controller.")
                              : tr("Installation cancelled."));
    m_runner->cancel();
}
void DfuInstaller::transfer(const QStringList &arguments)
{
    QStringList selector{"-d", ",0483:df11", "-p", m_device.path, "-c", QString::number(m_device.configuration),
                         "-i", QString::number(m_device.interface), "-a", QString::number(m_device.alternate)};
    if (!m_device.serial.isEmpty()) selector << "-S" << (',' + m_device.serial);
    m_runner->run(m_helper, selector + arguments, 180000);
}

void DfuInstaller::writeSegment()
{
    const auto region = m_image.segments[m_segment];
    change(Stage::Writing, tr("Writing firmware region %1 of %2… Keep USB connected.")
        .arg(m_segment + 1).arg(m_image.segments.size()));
    transfer({"-s", QString("0x%1").arg(m_image.base + region.offset, 0, 16), "-D",
              m_temporary.filePath(QString("install-%1.bin").arg(m_segment))});
}

void DfuInstaller::commandFinished(int exitCode, const QString &output)
{
    if (m_stage == Stage::Idle || m_stage == Stage::Failed || m_stage == Stage::Complete) return;
    m_log = (m_log + output).right(256 * 1024);
    if (output.contains("LIBUSB_ERROR_ACCESS") || output.contains("Permission denied")) {
        m_usbAccessDenied = true;
        m_accessCommand = usbAccessRulesCommand(usbAccessRules(m_helper));
        if (m_stage == Stage::Restarting || m_stage == Stage::Leaving) {
            awaitTelemetry(tr("Linux blocked the start command. Run the USB access command below, then reconnect USB with BOOT released."));
            return;
        }
        fail(m_accessCommand.isEmpty()
            ? tr("The board is in DFU mode, but Linux is blocking access and this build is missing its USB access rules. Reinstall the Linux package, reconnect in BOOT mode and retry.")
            : tr("The board is in DFU mode, but Linux is blocking access. Run the command below once in a terminal, then reconnect the board while holding BOOT and click Retry."));
        return;
    }
    if (output.contains("unable to initialize libusb", Qt::CaseInsensitive)) {
        if (m_stage == Stage::Restarting || m_stage == Stage::Leaving) {
            awaitTelemetry(tr("USB is unavailable for the start command. Reconnect the controller in a desktop session with USB access."));
            return;
        }
        fail(tr("Linux USB access is unavailable. Run the GCS in a desktop session with access to USB devices, then retry. See details for the USB error.")); return;
    }
    if (exitCode != 0) {
        if (m_stage == Stage::Restarting || m_stage == Stage::Leaving) {
            awaitTelemetry(tr("The controller did not answer the start command."));
            return;
        }
        fail(tr("The DFU operation failed. Keep the board in BOOT/DFU mode and retry; details show the device error.")); return;
    }
    if (m_stage == Stage::Restarting) {
        const auto found = devices(output);
        if (found.isEmpty()) {
            if (m_image.romBoot) {
                const auto connected = m_runner->connectedUsbDevice(m_device.path);
                awaitTelemetry(connected.isEmpty()
                    ? tr("The controller has left the USB bootloader but has not appeared on USB yet. Disconnect USB and the flight battery, then reconnect USB to the same port.")
                    : tr("The controller has left the USB bootloader. Waiting for live data from %1…").arg(connected));
                return;
            }
            const quint64 generation = m_startGeneration;
            emit nativeFirmwareStartRequested(m_device.path, m_image.descriptor);
            QTimer::singleShot(20000, this, [this, generation] {
                if (m_stage == Stage::Restarting && m_startGeneration == generation)
                    nativeFirmwareStarted(false, tr("The controller did not answer the start command."));
            });
        } else if (found.size() == 1 && found.first() == m_device) {
            leaveDfu();
        } else {
            nativeFirmwareStarted(false, tr("The connected bootloader does not match the installed controller. Reconnect it to the same USB port with BOOT released."));
        }
        return;
    }
    const bool discovering = m_stage == Stage::Waiting || m_stage == Stage::NeedsBoot;
    if (discovering || m_stage == Stage::Checking) {
        const auto found = devices(output);
        if (found.isEmpty() && discovering) {
            const QString connected = m_runner->connectedUsbDevice(m_expectedPath);
            if (connected != m_usbDescription) {
                m_usbDescription = connected;
                if (connected.isEmpty()) change(Stage::Waiting, tr("Waiting for STM32 DFU…"));
                else if (m_automaticBoot && m_wait.elapsed() < 5000)
                    change(Stage::Waiting, tr("Restarting %1 into the USB bootloader…").arg(connected));
                else change(Stage::NeedsBoot, tr("Connected as %1. Bootloader mode is not active. Check your board's manual for its bootloader procedure.").arg(connected));
            }
            if (m_wait.elapsed() >= 120000) {
                fail(tr("The board has not appeared in DFU mode. Check the USB data cable and your board's manual for its bootloader procedure, then retry.")); return;
            }
            QTimer::singleShot(1000, this, [this] {
                if (m_stage == Stage::Waiting || m_stage == Stage::NeedsBoot) listDevices();
            });
            return;
        }
        if (found.size() != 1) {
            fail(tr("A single STM32 DFU device is required. Disconnect other DFU boards and retry.")); return;
        }
        const auto device = found.first();
        if ((!m_expectedPath.isEmpty() && device.path != m_expectedPath)
            || (m_stage == Stage::Checking && !(device == m_device))) {
            fail(tr("The DFU device does not match the selected controller. Use the same USB port and retry.")); return;
        }
        if (!device.supports(m_image.base, m_image.bytes.size())
            || (m_image.romBoot && !importedFlashGeometry(device.layout, m_image.bytes.size()))) {
            fail(tr("The DFU flash layout cannot safely hold this installation image. No firmware was written.")); return;
        }
        if (discovering) {
            m_device = device;
            change(Stage::Ready, tr("STM32 DFU detected. Starting installation…"));
            checkDevice();
            return;
        }
        writeSegment();
    } else if (m_stage == Stage::Writing) {
        change(Stage::Reading, tr("Reading back the firmware to verify every byte… Keep USB connected."));
        const auto region = m_image.segments[m_segment];
        transfer({"-s", QString("0x%1:%2").arg(m_image.base + region.offset, 0, 16).arg(region.size), "-U",
                  m_temporary.filePath(QString("readback-%1.bin").arg(m_segment))});
    } else if (m_stage == Stage::Reading) {
        const auto region = m_image.segments[m_segment];
        QFile readback(m_temporary.filePath(QString("readback-%1.bin").arg(m_segment)));
        if (!readback.open(QIODevice::ReadOnly) || readback.size() != region.size
            || readback.readAll() != m_image.bytes.mid(region.offset, region.size)) {
            fail(tr("Firmware verification failed. The board will stay in DFU mode. Retry installation before using it.")); return;
        }
        if (++m_segment < m_image.segments.size()) writeSegment();
        else leaveDfu();
    } else if (m_stage == Stage::Leaving) {
        awaitTelemetry();
    }
}

void DfuInstaller::telemetryReceived(const QByteArray &descriptor, int schema, const QString &profile,
                                     const QString &layout, const QByteArray &profileHash)
{
    if (m_stage != Stage::AwaitingTelemetry && m_stage != Stage::StartupFailed) return;
    if (descriptor != m_image.descriptor) {
        fail(tr("The connected controller's firmware identity differs from the image just installed.")); return;
    }
    if (m_image.romBoot) {
        if (profileHash.isEmpty()) {
            change(Stage::AwaitingTelemetry, tr("Live data received. Reading the controller's board profile…"));
            return;
        }
        if (schema != 1 || profile != m_image.profileId || layout != m_image.layoutId
            || profileHash != m_image.profileHash) {
            fail(tr("The controller's board profile differs from the installed image. Reconnect in DFU mode and reinstall the matching firmware."));
            return;
        }
    }
    if (!m_settings.isEmpty() && !m_configured) {
        change(Stage::Configuring, tr("Configuring the board's hardware and sensor orientation…"));
        emit boardSettingsRequested(m_settings);
        return;
    }
    change(Stage::Complete, tr("DroniNG installed and verified. The controller is sending flight data."));
}

void DfuInstaller::startupTimedOut(const QString &message)
{
    if (m_stage != Stage::AwaitingTelemetry) return;
    m_startError = message.isEmpty()
        ? tr("Firmware was written and verified, but the controller has not started sending live data. Disconnect USB and the flight battery, reconnect USB to the same port, then click Start controller to retry.")
        : message;
    change(Stage::StartupFailed, m_startError);
}

void DfuInstaller::boardSettingsApplied(bool success, const QString &message)
{
    if (m_stage != Stage::Configuring) return;
    if (!success) { fail(message); return; }
    m_configured = true;
    change(Stage::AwaitingTelemetry, message);
    emit firmwareWritten(m_device.path);
}
