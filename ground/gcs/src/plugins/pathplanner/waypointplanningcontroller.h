/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "pathplanner_global.h"
#include <QObject>
#include <QHash>
#include <QElapsedTimer>
#include <QTimer>

class UAVObject;
class UAVObjectManager;
class BoardSettingsInstaller;
namespace Core { class ConnectionManager; }

// Eligibility always belongs to the current telemetry session, not a cached board.
class PATHPLANNER_EXPORT WaypointPlanningController : public QObject
{
    Q_OBJECT
public:
    explicit WaypointPlanningController(UAVObjectManager *objects,
        Core::ConnectionManager *connection, QObject *parent = nullptr);
    bool ready() const { return m_ready; }
    bool canEnable() const { return m_canEnable; }
    bool offersSetup() const { return m_offersSetup; }
    bool canUpload() const;
    QString status() const { return m_status; }
public slots:
    void setConnected(bool connected);
    void enablePlanning();
signals:
    void changed();
private:
    void refresh();
    void received(UAVObject *object);
    void reconnectController();
    bool fresh(const QString &name, int age = 15000) const;
    QString value(const QString &object, const QString &field, const QString &element = {}) const;
    QString follower() const;
    UAVObjectManager *m_objects;
    Core::ConnectionManager *m_connection;
    BoardSettingsInstaller *m_installer;
    QElapsedTimer m_clock;
    QTimer m_poll;
    QHash<QString, qint64> m_received;
    bool m_connected = false, m_ready = false, m_canEnable = false;
    bool m_offersSetup = false;
    bool m_saving = false, m_restarting = false, m_sawDisconnect = false;
    qint64 m_started = 0;
    qint64 m_connectedAt = 0;
    QString m_status, m_error, m_usbPath, m_deviceName, m_connectionName, m_serial;
};
