/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firmwarebootcontroller.h"
#include <coreplugin/connectionmanager.h>
#include <coreplugin/idevice.h>
#include <coreplugin/icore.h>
#include <coreplugin/boardmanager.h>
#include <QThread>

using namespace uploader;

namespace {
class NativeBootTransport final : public FirmwareBootTransport {
public:
    bool open(const USBPortInfo &port) override { return loader.OpenBootloaderComs(port); }
    tl_dfu::device capabilities() override { return loader.findCapabilities(); }
    QByteArray description(int size) override { return loader.DownloadDescriptionAsByteArray(size); }
    bool start() override { return loader.JumpToApp(false) > 0; }
private:
    tl_dfu::DFUObject loader{2000};
};
}

FirmwareBootController::FirmwareBootController(Core::ConnectionManager *manager, QObject *parent)
    : QObject(parent), m_manager(manager)
{
    connect(manager, &Core::ConnectionManager::firmwareBootRequested, this, &FirmwareBootController::start);
    connect(manager, &Core::ConnectionManager::firmwareBootCancelled, this, &FirmwareBootController::cancel);
    connect(manager, &QObject::destroyed, this, &FirmwareBootController::cancel);
}

FirmwareBootController::~FirmwareBootController()
{
    cancel();
    if (m_thread) m_thread->wait();
}

void FirmwareBootController::cancel()
{
    if (m_cancelled) m_cancelled->store(true);
    if (m_thread) m_thread->requestInterruption();
}

bool FirmwareBootController::startVerifiedFirmware(FirmwareBootTransport &transport,
        const USBPortInfo &port, const QByteArray &descriptor,
        const std::function<bool()> &cancelled, QString *error)
{
    error->clear();
    if (cancelled()) return false;
    if (descriptor.size() != 100 || (!descriptor.startsWith("TlFw") && !descriptor.startsWith("OpFw"))) {
        *error = tr("The installed firmware identity is unavailable.");
        return false;
    }
    if (!transport.open(port)) {
        *error = tr("The controller's bootloader did not answer.");
        return false;
    }
    if (cancelled()) return false;
    const auto caps = transport.capabilities();
    const int boardId = (quint8(descriptor[12]) << 8) | quint8(descriptor[13]);
    if (caps.ID != boardId || caps.HW_Rev != quint8(descriptor[13]) || caps.SizeOfDesc != descriptor.size()) {
        *error = tr("This bootloader does not match the installed firmware. Reconnect the controller to the same USB port.");
        return false;
    }
    if (cancelled()) return false;
    if (transport.description(caps.SizeOfDesc) != descriptor) {
        *error = tr("This controller's firmware differs from the installation. Reconnect the installed controller to the same USB port.");
        return false;
    }
    if (cancelled()) return false;
    if (!transport.start()) {
        *error = tr("The controller did not accept the start command.");
        return false;
    }
    return true;
}

void FirmwareBootController::start(const QString &usbPath, const QByteArray &descriptor)
{
    if (!m_manager || !m_manager->firmwareInstallationActive()) return;
    if (m_thread && m_thread->isRunning()) {
        emit m_manager->firmwareBootFinished(false, tr("The previous start attempt is finishing. Try Start controller again in a moment."));
        return;
    }
    QList<USBPortInfo> matches;
    const auto vendors = Core::ICore::instance()->boardManager()->getKnownVendorIDs();
    for (auto port : USBMonitor::instance()->availableDevices()) {
        if (vendors.contains(port.vendorID) && port.getRunState() == USBMonitor::Bootloader
            && !usbPath.isEmpty() && Core::IDevice::usbTopologyForDeviceNode(port.path) == usbPath)
            matches.append(port);
    }
    if (matches.size() != 1) {
        emit m_manager->firmwareBootFinished(false,
            tr("The controller is not available to start over USB."));
        return;
    }
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancelled;
    struct Result { bool success = false; QString error; };
    auto result = std::make_shared<Result>();
    auto *thread = QThread::create([port = matches.first(), descriptor, cancelled, result] {
        NativeBootTransport transport;
        result->success = startVerifiedFirmware(transport, port, descriptor,
            [cancelled] { return cancelled->load(); }, &result->error);
    });
    m_thread = thread; thread->setParent(this);
    connect(thread, &QThread::finished, this, [this, cancelled, result] {
        if (m_manager && m_manager->firmwareInstallationActive() && !cancelled->load())
            emit m_manager->firmwareBootFinished(result->success, result->error);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
