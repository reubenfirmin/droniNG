/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FIRMWAREUPGRADECONTROLLER_H
#define FIRMWAREUPGRADECONTROLLER_H

#include "firmwaresettingsbackup.h"
#include <QObject>
#include <QTimer>

namespace uploader {

struct FirmwareUpgradeContext {
    QString target, name, serial, backupRoot;
    quint16 boardId = 0;
    int codeSize = 0, descriptionSize = 0, settingsSize = 0;
    bool needsBootloader = false;
    QByteArray sourceDescriptor, firmware, bootloader;
};

// Transport-independent workflow. All device completions are asynchronous; no UI event loops.
class UPLOADER_EXPORT FirmwareUpgradeController : public QObject
{
    Q_OBJECT
public:
    enum State { Idle, Preparing, ReadingSettings, BootloaderDecision, WritingBootloader,
        WaitingBootloader, InstallDecision, ErasingSettings, WritingFirmware, WritingSettings,
        ReadingConvertedSettings, WaitingFirmware,
        Reconnect, VerifyingSettings, Complete, Failed, Cancelled };
    Q_ENUM(State)
    explicit FirmwareUpgradeController(UAVObjectManager *objects, QObject *parent = nullptr);
    void start(const FirmwareUpgradeContext &context);
    State state() const { return m_state; }
    QString title() const { return m_title; }
    QString detail() const { return m_detail; }
    QString backupPath() const { return m_backupPath; }
    QString primaryText() const;
    bool canCancel() const;
    bool resetSettings() const { return m_reset; }
    bool setupRequired() const { return m_reset || m_settings.empty; }
    bool canConfigure() const { return m_state == Complete || (m_state == Failed && m_confirmedFirmware); }
    bool transferDisconnected() const { return m_transferDisconnected; }
    bool canRetrySettingsConversion() const { return m_state == Failed && m_retryConversion; }
    int progress() const { return m_progress; }
    FirmwareUpgradeContext context() const { return m_context; }
    static QString validateImage(const QByteArray &image, quint16 boardId, int capacity, bool application);
    static bool sameControllerSerial(QString first, QString second);

public slots:
    void proceed();
    void cancel();
    void settingsRead(const QByteArray &data, bool success);
    void writeFinished(bool success);
    void settingsErased(bool success);
    void convertedSettingsWritten(bool success);
    void convertedSettingsRead(const QByteArray &data, bool success);
    void retrySettingsConversion(quint16 boardId, const QString &serial, int codeSize,
                                 int settingsSize, const QByteArray &descriptor);
    void bootloaderReady(quint16 boardId, const QString &serial, int codeSize, int settingsSize, bool currentLoader);
    void firmwareConnected(const QByteArray &descriptor, const QString &serial);
    void settingsVerified(bool success, const QString &reason = {});
    void transferProgress(int progress);
    void deviceRemoved();
    void fail(const QString &reason);

signals:
    void changed();
    void readSettingsRequested(int size);
    void cancelReadRequested();
    void writeRequested(const QByteArray &image);
    void eraseSettingsRequested();
    void writeSettingsRequested(const QByteArray &partition);
    void readConvertedSettingsRequested(int size);
    void bootRequested(bool bootloaderUpdate);
    void verifySettingsRequested(const QList<uploader::FirmwareSettingRecord> &records);
    void connectionCheckRequested();
    void retryRequested();
    void settled();

private:
    void setState(State state, const QString &title, const QString &detail);
    void readSettings();
    void writeApplication();
    void bootApplication();
    void cancelled();
    State m_state = Idle;
    QString m_title, m_detail, m_backupPath;
    int m_progress = -1;
    bool m_reset = false, m_cancelRead = false, m_confirmedFirmware = false;
    bool m_transferDisconnected = false;
    bool m_retryConversion = false;
    FirmwareUpgradeContext m_context;
    FirmwareSettingsInspection m_settings;
    UAVObjectManager *m_objects;
    QTimer m_wait;
};
}
#endif
