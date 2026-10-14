/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "waypointplanningcontroller.h"
#include <coreplugin/connectionmanager.h>
#include <coreplugin/iconnection.h>
#include <uavobjects/uavobjectmanager.h>
#include <uavobjects/uavdataobject.h>
#include <uavobjects/uavobjectfield.h>
#include <uavtalk/boardsettingsinstaller.h>
#include <QJsonArray>
#include <QJsonObject>

static const QStringList watchedObjects = {"GPSPosition", "TaskInfo", "ModuleSettings",
    "SystemSettings", "StateEstimation", "FlightStatus", "HomeLocation", "PathPlannerSettings"};

WaypointPlanningController::WaypointPlanningController(UAVObjectManager *objects,
    Core::ConnectionManager *connection, QObject *parent)
    : QObject(parent), m_objects(objects), m_connection(connection),
      m_installer(new BoardSettingsInstaller(objects, this))
{
    m_clock.start();
    m_installer->setRequireDisarmed(true);
    for (const auto &name : watchedObjects) {
        if (auto *object = objects->getObject(name))
            connect(object, &UAVObject::objectUnpacked, this, &WaypointPlanningController::received);
    }
    connect(m_installer, &BoardSettingsInstaller::finished, this, [this](bool success, const QString &message) {
        m_saving = false;
        m_restarting = success;
        m_started = m_clock.elapsed();
        if (!success) m_error = tr("Waypoint setup did not finish. %1").arg(message);
        refresh();
    });
    m_poll.setInterval(1000);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (m_restarting && m_clock.elapsed() - m_started > 45000) {
            m_restarting = false;
            m_error = tr("Restart could not be confirmed. Reconnect this controller and check its GPS, compass and barometer in Configuration.");
        }
        reconnectController();
        if (m_connected && !m_saving) {
            for (const auto &name : watchedObjects)
                if (!fresh(name, 5000))
                    if (auto *object = m_objects->getObject(name)) object->requestUpdate();
        }
        refresh();
    });
    m_poll.start();
    refresh();
}

void WaypointPlanningController::setConnected(bool connected)
{
    if (m_connected == connected) return;
    m_connected = connected;
    m_received.clear();
    if (!connected) {
        m_sawDisconnect = true;
        m_installer->disconnected();
    } else {
        m_connectedAt = m_clock.elapsed();
        if (m_restarting && m_connection && !m_serial.isEmpty()
            && !m_connection->controllerSerial().isEmpty() && m_serial != m_connection->controllerSerial()) {
            m_restarting = false;
            m_error = tr("A different controller is connected. Check waypoint setup on this controller.");
        } else if (!m_restarting && !m_saving) m_error.clear();
        for (const auto &name : watchedObjects)
            if (auto *object = m_objects->getObject(name)) object->requestUpdate();
    }
    refresh();
}

void WaypointPlanningController::received(UAVObject *object)
{
    if (!m_connected) return;
    m_received.insert(object->getName(), m_clock.elapsed());
    refresh();
}

bool WaypointPlanningController::fresh(const QString &name, int age) const
{
    return m_connected && m_received.contains(name) && m_clock.elapsed() - m_received.value(name) < age;
}

QString WaypointPlanningController::value(const QString &object, const QString &field, const QString &element) const
{
    auto *obj = m_objects->getObject(object);
    auto *f = obj ? obj->getField(field) : nullptr;
    const int index = element.isEmpty() ? 0 : f ? f->getElementIndex(element) : -1;
    return f && index >= 0 ? f->getValue(index).toString() : QString();
}

QString WaypointPlanningController::follower() const
{
    const auto type = value("SystemSettings", "AirframeType");
    if (type.startsWith("FixedWing")) return "FixedWingPathFollower";
    if (type == "VTOL" || type == "HeliCP" || type.startsWith("Quad")
        || type.startsWith("Hexa") || type.startsWith("Octo") || type == "Tri")
        return "VtolPathFollower";
    return {};
}

bool WaypointPlanningController::canUpload() const
{
    const auto fix = value("GPSPosition", "Status");
    return m_ready && fresh("GPSPosition", 10000) && fresh("TaskInfo")
        && fresh("HomeLocation") && value("HomeLocation", "Set") == "TRUE"
        && (fix == "Fix3D" || fix == "Diff3D");
}

void WaypointPlanningController::refresh()
{
    bool ready = false, enable = false, offersSetup = false;
    QString status;
    const bool gps = fresh("GPSPosition", 10000) && value("GPSPosition", "Status") != "NoGPS";
    if (m_saving) status = tr("Saving and verifying navigation settings. The controller will restart…");
    else if (!m_connected) status = m_restarting
        ? tr("Reconnecting the controller after restart…") : tr("Connect a controller with GPS to plan waypoints.");
    else if (!fresh("GPSPosition", 10000)) status = m_clock.elapsed() - m_connectedAt < 5000
        ? tr("Waiting for GPS data from this controller…")
        : tr("No current GPS data. Check the receiver, its wiring and GPS port in Configuration → Hardware.");
    else if (!gps) status = tr("Waypoint planning needs an attached GPS. Connect a receiver and configure its port in Configuration → Hardware.");
    else if (!fresh("ModuleSettings") || !fresh("SystemSettings") || !fresh("StateEstimation"))
        status = tr("GPS detected. Checking navigation settings…");
    else if (follower().isEmpty()) status = tr("Waypoint planning is unavailable for this aircraft type. Choose a supported aircraft in Configuration → Vehicle.");
    else if (!fresh("PathPlannerSettings")) status = tr("GPS detected. This firmware has not reported waypoint support. Check the firmware target and navigation modules in Configuration.");
    else {
        const bool configured = value("ModuleSettings", "AdminState", "PathPlanner") == "Enabled"
            && value("ModuleSettings", "AdminState", follower()) == "Enabled"
            && value("StateEstimation", "NavigationFilter") != "None";
        ready = configured && fresh("TaskInfo") && value("TaskInfo", "Running", "PathPlanner") == "TRUE"
            && value("TaskInfo", "Running", "PathFollower") == "TRUE";
        if (ready) {
            m_restarting = false; m_error.clear();
            const auto fix = value("GPSPosition", "Status");
            status = fresh("HomeLocation") && value("HomeLocation", "Set") == "TRUE"
                && (fix == "Fix3D" || fix == "Diff3D") ? tr("Waypoint planning ready.")
                : tr("Waypoint planning ready. A 3D GPS fix and home position are needed to send a route.");
        } else if (m_restarting) status = tr("Controller restarted. Checking GPS and navigation modules…");
        else {
            offersSetup = true;
            enable = fresh("FlightStatus", 10000) && value("FlightStatus", "Armed") == "Disarmed";
            status = configured
                ? tr("Navigation is enabled but its modules are not running. Restart to activate it; a compass and barometer are required.")
                : tr("GPS detected. Enable navigation to plan waypoints. Settings will be saved and the controller restarted; a compass and barometer are required.");
            if (!enable) status += tr(" Disarm the controller to continue.");
        }
    }
    if (!m_error.isEmpty()) status = m_error;
    if (m_ready != ready || m_canEnable != enable || m_offersSetup != offersSetup || m_status != status) {
        m_ready = ready; m_canEnable = enable; m_offersSetup = offersSetup; m_status = status; emit changed();
    }
}

void WaypointPlanningController::enablePlanning()
{
    if (!m_canEnable || m_saving || m_restarting) return;
    QJsonArray settings;
    for (const auto &module : {QString("PathPlanner"), follower()})
        settings.append(QJsonObject{{"object", "ModuleSettings"}, {"field", "AdminState"},
            {"element", module}, {"value", "Enabled"}});
    const QString other = follower() == "VtolPathFollower" ? "FixedWingPathFollower" : "VtolPathFollower";
    settings.append(QJsonObject{{"object", "ModuleSettings"}, {"field", "AdminState"},
        {"element", other}, {"value", "Disabled"}});
    if (value("StateEstimation", "NavigationFilter") == "None")
        settings.append(QJsonObject{{"object", "StateEstimation"}, {"field", "NavigationFilter"},
            {"value", value("StateEstimation", "AttitudeFilter") == "INSOutdoor" ? "INS" : "Raw"}});
    if (m_connection) {
        const auto device = m_connection->getCurrentDevice();
        m_usbPath = device.device ? device.device->usbTopologyPath() : QString();
        m_deviceName = device.device ? device.device->getName() : QString();
        m_connectionName = device.connection ? device.connection->shortName() : QString();
        m_serial = m_connection->controllerSerial();
    }
    m_error.clear(); m_saving = true; m_sawDisconnect = false;
    refresh();
    m_installer->start(settings);
}

void WaypointPlanningController::reconnectController()
{
    if (!m_restarting || !m_sawDisconnect || !m_connection || m_connection->isConnected()) return;
    for (const auto &device : m_connection->getAvailableDevices()) {
        if (!device.device || !device.connection || device.connection->shortName() != m_connectionName) continue;
        if (m_usbPath.isEmpty() ? device.device->getName() != m_deviceName : device.device->usbTopologyPath() != m_usbPath) continue;
        m_connection->connectDevice(device);
        break;
    }
}
