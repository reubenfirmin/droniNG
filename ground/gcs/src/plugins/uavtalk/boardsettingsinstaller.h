/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BOARDSETTINGSINSTALLER_H
#define BOARDSETTINGSINSTALLER_H
#include "uavtalk_global.h"
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QTimer>
class UAVObject;
class UAVObjectManager;

// Applies a catalog profile only during first installation. Each settings object
// is fetched, acknowledged, saved, loaded from flash and checked before reboot.
class UAVTALK_EXPORT BoardSettingsInstaller : public QObject {
    Q_OBJECT
public:
    explicit BoardSettingsInstaller(UAVObjectManager *objects, QObject *parent = nullptr);
    void start(const QJsonArray &settings);
    void disconnected();
    void cancel();
signals:
    void finished(bool success, const QString &message);
private:
    enum class Phase { Idle, Reading, Preparing, Writing, Saving, Loading, Verifying, Rebooting, Restarting };
    void next();
    void unpacked(UAVObject *object);
    void acknowledged(UAVObject *object, bool success);
    void persistence(bool load);
    void reboot();
    void finish(bool success, const QString &message);
    void watch(UAVObject *object);
    bool matches() const;
    UAVObjectManager *m_objects;
    QJsonArray m_settings;
    QList<UAVObject *> m_pending;
    QPointer<UAVObject> m_current;
    QList<QMetaObject::Connection> m_connections;
    QTimer m_timeout, m_rebootDelay;
    Phase m_phase = Phase::Idle;
    int m_rebootStep = 0;
};
#endif
