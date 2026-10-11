/**
 ******************************************************************************
 *
 * @file       connectionmanager.h
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 *             Parts by Nokia Corporation (qt-info@nokia.com) Copyright (C) 2009.
 * @author     dRonin, http://dronin.org Copyright (C) 2015-2016
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup CorePlugin Core Plugin
 * @{
 * @brief The Core GCS plugin
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

#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include "QWidget"
#include "mainwindow.h"
#include "generalsettings.h"
#include "telemetrymonitorwidget.h"
#include <coreplugin/iconnection.h>
#include <QtCore/QVector>
#include <QtCore/QIODevice>
#include <QtCore/QList>
#include <QPushButton>
#include <QComboBox>
#include <QPointer>
#include <QSet>
#include <QMessageBox>

#include "core_global.h"
#include <QTimer>
#include <QJsonArray>

QT_BEGIN_NAMESPACE
class QTabWidget;
QT_END_NAMESPACE

namespace Core {
struct FirmwareBoardProfile;

class IConnection;
class IDevice;

namespace Internal {
    class MainWindow;
} // namespace Internal

class DevListItem
{
public:
    DevListItem(IConnection *c, IDevice *d)
        : connection(c)
        , device(d)
    {
    }

    DevListItem()
        : connection(NULL)
    {
    }

    QString getConName()
    {
        if (connection == NULL || device.isNull())
            return "";
        return connection->shortName() + ": " + device->getDisplayName();
    }

    bool operator==(const DevListItem &rhs) const
    {
        return connection == rhs.connection && device == rhs.device;
    }

    bool operator!=(const DevListItem &rhs) const { return !(*this == rhs); }

    IConnection *connection;
    QPointer<IDevice> device;
};

class CORE_EXPORT ConnectionManager : public QWidget
{
    Q_OBJECT

public:
    ConnectionManager(Internal::MainWindow *mainWindow, QTabWidget *modeStack);
    virtual ~ConnectionManager();

    void init();

    QIODevice *getCurrentConnection() { return m_ioDev; }
    DevListItem getCurrentDevice() { return m_connectionDevice; }
    DevListItem findDevice(const QString &devName);

    QList<DevListItem> getAvailableDevices() { return m_devList; }

    bool isConnected() { return m_ioDev != nullptr; }
    bool firmwareInstallationActive() const { return m_firmwareInstalling; }
    bool firmwareDetectionActive() const { return m_firmwareDetecting; }
    bool flightDataAvailable() const { return m_flightDataAvailable; }
    bool controllerSetupRequired() const { return m_controllerSetupRequired; }

    bool connectDevice(DevListItem device);
    bool disconnectDevice();
    void suspendPolling();
    void resumePolling();
    TelemetryMonitorWidget *getTelemetryMonitorWidget() { return m_monitorWidget; }
    bool getAutoconnect();
    QString firmwareVariant() const { return m_firmwareVariant; }
    QString firmwareBoardTarget() const { return m_firmwareBoardTarget; }
    bool hasMspFirmware() const { return m_mspFirmwareDetected; }
    QString firmwareVersion() const { return m_firmwareVersion; }
    QString detectedBoard() const { return m_detectedBoard; }
    QString detectedFirmware() const { return m_detectedFirmware; }
    QString detectedBootloader() const { return m_detectedBootloader; }
    QString firmwareStatus() const { return m_firmwareStatus; }
    ConnectionError firmwareError() const { return m_firmwareError; }
    void setFirmwareIdentity(const QString &variant, const QString &boardTarget,
                             bool mspDetected = false, const QString &version = {});
    void setNativeFirmwareIdentity(const QString &board, const QString &firmware,
                                   const QString &bootloader = {});
    void setFirmwareStatus(const QString &message);
    void setFirmwareDetectionActive(bool active);
    void beginFirmwareInstallation();
    void endFirmwareInstallation();
    void reportFirmwareDescription(const QByteArray &description);
    void reportFlightDataReceived();
    void setControllerSetupRequired(bool required);
    void requestControllerSetup();

protected:
    void updateConnectionList(IConnection *connection);
    void registerDevice(IConnection *conn, IDevice *device);
    void updateConnectionDropdown();

signals:
    void deviceConnected(QIODevice *device);
    void deviceAboutToDisconnect();
    void deviceDisconnected();
    void availableDevicesChanged(const QList<Core::DevListItem> devices);
    void firmwareDescriptionReceived(const QByteArray &description);
    void flightDataReady();
    void boardSettingsRequested(const QJsonArray &settings);
    void boardSettingsFinished(bool success, const QString &message);
    void boardSettingsCancelled();
    void firmwareBootRequested(const QString &usbPath, const QByteArray &descriptor);
    void firmwareBootFinished(bool success, const QString &message);
    void firmwareBootCancelled();
    void firmwareStateChanged();
    void firmwarePageRequested();
    void setupWizardRequested();

public slots:
    void telemetryConnected();
    void telemetryDisconnected();
    void telemetryUpdated(double txRate, double rxRate);
    void telemetryConnectionFailed(const Core::ConnectionError &error);
    void telemetryHandshakeCompleted();
    void showFirmwarePage();

private slots:
    void objectAdded(QObject *obj);
    void aboutToRemoveObject(QObject *obj);

    void onConnectClicked();
    void devChanged(IConnection *connection);

    void connectionsCallBack(); // used to call devChange after all the plugins are loaded
    void reconnectSlot();
    void reconnectCheckSlot();

protected:
    QComboBox *m_availableDevList;
    QPushButton *m_connectBtn;
    QList<DevListItem> m_devList;
    QList<IConnection *> m_connectionsList;

    // tx/rx telemetry monitor
    TelemetryMonitorWidget *m_monitorWidget;

    // currently connected connection plugin
    DevListItem m_connectionDevice;

    // Last thing the user tried to connect to manually
    DevListItem m_lastManualConnect;

    // currently connected QIODevice
    QIODevice *m_ioDev;

private:
    bool connectDevice();
    bool polling;
    bool m_firmwareInstalling = false;
    bool m_firmwareDetecting = false;
    bool m_flightDataAvailable = false;
    bool m_controllerSetupRequired = false;
    QSet<IConnection *> m_enumeratedConnections;
    Internal::MainWindow *m_mainWindow;
    QList<IConnection *> connectionBackup;
    QTimer *reconnect;
    QTimer *reconnectCheck;
    ConnectionError m_connectionError;
    QMetaObject::Connection m_connectionDestroyed;
    quint64 m_connectionGeneration = 0;
    QString m_firmwareVariant, m_firmwareBoardTarget;
    QString m_firmwareVersion, m_detectedBoard, m_detectedFirmware, m_detectedBootloader;
    QString m_firmwareStatus;
    ConnectionError m_firmwareError;
    bool m_mspFirmwareDetected = false;

    void connectDeviceFailed(DevListItem &device);
};

} // namespace Core

#endif // CONNECTIONMANAGER_H
