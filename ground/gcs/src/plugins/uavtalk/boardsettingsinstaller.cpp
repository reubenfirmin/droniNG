/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "boardsettingsinstaller.h"
#include <uavobjects/uavdataobject.h>
#include <uavobjects/uavobjectfield.h>
#include <uavobjects/uavobjectmanager.h>
#include <firmwareiapobj.h>
#include <flightstatus.h>
#include <objectpersistence.h>
#include <QJsonObject>
#include <QSet>
#include <cmath>
#include <limits>

BoardSettingsInstaller::BoardSettingsInstaller(UAVObjectManager *objects, QObject *parent)
    : QObject(parent), m_objects(objects)
{
    m_timeout.setSingleShot(true);
    m_rebootDelay.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        finish(false, tr("Board configuration timed out. Reconnect the controller and retry installation."));
    });
    connect(&m_rebootDelay, &QTimer::timeout, this, [this] {
        if (m_phase == Phase::Restarting)
            finish(true, tr("Board configuration saved and verified. Reconnecting after restart…"));
        else if (m_phase == Phase::Rebooting)
            reboot();
    });
}

void BoardSettingsInstaller::watch(UAVObject *object)
{
    m_connections.append(connect(object, &UAVObject::objectUnpacked, this, &BoardSettingsInstaller::unpacked));
    m_connections.append(connect(object, QOverload<UAVObject *, bool>::of(&UAVObject::transactionCompleted),
                                 this, &BoardSettingsInstaller::acknowledged));
}

void BoardSettingsInstaller::start(const QJsonArray &settings)
{
    if (m_phase != Phase::Idle) return;
    m_settings = settings;
    QSet<QString> seen;
    // Validate the entire profile before sending any settings to the board.
    for (const auto &entry : settings) {
        const auto setting = entry.toObject();
        const QString name = setting.value("object").toString();
        auto *object = qobject_cast<UAVDataObject *>(m_objects->getObject(name));
        auto *field = object ? object->getField(setting.value("field").toString()) : nullptr;
        const int index = !field ? -1 : setting.contains("element")
            ? field->getElementIndex(setting.value("element").toString()) : field->getNumElements() == 1 ? 0 : -1;
        const auto value = setting.value("value");
        const QString key = name + '/' + setting.value("field").toString() + '/' + QString::number(index);
        bool valid = field && index >= 0 && object->isSettings()
            && UAVObject::GetGcsAccess(object->getMetadata()) == UAVObject::ACCESS_READWRITE;
        if (valid && field->getType() == UAVObjectField::ENUM)
            valid = value.isString() && field->getOptions().contains(value.toString());
        else if (valid) {
            valid = value.isDouble() && std::isfinite(value.toDouble());
            const double number = value.toDouble();
            switch (field->getType()) {
            case UAVObjectField::INT8: valid &= number >= -128 && number <= 127; break;
            case UAVObjectField::INT16: valid &= number >= -32768 && number <= 32767; break;
            case UAVObjectField::INT32: valid &= number >= -2147483648.0 && number <= 2147483647.0; break;
            case UAVObjectField::UINT8: valid &= number >= 0 && number <= 255; break;
            case UAVObjectField::UINT16: valid &= number >= 0 && number <= 65535; break;
            case UAVObjectField::UINT32: valid &= number >= 0 && number <= 4294967295.0; break;
            case UAVObjectField::FLOAT32: valid &= std::abs(number) <= std::numeric_limits<float>::max(); break;
            default: valid = false; break;
            }
            if (field->getType() != UAVObjectField::FLOAT32) valid &= number == std::trunc(number);
        }
        if (!valid || seen.contains(key)) {
            finish(false, tr("This build cannot apply the board setting %1.").arg(key)); return;
        }
        seen.insert(key);
        if (!m_pending.contains(object)) m_pending.append(object);
    }
    if (m_pending.isEmpty()) { finish(true, {}); return; }
    for (auto *object : m_pending) watch(object);
    watch(ObjectPersistence::GetInstance(m_objects));
    watch(FirmwareIAPObj::GetInstance(m_objects));
    next();
}

void BoardSettingsInstaller::next()
{
    if (m_pending.isEmpty()) {
        // Reset commands are accepted by firmware only while disarmed. Fetch
        // FlightStatus rather than relying on a default or an earlier session.
        m_current = FlightStatus::GetInstance(m_objects);
        watch(m_current);
    } else m_current = m_pending.first();
    m_phase = Phase::Reading;
    m_timeout.start(5000);
    m_current->requestUpdate();
}

bool BoardSettingsInstaller::matches() const
{
    for (const auto &entry : m_settings) {
        const auto setting = entry.toObject();
        if (setting.value("object").toString() != m_current->getName()) continue;
        auto *field = m_current->getField(setting.value("field").toString());
        const int index = setting.contains("element") ? field->getElementIndex(setting.value("element").toString()) : 0;
        const auto value = setting.value("value");
        const double expected = field->getType() == UAVObjectField::FLOAT32
            ? double(value.toVariant().toFloat()) : value.toDouble();
        if (value.isString() ? field->getValue(index).toString() != value.toString()
                             : field->getDouble(index) != expected) return false;
    }
    return true;
}

void BoardSettingsInstaller::unpacked(UAVObject *object)
{
    if (object == ObjectPersistence::GetInstance(m_objects)
        && (m_phase == Phase::Saving || m_phase == Phase::Loading)) {
        const auto data = ObjectPersistence::GetInstance(m_objects)->getData();
        if (data.ObjectID != m_current->getObjID() || data.InstanceID != m_current->getInstID()) return;
        if (data.Operation == ObjectPersistence::OPERATION_ERROR) {
            finish(false, tr("Could not save the board configuration to the controller.")); return;
        }
        if (data.Operation != ObjectPersistence::OPERATION_COMPLETED) return;
        if (m_phase == Phase::Saving) persistence(true);
        else {
            m_phase = Phase::Verifying;
            m_timeout.start(5000);
            m_current->requestUpdate();
        }
        return;
    }
    if (object != m_current) return;
    if (m_phase == Phase::Verifying) {
        if (!matches()) { finish(false, tr("The controller did not retain the board configuration.")); return; }
        m_pending.removeFirst(); next(); return;
    }
    if (m_phase != Phase::Reading) return;
    if (object == FlightStatus::GetInstance(m_objects)) {
        if (FlightStatus::GetInstance(m_objects)->getData().Armed != FlightStatus::ARMED_DISARMED) {
            finish(false, tr("Disarm the controller before restarting it.")); return;
        }
        m_phase = Phase::Rebooting; m_rebootStep = 0; reboot(); return;
    }
    // The read transaction completes after objectUnpacked. Defer the write so
    // its acknowledgement cannot be confused with completion of that read.
    m_phase = Phase::Preparing;
    QTimer::singleShot(0, this, [this] {
        if (m_phase != Phase::Preparing) return;
        for (const auto &entry : m_settings) {
            const auto setting = entry.toObject();
            if (setting.value("object").toString() != m_current->getName()) continue;
            auto *field = m_current->getField(setting.value("field").toString());
            const int index = setting.contains("element") ? field->getElementIndex(setting.value("element").toString()) : 0;
            field->setValue(setting.value("value").toVariant(), index);
        }
        m_phase = Phase::Writing; m_timeout.start(5000);
        m_current->updated();
    });
}

void BoardSettingsInstaller::acknowledged(UAVObject *object, bool success)
{
    if (m_phase == Phase::Writing && object == m_current) {
        if (!success) { finish(false, tr("The controller rejected the board configuration.")); return; }
        persistence(false);
    } else if (m_phase == Phase::Rebooting && object == FirmwareIAPObj::GetInstance(m_objects)) {
        if (!success) { finish(false, tr("The controller could not restart. Reconnect USB to activate its board configuration.")); return; }
        ++m_rebootStep;
        if (m_rebootStep == 3) m_phase = Phase::Restarting;
        // Firmware requires 500–5000 ms between commands, then resets after 500 ms.
        m_rebootDelay.start(700);
    }
}

void BoardSettingsInstaller::persistence(bool load)
{
    auto *object = ObjectPersistence::GetInstance(m_objects);
    auto data = object->getData();
    data.ObjectID = m_current->getObjID(); data.InstanceID = m_current->getInstID();
    data.Operation = load ? ObjectPersistence::OPERATION_LOAD : ObjectPersistence::OPERATION_SAVE;
    m_phase = load ? Phase::Loading : Phase::Saving;
    m_timeout.start(5000);
    object->setData(data); object->updated();
}

void BoardSettingsInstaller::reboot()
{
    auto *object = FirmwareIAPObj::GetInstance(m_objects);
    auto data = object->getData();
    static const quint16 commands[] = {1122, 2233, 4455}; // Native restart, without entering the uploader.
    data.Command = commands[m_rebootStep];
    m_timeout.start(5000);
    object->setData(data); object->updated();
}

void BoardSettingsInstaller::finish(bool success, const QString &message)
{
    cancel(); emit finished(success, message);
}
void BoardSettingsInstaller::cancel()
{
    m_timeout.stop(); m_rebootDelay.stop();
    for (const auto &connection : m_connections) disconnect(connection);
    m_connections.clear(); m_pending.clear(); m_current.clear(); m_phase = Phase::Idle;
}
void BoardSettingsInstaller::disconnected()
{
    if (m_phase != Phase::Idle && m_phase != Phase::Restarting)
        finish(false, tr("The controller disconnected before board configuration finished. Reconnect it and retry installation."));
}
