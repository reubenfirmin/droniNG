/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbaccesssetup.h"
#include "dfuinstaller.h"
#include <QGuiApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

using namespace Core;

namespace {
QString read(const QString &path)
{
    if (path.isEmpty()) return {};
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QStringList rules(const QString &contents)
{
    QStringList result;
    for (auto line : contents.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        // The supplied rules have single-line expressions without quoted commas.
        // Accept formatting and field order changes without requiring a filename.
        auto fields = line.split(',');
        for (auto &field : fields) field.remove(QRegularExpression("\\s+"));
        fields.sort();
        result.append(fields.join(','));
    }
    return result;
}

UsbAccessSetup::Paths systemPaths()
{
    return {DfuInstaller::bundledUsbAccessRules(),
        {"/usr/lib/udev/rules.d", "/usr/local/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d"},
        "/sys/bus/usb/devices", "/dev/bus/usb"};
}
}

UsbAccessSetup::State UsbAccessSetup::inspect(const Paths &paths)
{
#ifndef Q_OS_LINUX
    Q_UNUSED(paths)
    return State::Ready;
#else
    const auto supplied = read(paths.bundledRules);
    const auto required = rules(supplied);
    if (required.isEmpty()) return State::MissingRules;

    // udev processes filenames in order; a higher-precedence directory masks
    // the same filename in a lower one, including a symlink to /dev/null.
    QMap<QString, QString> files;
    for (const auto &directory : paths.ruleDirectories) {
        const QDir dir(directory);
        for (const auto &name : dir.entryList({"*.rules"}, QDir::Files | QDir::System))
            files[name] = dir.filePath(name);
    }
    QSet<QString> installed;
    for (auto file = files.cbegin(); file != files.cend(); ++file) {
        // uaccess tags must be set before systemd applies desktop-session ACLs.
        if (file.key() >= "73-seat-late.rules") continue;
        const auto entries = rules(read(file.value()));
        for (const auto &entry : entries) installed.insert(entry);
    }
    for (const auto &entry : required)
        if (!installed.contains(entry)) return State::InstallRules;

    // Rules may have been installed after a connected device appeared. Check
    // effective access without opening, claiming or communicating with it.
    QSet<QString> supported;
    const QRegularExpression identity("ATTRS?\\{idVendor\\}==\"([0-9a-fA-F]{4})\".*ATTRS?\\{idProduct\\}==\"([0-9a-fA-F]{4})\"");
    for (const auto &line : supplied.split('\n')) {
        const auto match = identity.match(line);
        if (match.hasMatch()) supported.insert((match.captured(1) + ":" + match.captured(2)).toLower());
    }
    const QDir devices(paths.usbDevices);
    for (const auto &name : devices.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir device(devices.filePath(name));
        const QString id = read(device.filePath("idVendor")).trimmed().toLower() + ":"
            + read(device.filePath("idProduct")).trimmed().toLower();
        if (!supported.contains(id)) continue;
        bool busOk = false, deviceOk = false;
        const int bus = read(device.filePath("busnum")).trimmed().toInt(&busOk);
        const int number = read(device.filePath("devnum")).trimmed().toInt(&deviceOk);
        if (!busOk || !deviceOk || bus < 1 || number < 1) continue;
        const QString node = QDir(paths.usbNodes).filePath(QString("%1/%2")
            .arg(bus, 3, 10, QLatin1Char('0')).arg(number, 3, 10, QLatin1Char('0')));
        // A missing node can be a device removal racing this read-only scan.
        if (QFileInfo::exists(node) && ::access(QFile::encodeName(node).constData(), R_OK | W_OK) != 0)
            return State::Reconnect;
    }
    return State::Ready;
#endif
}

UsbAccessSetup::UsbAccessSetup(QObject *parent) : UsbAccessSetup(systemPaths(), parent) {}

UsbAccessSetup::UsbAccessSetup(const Paths &paths, QObject *parent)
    : QObject(parent), m_paths(paths)
{
    refresh();
    auto *poll = new QTimer(this); poll->setInterval(2000);
    connect(poll, &QTimer::timeout, this, &UsbAccessSetup::refresh); poll->start();
}

QString UsbAccessSetup::title() const
{
    switch (m_state) {
    case State::Ready: return tr("Ready to connect");
    case State::InstallRules: return tr("Enable USB connections");
    case State::Reconnect: return tr("Reconnect your controller");
    case State::MissingRules: return tr("Repair your installation");
    }
    return {};
}

QString UsbAccessSetup::message() const
{
    switch (m_state) {
    case State::Ready:
        return tr("Connect your controller for tuning, configuration and live diagnostics.");
    case State::InstallRules:
        return tr("Install the Linux USB access rules so DroniNG can connect controllers and update firmware.");
    case State::Reconnect:
        return tr("USB access is set up. Unplug and reconnect your controller to apply it.");
    case State::MissingRules:
        return tr("This installation is missing the USB access rules. Reinstall the Linux package to enable controller access.");
    }
    return {};
}

QString UsbAccessSetup::command() const
{
    return m_state == State::InstallRules ? DfuInstaller::usbAccessRulesCommand(m_paths.bundledRules) : QString();
}

bool UsbAccessSetup::copyCommand()
{
    const auto text = command();
    if (text.isEmpty()) return false;
    QGuiApplication::clipboard()->setText(text);
    if (!m_copied) { m_copied = true; emit copiedChanged(); }
    return true;
}

void UsbAccessSetup::refresh()
{
    const auto state = inspect(m_paths);
    if (m_state == state) return;
    m_state = state;
    if (m_copied) { m_copied = false; emit copiedChanged(); }
    emit statusChanged();
}
