/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DFUINSTALLER_H
#define DFUINSTALLER_H
#include "firmwareinstallimage.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTemporaryDir>

namespace Core {
// Commands are argument lists, never shell commands. Tests substitute this transport.
class CORE_EXPORT DfuCommandRunner : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void run(const QString &program, const QStringList &arguments, int timeoutMs) = 0;
    virtual void cancel() = 0;
    virtual QString connectedUsbDevice(const QString &usbPath) const;
signals:
    void output(const QString &text);
    void finished(int exitCode, const QString &output);
};

struct CORE_EXPORT DfuFlashDevice {
    QString path, serial, layout;
    int configuration = -1, interface = -1, alternate = -1;
    bool supports(quint32 base, quint32 size) const;
    bool operator==(const DfuFlashDevice &other) const;
};

class CORE_EXPORT DfuInstaller : public QObject {
    Q_OBJECT
public:
    enum class Stage { Idle, Waiting, NeedsBoot, Ready, Checking, Writing, Reading, Leaving, Restarting,
                       AwaitingTelemetry, StartupFailed, Configuring, Complete, Failed };
    explicit DfuInstaller(QObject *parent = nullptr, DfuCommandRunner *runner = nullptr);
    static QString bundledHelper();
    static QString bundledUsbAccessRules();
    static QString usbAccessRulesCommand(const QString &rulesPath);
    static QList<DfuFlashDevice> devices(const QString &listing);
    // Starts a requested installation and continues automatically when DFU appears.
    void start(const FirmwareBoardProfile &board, const QString &imagePath,
               const QString &expectedUsbPath = {}, const QString &helper = bundledHelper(), bool automaticBoot = false);
    void cancel();
    void startInstalledFirmware();
    void nativeFirmwareStarted(bool success, const QString &message);
    void startupTimedOut(const QString &message = {});
    void telemetryReceived(const QByteArray &descriptor, int schema = 0, const QString &profile = {},
                           const QString &layout = {}, const QByteArray &profileHash = {});
    void boardSettingsApplied(bool success, const QString &message);
    Stage stage() const { return m_stage; }
    QString log() const { return m_log; }
    DfuFlashDevice device() const { return m_device; }
    bool transferring() const;
    bool usbAccessDenied() const { return m_usbAccessDenied; }
    QString accessCommand() const { return m_accessCommand; }
    QString startError() const { return m_startError; }
    QByteArray installedDescriptor() const { return m_image.descriptor; }
signals:
    void changed(Core::DfuInstaller::Stage stage, const QString &message);
    void progress(int percent);
    void transferProgress(const QString &task, int percent);
    void firmwareWritten(const QString &usbPath);
    void nativeFirmwareStartRequested(const QString &usbPath, const QByteArray &descriptor);
    void boardSettingsRequested(const QJsonArray &settings);
private:
    void commandFinished(int exitCode, const QString &output);
    void checkDevice();
    void listDevices();
    void transfer(const QStringList &arguments);
    void leaveDfu();
    void writeSegment();
    int m_segment = 0;
    void awaitTelemetry(const QString &startError = {});
    void change(Stage stage, const QString &message);
    void fail(const QString &message);
    Stage m_stage = Stage::Idle;
    DfuCommandRunner *m_runner;
    FirmwareInstallImage m_image;
    DfuFlashDevice m_device;
    QString m_helper, m_expectedPath, m_log, m_usbDescription, m_accessCommand;
    QString m_progressOutput, m_startError;
    quint64 m_startGeneration = 0;
    bool m_restarting = false;
    QTemporaryDir m_temporary;
    QElapsedTimer m_wait;
    QJsonArray m_settings;
    bool m_configured = false;
    bool m_usbAccessDenied = false;
    bool m_automaticBoot = false;
};
}
Q_DECLARE_METATYPE(Core::DfuInstaller::Stage)
#endif
