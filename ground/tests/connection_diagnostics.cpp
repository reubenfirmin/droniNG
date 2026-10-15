/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <coreplugin/connectionmanager.h>
#include <coreplugin/icore.h>
#include <coreplugin/modemanager.h>
#include <coreplugin/imode.h>
#include <coreplugin/iuavgadget.h>
#include <coreplugin/iuavgadgetfactory.h>
#include <coreplugin/firmwarecatalog.h>
#include <coreplugin/firmwareinstallimage.h>
#include <coreplugin/iboardtype.h>
#include <coreplugin/firmwarepage.h>
#include <coreplugin/firmwareinstallwidget.h>
#include <coreplugin/dfuinstaller.h>
#include <coreplugin/usbaccesssetup.h>
#include <coreplugin/workspacelayout.h>
#include <extensionsystem/iplugin.h>
#include <extensionsystem/pluginmanager.h>
#include <serialconnection/serialplugin.h>
#include <rawhid/rawhidplugin.h>
#include <rawhid/rawhid.h>
#include <uavobjects/uavobjectsplugin.h>
#include <uavtalk/telemetry.h>
#include <uavtalk/telemetrymonitor.h>
#include <uavtalk/telemetrymanager.h>
#include <uavobjectutil/uavobjectutilmanager.h>
#include <uploader/uploadergadgetwidget.h>
#include <uavtalk/boardsettingsinstaller.h>
#include <pathplanner/waypointplanningcontroller.h>
#include <pathplanner/flightdatamodel.h>
#include <pathplanner/pathplannergadgetwidget.h>
#include <opmap/opmapgadgetwidget.h>
#include <modulesettings.h>
#include <systemsettings.h>
#include <stateestimation.h>
#include <taskinfo.h>
#include <homelocation.h>
#include <pathplannersettings.h>
#include <uavobjects/uavobjectfield.h>
#include <uavobjects/uavdataobject.h>
#include <objectpersistence.h>
#include <firmwareiapobj.h>
#include <flightstatus.h>
#include <attitudesettings.h>
#include <actuatorsettings.h>
#include <hwrevolution.h>
#include <hwimported.h>
#include <boardprofile.h>
#include <utils/pathutils.h>
#include <utils/darktheme.h>
#include <utils/mytabbedstackwidget.h>
#include <tlmapcontrol/internals/core.h>
#include <attitudeactual.h>
#include <flighttelemetrystats.h>
#include <systemalarms.h>
#include <systemstats.h>
#include <baroaltitude.h>
#include <gpsposition.h>
#include <QPluginLoader>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QGraphicsView>
#include <QGridLayout>
#include <QLineEdit>
#include <QClipboard>
#include <QDialog>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QToolButton>
#include <QTableView>
#include <QTabWidget>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QMouseEvent>
#include <QWizard>
#include <cstring>
#include <cmath>
#include <memory>
#include <functional>
#include <algorithm>
#include <atomic>
#include <cerrno>

Q_DECLARE_METATYPE(QFile::Permissions)

class MessageCapture
{
    static MessageCapture *active;
    QtMessageHandler previous;
    static void capture(QtMsgType type, const QMessageLogContext &, const QString &message)
    {
        // Offscreen CI cannot propagate native window size hints. Keep that
        // platform limitation out of assertions about product diagnostics.
        if (QApplication::platformName() == "offscreen"
            && message == "This plugin does not support propagateSizeHints()")
            return;
        if (type == QtWarningMsg)
            active->warnings.append(message);
        if (type == QtInfoMsg)
            active->information.append(message);
    }
public:
    QStringList warnings, information;
    MessageCapture() : previous(qInstallMessageHandler(capture)) { active = this; }
    ~MessageCapture() { qInstallMessageHandler(previous); active = nullptr; }
};
MessageCapture *MessageCapture::active = nullptr;

class ProfileBoard : public QObject {
public:
    QMap<quint32, QByteArray> remote, initial, saved;
    QList<int> rebootCommands;
    int writes = 0;
    bool rejectWrite = false, saveError = false, corruptSave = false;
    explicit ProfileBoard(UAVObjectManager *objects) {
        for (const auto &name : {"AttitudeSettings", "HwRevolution", "FlightStatus", "FirmwareIAPObj", "ObjectPersistence",
                                "ModuleSettings", "SystemSettings", "StateEstimation", "GPSPosition", "TaskInfo", "HomeLocation", "PathPlannerSettings"}) {
            auto *object = objects->getObject(name);
            QByteArray bytes(object->getNumBytes(), 0); object->pack(reinterpret_cast<quint8 *>(bytes.data()));
            remote[object->getObjID()] = bytes;
            connect(object, &UAVObject::updateRequested, this, [this, object] {
                QTimer::singleShot(0, this, [this, object] {
                    object->unpack(reinterpret_cast<const quint8 *>(remote[object->getObjID()].constData()));
                    object->emitTransactionCompleted(true);
                });
            });
            connect(object, &UAVObject::objectUpdatedManual, this, [this, object] {
                QTimer::singleShot(0, this, [this, object] {
                    if (object->getName() == "ObjectPersistence") {
                        auto *persistence = static_cast<ObjectPersistence *>(object);
                        auto data = persistence->getData();
                        if (data.Operation == ObjectPersistence::OPERATION_SAVE)
                            saved[data.ObjectID] = corruptSave ? initial[data.ObjectID] : remote[data.ObjectID];
                        else if (data.Operation == ObjectPersistence::OPERATION_LOAD)
                            remote[data.ObjectID] = saved[data.ObjectID];
                        data.Operation = saveError ? ObjectPersistence::OPERATION_ERROR : ObjectPersistence::OPERATION_COMPLETED;
                        persistence->setData(data);
                        QByteArray response(object->getNumBytes(), 0);
                        object->pack(reinterpret_cast<quint8 *>(response.data()));
                        object->emitTransactionCompleted(true);
                        object->unpack(reinterpret_cast<const quint8 *>(response.constData()));
                    } else if (object->getName() == "FirmwareIAPObj") {
                        rebootCommands.append(static_cast<FirmwareIAPObj *>(object)->getData().Command);
                        object->emitTransactionCompleted(true);
                    } else {
                        ++writes;
                        QByteArray bytes(object->getNumBytes(), 0);
                        object->pack(reinterpret_cast<quint8 *>(bytes.data()));
                        if (!rejectWrite) remote[object->getObjID()] = bytes;
                        object->emitTransactionCompleted(!rejectWrite);
                    }
                });
            });
        }
        initial = remote;
    }
};

class TestDevice : public QIODevice
{
    QByteArray incoming;
public:
    bool failOpen = false, failWrite = false;
    QByteArray written;
    std::function<void(const QByteArray &)> onWrite;
    int openCalls = 0;
    explicit TestDevice(QObject *parent = nullptr) : QIODevice(parent) {}
    bool open(OpenMode mode) override
    {
        ++openCalls;
        if (failOpen) {
            setErrorString("simulated permission denied");
            return false;
        }
        return QIODevice::open(mode);
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return incoming.size() + QIODevice::bytesAvailable(); }
    void receive(const QByteArray &bytes) { incoming.append(bytes); emit readyRead(); }
protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 size = qMin(maxSize, qint64(incoming.size()));
        std::memcpy(data, incoming.constData(), size);
        incoming.remove(0, size);
        return size;
    }
    qint64 writeData(const char *data, qint64 size) override
    {
        if (failWrite) {
            setErrorString("simulated write failure");
            return -1;
        }
        const QByteArray bytes(data, size);
        written += bytes;
        if (onWrite)
            QTimer::singleShot(0, this, [this, bytes] { onWrite(bytes); });
        return size;
    }
};

class TestConnection : public Core::IConnection
{
    QList<Core::IDevice *> devices;
public:
    Core::IDevice *device;
    QPointer<TestDevice> io;
    bool openInPlugin = false, failOpen = false, returnNull = false;
    QString errorDetails = "simulated serial open failure";
    QString transport = "Serial";
    Core::ConnectionError failure;
    int closeCalls = 0;
    explicit TestConnection(const QString &displayName = "test controller", const QString &usbPath = {})
    {
        device = new Core::IDevice;
        device->setParent(this);
        device->setName("test-serial-port");
        device->setDisplayName(displayName);
        device->setUsbTopologyPath(usbPath);
        device->setControllerCandidate(true);
        devices.append(device);
        ExtensionSystem::PluginManager::instance()->addObject(this);
    }
    ~TestConnection() override
    {
        devices.clear();
        emit availableDevChanged(this);
        ExtensionSystem::PluginManager::instance()->removeObject(this);
    }
    QList<Core::IDevice *> availableDevices() override { return devices; }
    void detachController()
    {
        devices.removeAll(device);
        emit availableDevChanged(this);
    }
    void attachController(bool candidate = true)
    {
        auto *attached = new Core::IDevice;
        attached->setParent(this);
        attached->setName("new-serial-port");
        attached->setDisplayName(candidate ? "New controller" : "USB serial adapter");
        attached->setUsbTopologyPath("1-3");
        attached->setControllerCandidate(candidate);
        devices.append(attached);
        emit availableDevChanged(this);
    }
    QString connectionName() override { return transport; }
    QString errorString() const override { return errorDetails; }
    Core::ConnectionError error() const override
    {
        return failure.message.isEmpty() ? IConnection::error() : failure;
    }
    QIODevice *openDevice(Core::IDevice *) override
    {
        if (returnNull)
            return nullptr;
        io = new TestDevice(this);
        io->failOpen = failOpen;
        if (openInPlugin)
            io->open(QIODevice::ReadWrite);
        return io;
    }
    void closeDevice(const QString &) override
    {
        ++closeCalls;
        if (io) {
            io->close();
            io->deleteLater();
            io = nullptr;
        }
    }
};

class EnumeratedSerialConnection : public SerialConnection
{
public:
    QList<Port> connectedPorts;
    QPointer<TestDevice> io;
    int closeCalls = 0;
    EnumeratedSerialConnection() { ExtensionSystem::PluginManager::instance()->addObject(this); }
    ~EnumeratedSerialConnection() override { ExtensionSystem::PluginManager::instance()->removeObject(this); }
    void attach(const QString &description = "OmnibusF4", quint16 vendor = 0x0483, quint16 product = 0x5740)
    { connectedPorts = {{"ttyACM0", "/dev/ttyACM0", description, "controller-serial", "1-3", vendor, product}}; }
    QIODevice *openDevice(Core::IDevice *) override {
        io = new TestDevice(this); io->open(QIODevice::ReadWrite); m_deviceOpened = true; return io;
    }
    void closeDevice(const QString &) override {
        ++closeCalls; m_deviceOpened = false;
        if (io) { io->close(); io->deleteLater(); io.clear(); }
    }
protected:
    QList<Port> ports() const override { return connectedPorts; }
};

class HidTestInventory : public RawHIDConnection
{
public:
    USBDevice *device;
    HidTestInventory() {
        USBPortInfo port{};
        port.path = "/dev/test-hid"; port.serialNumber = "test-hid";
        device = static_cast<USBDevice *>(devicesForPorts({port}).first());
    }
};

// Independent wire framing feeds the actual UAVTalk parser, not the monitor's state directly.
static QByteArray objectFrame(UAVObject *object)
{
    QByteArray frame(8 + object->getNumBytes(), char(0));
    frame[0] = char(0x3c);
    frame[1] = char(0x20);
    frame[2] = char(frame.size() & 0xff);
    frame[3] = char(frame.size() >> 8);
    for (int byte = 0; byte < 4; ++byte)
        frame[4 + byte] = char(object->getObjID() >> (8 * byte));
    object->pack(reinterpret_cast<quint8 *>(frame.data() + 8));
    quint8 crc = 0;
    for (char byte : frame) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? quint8((crc << 1) ^ 0x07) : quint8(crc << 1);
    }
    frame.append(char(crc));
    return frame;
}

static QByteArray mspFrame(quint8 command, const QByteArray &payload)
{
    QByteArray frame("$M>", 3);
    frame.append(char(payload.size()));
    frame.append(char(command));
    frame += payload;
    quint8 checksum = 0;
    for (int i = 3; i < frame.size(); ++i)
        checksum ^= quint8(frame[i]);
    frame.append(char(checksum));
    return frame;
}

static QByteArray nackFrame(quint32 id)
{
    QByteArray frame = QByteArray::fromHex("3c24080000000000");
    for (int byte = 0; byte < 4; ++byte) frame[4 + byte] = char(id >> (8 * byte));
    quint8 crc = 0;
    for (char byte : frame) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? quint8((crc << 1) ^ 0x07) : quint8(crc << 1);
    }
    return frame + char(crc);
}

class ConnectionDiagnosticsTests : public QObject
{
    Q_OBJECT
    QTemporaryDir settingsDirectory;
    ExtensionSystem::PluginManager plugins;
    UAVObjectsPlugin uavObjects;
    QPluginLoader core{QString::fromUtf8(CORE_PLUGIN_FILE)};
    Core::ConnectionManager *manager = nullptr;
    Core::FirmwarePage *page = nullptr;
    UAVObjectManager *objects = nullptr;

    void captureFirmwarePage(const QString &name)
    {
        const auto directory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (directory.isEmpty()) return;
        const auto previous = page->size();
        page->resize(1100, 740); QCoreApplication::processEvents();
        QVERIFY(page->grab().save(directory + "/" + name + ".png"));
        page->resize(previous); QCoreApplication::processEvents();
    }

private slots:
    void waypointEligibilityUsesCurrentGpsAndRunningModules()
    {
        WaypointPlanningController planning(objects, nullptr);
        const auto receive = [this](const QString &name) {
            auto *object = objects->getObject(name);
            QByteArray bytes(object->getNumBytes(), 0);
            object->pack(reinterpret_cast<quint8 *>(bytes.data()));
            object->unpack(reinterpret_cast<const quint8 *>(bytes.constData()));
        };
        const auto set = [this, &receive](const QString &object, const QString &field, const QString &value, const QString &element = {}) {
            auto *f = objects->getObject(object)->getField(field);
            f->setValue(value, element.isEmpty() ? 0 : f->getElementIndex(element));
            receive(object);
        };
        planning.setConnected(true);
        QVERIFY(!planning.ready());
        set("GPSPosition", "Status", "NoGPS");
        QVERIFY(!planning.canEnable());
        QVERIFY(planning.status().contains("attached GPS"));
        set("SystemSettings", "AirframeType", "QuadX");
        set("StateEstimation", "NavigationFilter", "None");
        set("ModuleSettings", "AdminState", "Disabled", "PathPlanner");
        set("ModuleSettings", "AdminState", "Disabled", "VtolPathFollower");
        set("FlightStatus", "Armed", "Disarmed");
        receive("PathPlannerSettings");
        // An attached receiver with no fix is enough to offer setup.
        set("GPSPosition", "Status", "NoFix");
        QVERIFY(planning.offersSetup());
        QVERIFY(planning.canEnable());
        QVERIFY(!planning.ready());
        set("FlightStatus", "Armed", "Armed");
        QVERIFY(planning.offersSetup());
        QVERIFY(!planning.canEnable());
        set("FlightStatus", "Armed", "Disarmed");
        set("StateEstimation", "NavigationFilter", "Raw");
        set("ModuleSettings", "AdminState", "Enabled", "PathPlanner");
        set("ModuleSettings", "AdminState", "Enabled", "VtolPathFollower");
        set("TaskInfo", "Running", "FALSE", "PathPlanner");
        set("TaskInfo", "Running", "FALSE", "PathFollower");
        QVERIFY(!planning.ready()); // Saved settings do not prove modules started.
        set("TaskInfo", "Running", "TRUE", "PathPlanner");
        set("TaskInfo", "Running", "TRUE", "PathFollower");
        QVERIFY(planning.ready());
        QVERIFY(!planning.canUpload());
        set("HomeLocation", "Set", "TRUE");
        set("GPSPosition", "Status", "Fix3D");
        QVERIFY(planning.canUpload());
        set("GPSPosition", "Status", "NoGPS");
        QVERIFY(!planning.ready());
        QVERIFY(!planning.canUpload());
        set("GPSPosition", "Status", "Fix3D");
        QVERIFY(planning.ready());
        planning.setConnected(false);
        planning.setConnected(true);
        QVERIFY(!planning.ready()); // A new session cannot inherit the previous receiver.
        QVERIFY(!planning.canEnable());
        receive("GPSPosition");
        QVERIFY(!planning.ready()); // Fresh GPS alone cannot inherit old module state.
    }

    void waypointSetupPersistsVerifiesAndRestarts_data()
    {
        QTest::addColumn<QString>("airframe");
        QTest::addColumn<bool>("failSave");
        QTest::addColumn<bool>("armedBeforeWrite");
        QTest::newRow("multirotor") << QString("QuadX") << false << false;
        QTest::newRow("fixed-wing") << QString("FixedWing") << false << false;
        QTest::newRow("save-failure") << QString("QuadX") << true << false;
        QTest::newRow("armed-before-write") << QString("QuadX") << false << true;
    }
    void waypointSetupPersistsVerifiesAndRestarts()
    {
        QFETCH(QString, airframe);
        QFETCH(bool, failSave);
        QFETCH(bool, armedBeforeWrite);
        const auto set = [this](const QString &object, const QString &field, const QString &value, const QString &element = {}) {
            auto *f = objects->getObject(object)->getField(field);
            f->setValue(value, element.isEmpty() ? 0 : f->getElementIndex(element));
        };
        set("SystemSettings", "AirframeType", airframe);
        set("GPSPosition", "Status", "NoFix");
        set("FlightStatus", "Armed", "Disarmed");
        set("StateEstimation", "NavigationFilter", "None");
        set("StateEstimation", "AttitudeFilter", "Complementary");
        set("ModuleSettings", "AdminState", "Disabled", "PathPlanner");
        set("ModuleSettings", "AdminState", "Disabled", "VtolPathFollower");
        set("ModuleSettings", "AdminState", "Disabled", "FixedWingPathFollower");
        set("TaskInfo", "Running", "FALSE", "PathPlanner");
        set("TaskInfo", "Running", "FALSE", "PathFollower");
        ProfileBoard board(objects);
        board.saveError = failSave;
        WaypointPlanningController planning(objects, nullptr);
        planning.setConnected(true);
        QTRY_VERIFY(planning.canEnable());
        if (armedBeforeWrite) {
            set("FlightStatus", "Armed", "Armed");
            auto *flight = objects->getObject("FlightStatus");
            flight->pack(reinterpret_cast<quint8 *>(board.remote[flight->getObjID()].data()));
            // The UI's last received status is disarmed; the next read detects arming.
            set("FlightStatus", "Armed", "Disarmed");
        }
        planning.enablePlanning();
        QVERIFY(!planning.ready());
        if (armedBeforeWrite) {
            QTRY_VERIFY(planning.status().contains("Disarm"));
            QCOMPARE(board.writes, 0);
            QVERIFY(board.saved.isEmpty());
            QVERIFY(board.rebootCommands.isEmpty());
        } else if (failSave) {
            QTRY_VERIFY(planning.status().contains("did not finish"));
            QCOMPARE(board.rebootCommands.size(), 0);
            QVERIFY(!planning.ready());
            QVERIFY(planning.canEnable());
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(board.rebootCommands.size(), 3, 5000);
            QCOMPARE(board.rebootCommands, QList<int>({1122, 2233, 4455}));
            QVERIFY(board.saved.contains(ModuleSettings::OBJID));
            QVERIFY(board.saved.contains(StateEstimation::OBJID));
            QCOMPARE(objects->getObject("StateEstimation")->getField("NavigationFilter")->getValue().toString(), QString("Raw"));
            const QString follower = airframe == "FixedWing" ? "FixedWingPathFollower" : "VtolPathFollower";
            auto *admin = objects->getObject("ModuleSettings")->getField("AdminState");
            QCOMPARE(admin->getValue(admin->getElementIndex(follower)).toString(), QString("Enabled"));
            QVERIFY(!planning.ready());
            // Simulate the restart and its fresh runtime report.
            QTest::qWait(750);
            planning.setConnected(false);
            planning.setConnected(true);
            set("TaskInfo", "Running", "TRUE", "PathPlanner");
            set("TaskInfo", "Running", "TRUE", "PathFollower");
            auto *task = objects->getObject("TaskInfo");
            task->pack(reinterpret_cast<quint8 *>(board.remote[task->getObjID()].data()));
            QTRY_VERIFY(planning.ready());
        }
    }

    void mapWaypointControlsFollowGpsAndModuleState()
    {
        const auto set = [this](const QString &object, const QString &field, const QVariant &value, const QString &element = {}) {
            auto *obj = objects->getObject(object);
            auto *f = obj->getField(field);
            f->setValue(value, element.isEmpty() ? 0 : f->getElementIndex(element));
            QByteArray bytes(obj->getNumBytes(), 0);
            obj->pack(reinterpret_cast<quint8 *>(bytes.data()));
            obj->unpack(reinterpret_cast<const quint8 *>(bytes.constData()));
        };
        set("HomeLocation", "Set", "FALSE");
        WaypointPlanningController planning(objects, nullptr);
        FlightDataModel model(nullptr);
        QItemSelectionModel selection(&model);
        plugins.addObject(&planning); plugins.addObject(&model); plugins.addObject(&selection);
        const auto remove = qScopeGuard([&] {
            plugins.removeObject(&selection); plugins.removeObject(&model); plugins.removeObject(&planning);
        });
        Core::ICore::instance()->settings()->setValue("Map/ApproximateLatitude", 9.3);
        Core::ICore::instance()->settings()->setValue("Map/ApproximateLongitude", -83.8);
        Core::ICore::instance()->settings()->setValue("Map/ApproximateLocationUpdated", QDateTime::currentSecsSinceEpoch());
        OPMapGadgetWidget map;
        map.resize(1100, 740); map.show();
        auto *plan = map.findChild<QPushButton *>("planRouteButton");
        auto *enable = map.findChild<QPushButton *>("enableWaypointPlanning");
        QVERIFY(plan); QVERIFY(enable);
        QVERIFY(!plan->isEnabled());
        QVERIFY(enable->isHidden());
        planning.setConnected(true);
        set("SystemSettings", "AirframeType", "QuadX");
        set("StateEstimation", "NavigationFilter", "None");
        set("ModuleSettings", "AdminState", "Disabled", "PathPlanner");
        set("ModuleSettings", "AdminState", "Disabled", "VtolPathFollower");
        set("FlightStatus", "Armed", "Disarmed");
        set("PathPlannerSettings", "PreprogrammedPath", "NONE");
        set("GPSPosition", "Status", "NoFix");
        QVERIFY(!plan->isEnabled());
        QVERIFY(enable->isVisible()); QVERIFY(enable->isEnabled());
        const auto capture = [&](const QString &name) {
            const auto directory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
            if (!directory.isEmpty()) {
                QCoreApplication::processEvents();
                QVERIFY(map.grab().save(directory + '/' + name + ".png"));
            }
        };
        capture("map-gps-enable-navigation");
        set("StateEstimation", "NavigationFilter", "Raw");
        set("ModuleSettings", "AdminState", "Enabled", "PathPlanner");
        set("ModuleSettings", "AdminState", "Enabled", "VtolPathFollower");
        set("TaskInfo", "Running", "TRUE", "PathPlanner");
        set("TaskInfo", "Running", "TRUE", "PathFollower");
        QVERIFY(plan->isEnabled()); QVERIFY(enable->isHidden());
        QTest::mouseClick(plan, Qt::LeftButton);
        auto *table = map.findChild<QTableView *>("tableView");
        auto *add = map.findChild<QToolButton *>("tbAdd");
        QVERIFY(table); QVERIFY(table->isEnabled()); QVERIFY(add->isEnabled());
        QTest::mouseClick(add, Qt::LeftButton);
        QCOMPARE(model.rowCount(), 1);
        auto *view = map.findChild<mapcontrol::TLMapWidget *>("localMapView");
        QVERIFY(view);
        auto *waypoint = view->WPFind(0);
        QVERIFY(waypoint);
        QVERIFY(waypoint->flags().testFlag(QGraphicsItem::ItemIsMovable));
        const double longitude = model.index(0, FlightDataModel::LNGPOSITION).data().toDouble();
        QCoreApplication::processEvents();
        const QPoint grab = view->mapFromScene(waypoint->scenePos()) - QPoint(0, 12);
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, grab);
        for (int step = 1; step <= 5; ++step) {
            const QPoint pos = grab + QPoint(step * 10, step * 10);
            QMouseEvent move(QEvent::MouseMove, pos, view->viewport()->mapToGlobal(pos),
                Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &move);
        }
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, grab + QPoint(50, 50));
        QVERIFY(model.index(0, FlightDataModel::LNGPOSITION).data().toDouble() != longitude);
        capture("map-waypoint-planning-ready");
        map.resize(840, 740); capture("map-waypoint-planning-ready-narrow");
        model.setData(model.index(0, FlightDataModel::LOCKED), true);
        set("GPSPosition", "Status", "NoGPS");
        QVERIFY(!plan->isEnabled()); QVERIFY(!table->isEnabled()); QVERIFY(!add->isEnabled());
        QVERIFY(!waypoint->flags().testFlag(QGraphicsItem::ItemIsMovable));
        QMetaObject::invokeMethod(add, "clicked");
        QCOMPARE(model.rowCount(), 1);
        capture("map-waypoint-gps-lost");
        set("GPSPosition", "Status", "NoFix");
        QVERIFY(plan->isEnabled()); QVERIFY(table->isEnabled());
        QVERIFY(!waypoint->flags().testFlag(QGraphicsItem::ItemIsMovable));
    }

    void initTestCase()
    {
        Utils::applyDarkTheme();
        qRegisterMetaType<Core::IMode *>();
        const auto captureDirectory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captureDirectory.isEmpty()) QVERIFY(QDir().mkpath(captureDirectory));
        QVERIFY(settingsDirectory.isValid());
        Utils::PathUtils().setSettingsFilename(settingsDirectory.filePath("settings.xml"));
        auto *plugin = qobject_cast<ExtensionSystem::IPlugin *>(core.instance());
        QVERIFY2(plugin, qPrintable(core.errorString()));
        QString error;
        QVERIFY2(plugin->initialize({}, &error), qPrintable(error));
        plugins.loadPlugins();
        QVERIFY(plugins.allPluginsLoaded());
        QVERIFY(uavObjects.initialize({}, &error));
        objects = plugins.getObject<UAVObjectManager>();
        QVERIFY(objects);
        manager = Core::ICore::instance()->connectionManager();
        QVERIFY(manager);
        page = new Core::FirmwarePage(manager, manager->window());
        page->resize(650, 500);
        manager->window()->show();
        page->show();
    }
    void cleanup()
    {
        manager->disconnectDevice();
        manager->resetAutomaticConnectionFailures(manager->controllerRecovery().usbPath);
        manager->clearControllerRecovery();
        manager->clearVerifiedInstallation();
        page->findChild<QToolButton *>("connectionErrorDetailsToggle")->setChecked(false);
        QVERIFY(!manager->window()->findChild<QDialog *>("connectionErrorDialog"));
        QVERIFY(!manager->window()->findChild<QDialog *>("telemetryErrorDialog"));
        QVERIFY(!manager->window()->findChild<QDialog *>("firmwareCatalogDialog"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void flightWorkspaceMigrationPreservesMapAndInstrumentSettings()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("profile.ini"), QSettings::IniFormat);
        settings.setValue("Workspace/NumberOfWorkspaces", 2);
        settings.setValue("Workspace/Workspace1", "Flight data");
        settings.setValue("Workspace/Workspace2", "Firmware");
        const QString flight = "UAVGadgetManager/Mode1/splitter/";
        settings.setValue(flight + "side0/classId", "OPMapGadget");
        settings.setValue(flight + "side0/gadget/activeConfiguration", "My GPS map");
        settings.setValue(flight + "side0/gadget/customMapState", "saved position");
        settings.setValue(flight + "side1/classId", "ModelViewGadget");
        settings.setValue(flight + "side1/gadget/activeConfiguration", "My aircraft");
        const QString firmware = "UAVGadgetManager/Mode2/splitter/";
        settings.setValue(firmware + "side0/classId", "Uploader");
        settings.setValue(firmware + "side1/classId", "SystemHealthGadget");
        Core::Internal::migrateFlightWorkspace(&settings);
        QCOMPARE(settings.value("Workspace/Workspace1").toString(), QString("Map"));
        QCOMPARE(settings.value(flight + "classId").toString(), QString("OPMapGadget"));
        QCOMPARE(settings.value(flight + "gadget/activeConfiguration").toString(), QString("My GPS map"));
        QCOMPARE(settings.value(flight + "gadget/customMapState").toString(), QString("saved position"));
        QCOMPARE(settings.value("FlightDashboard/ModelViewGadget/configuration").toString(), QString("My aircraft"));
        QCOMPARE(settings.value(firmware + "classId").toString(), QString("Uploader"));
        QVERIFY(!settings.contains(firmware + "side1/classId"));
        const auto keys = settings.allKeys();
        Core::Internal::migrateFlightWorkspace(&settings);
        QCOMPARE(settings.allKeys(), keys);
    }
    void flightWorkspaceMigrationAddsMapToBasicLayouts()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("profile.ini"), QSettings::IniFormat);
        settings.setValue("Workspace/NumberOfWorkspaces", 1);
        settings.setValue("Workspace/Workspace1", "Flight data");
        settings.setValue("UAVGadgetManager/Mode1/splitter/classId", "PfdQmlGadget");
        settings.setValue("UAVGadgetManager/Mode1/splitter/gadget/activeConfiguration", "NoTerrain");
        Core::Internal::migrateFlightWorkspace(&settings);
        QCOMPARE(settings.value("Workspace/Workspace1").toString(), QString("Map"));
        QCOMPARE(settings.value("UAVGadgetManager/Mode1/splitter/classId").toString(), QString("OPMapGadget"));
        QCOMPARE(settings.value("FlightDashboard/PfdQmlGadget/configuration").toString(), QString("NoTerrain"));
    }
    void flightPanelsFollowLiveFirmware()
    {
        Core::FirmwarePage dashboard(manager);
        const QStringList names{"firmwareAttitudeCard", "firmwareModelCard", "firmwareHealthCard"};
        for (int index = 0; index < names.size(); ++index)
            dashboard.setFlightPanel(index, new QWidget(&dashboard));
        const auto check = [&dashboard, &names](bool available) {
            for (const auto &name : names) QCOMPARE(dashboard.findChild<QWidget *>(name)->isEnabled(), available);
        };
        check(false);
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->reportFlightDataReceived();
        check(false);
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG 0.1.0");
        check(true);
        manager->telemetryDisconnected();
        check(false);
        manager->reportFlightDataReceived();
        check(true);
        manager->beginFirmwareInstallation();
        check(false);
        manager->endFirmwareInstallation();
        manager->disconnectDevice();
        check(false);
    }
    void firmwareGridReflowsWithWindowAndExpandedCards()
    {
        Core::FirmwarePage dashboard(manager);
        dashboard.resize(1100, 900); dashboard.show();
        auto *overview = dashboard.findChild<QWidget *>("firmwareOverview");
        auto *attitude = dashboard.findChild<QWidget *>("firmwareAttitudeCard");
        auto *aircraft = dashboard.findChild<QWidget *>("firmwareModelCard");
        auto *recovery = dashboard.findChild<QWidget *>("firmwareRecoveryCard");
        auto *log = dashboard.findChild<QWidget *>("firmwareLogCard");
        auto *title = dashboard.findChild<QLabel *>("firmwarePageTitle");
        QVERIFY(overview->isAncestorOf(title));
        QTRY_VERIFY(attitude->x() > overview->x());
        const int originalWidth = attitude->width();
        dashboard.resize(1320, 950);
        QTRY_VERIFY(attitude->width() > originalWidth + 70);
        QCOMPARE(aircraft->width(), attitude->width());
        QVERIFY(recovery->y() >= aircraft->y() + aircraft->height());
        QCOMPARE(recovery->x(), aircraft->x()); QCOMPARE(log->x(), attitude->x());
        dashboard.findChild<QToolButton *>("firmwareRecoveryToolsToggle")->setChecked(true);
        dashboard.findChild<QToolButton *>("connectionErrorDetailsToggle")->setChecked(true);
        QTRY_VERIFY(dashboard.findChild<QWidget *>("firmwareRecoveryBody")->isVisible());
        QVERIFY(dashboard.findChild<QPlainTextEdit *>("connectionErrorDetails")->isVisible());
        dashboard.resize(680, 740);
        QTRY_COMPARE(attitude->x(), overview->x());
        QTRY_VERIFY(attitude->width() > originalWidth);
        QCOMPARE(recovery->x(), overview->x()); QCOMPARE(log->x(), overview->x());
        QVERIFY(log->y() >= recovery->y() + recovery->height());
        dashboard.resize(1100, 900);
        QTRY_VERIFY(attitude->x() > overview->x());
        QCOMPARE(log->x(), overview->x());
        dashboard.findChild<QToolButton *>("firmwareRecoveryToolsToggle")->setChecked(false);
        QCoreApplication::processEvents();
        QCOMPARE(log->x(), attitude->x());
    }
    void mapOnlyRequestsTilesVisibleAtFractionalZoom()
    {
        for (const auto &size : {QSize(1000, 600), QSize(600, 1000), QSize(1800, 1000)}) {
            for (const double scale : {1.0, 1.5, 1.95}) {
                internals::Core map;
                map.OnMapSizeChanged(size.width(), size.height()); map.SetZoom(10);
                map.SetcenterTileXYLocation(core::Point(500, 500));
                map.SetrenderOffset(core::Point(size.width() / 2 - 500 * 256 - 128,
                                               size.height() / 2 - 500 * 256 - 128));
                map.SetViewScale(scale);
                QList<core::Point> tiles; map.FindTilesAround(tiles);
                const int maximum = (int(std::ceil(size.width() / (256 * scale))) + 1)
                    * (int(std::ceil(size.height() / (256 * scale))) + 1);
                QVERIFY(tiles.size() <= maximum);
                // Independently sample the final, scaled viewport, including
                // its four edges. Every sample must have a requested tile.
                for (int x = 0; x < size.width(); x += 19)
                    for (int y = 0; y < size.height(); y += 19) {
                        const int globalX = int(std::floor((x - size.width() / 2.0) / scale + 500 * 256 + 128));
                        const int globalY = int(std::floor((y - size.height() / 2.0) / scale + 500 * 256 + 128));
                        QVERIFY(tiles.contains(core::Point(globalX / 256, globalY / 256)));
                    }
            }
        }
    }
    void mapDownloadsInParallelAndCompletesAfterLastTile()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int requests = 0, responses = 0, active = 0, peak = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                socket->setParent(&server);
                connect(socket, &QTcpSocket::readyRead, &server, [&, socket] {
                    socket->readAll();
                    if (socket->property("requested").toBool()) return;
                    socket->setProperty("requested", true);
                    ++requests; peak = qMax(peak, ++active);
                    QTimer::singleShot(requests == 1 ? 700 : 100, socket, [&, socket] {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\ntile");
                        socket->disconnectFromHost(); ++responses; --active;
                    });
                });
            }
        });
        auto *tiles = core::TLMaps::Instance();
        const auto previousProxy = tiles->Proxy;
        const auto previousMode = tiles->GetAccessMode();
        const bool previousMemory = tiles->UseMemoryCache();
        const auto restore = qScopeGuard([&] {
            tiles->Proxy = previousProxy; tiles->setAccessMode(previousMode); tiles->setUseMemoryCache(previousMemory);
        });
        tiles->Proxy = QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", server.serverPort());
        tiles->setAccessMode(core::AccessMode::ServerOnly); tiles->setUseMemoryCache(false);
        internals::Core map;
        map.OnMapSizeChanged(512, 512); map.SetZoom(10);
        map.SetMapType(core::MapType::OpenStreetMap);
        map.SetCurrentPosition(internals::PointLatLng(9.2, -83.4));
        map.SetrenderOffset(core::Point(256 - map.GetcurrentPositionGPixel().X(), 256 - map.GetcurrentPositionGPixel().Y()));
        map.UpdateCenterTileXYLocation();
        QSignalSpy complete(&map, &internals::Core::OnTileLoadComplete);
        int responsesAtCompletion = 0;
        connect(&map, &internals::Core::OnTileLoadComplete, this, [&] { responsesAtCompletion = responses; });
        map.UpdateBounds();
        QTRY_VERIFY_WITH_TIMEOUT(complete.size() > 0, 2500);
        QVERIFY2(peak >= 4, "A shared settings lock must not serialize tile downloads");
        QCOMPARE(responsesAtCompletion, requests);
        QVERIFY(requests > 4);
        QCOMPARE(complete.size(), 1);
        QTRY_COMPARE(map.GetDiagnostics().runningThreads, 0);
    }
    void failedClickShowsPersistentFirmwareState()
    {
        TestConnection connection;
        connection.returnNull = true;
        MessageCapture messages;
        QSignalSpy connected(manager, &Core::ConnectionManager::deviceConnected);
        QVERIFY(QMetaObject::invokeMethod(manager, "onConnectClicked", Qt::DirectConnection));
        QCoreApplication::processEvents();
        auto *dialog = page;
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        auto *guidance = dialog->findChild<QLabel *>("connectionErrorGuidance");
        QVERIFY(guidance);
        QVERIFY(!guidance->text().contains("simulated serial open failure"));
        auto *details = dialog->findChild<QPlainTextEdit *>("connectionErrorDetails");
        QVERIFY(details);
        QVERIFY(details->toPlainText().contains("simulated serial open failure"));
        QVERIFY(!details->isVisible());
        QVERIFY(!dialog->findChild<QPushButton *>("copyConnectionErrorCommand")->isVisible());
        QCOMPARE(connected.count(), 0);
        QVERIFY(!manager->isConnected());
        QCOMPARE(messages.warnings.size(), 1);
        QVERIFY(messages.warnings.first().contains("Serial: test controller"));
        manager->disconnectDevice();
        QVERIFY(!guidance->text().isEmpty());
        manager->clearControllerRecovery();
        QVERIFY(guidance->text().isEmpty());
    }
    void alreadyOpenDeviceIsNotOpenedTwice()
    {
        TestConnection connection;
        connection.openInPlugin = true;
        MessageCapture messages;
        QSignalSpy connected(manager, &Core::ConnectionManager::deviceConnected);
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        QCOMPARE(connection.io->openCalls, 1);
        QCOMPARE(connected.count(), 1);
        QVERIFY(messages.warnings.isEmpty());
        QVERIFY(messages.information.join('\n').contains("waiting for the UAVTalk handshake"));
        QVERIFY(manager->disconnectDevice());
        QCOMPARE(connection.closeCalls, 1);
    }
    void serialUnplugWhileOpenClearsFirmwareAndReconnectsFresh()
    {
        EnumeratedSerialConnection connection;
        connection.attach();
        auto *device = connection.availableDevices().first();
        QVERIFY(manager->connectDevice({&connection, device}));
        manager->setFirmwareIdentity("BTFL", "OMNIBUSF4SD", true, "3.4.0");
        QSignalSpy disconnected(manager, &Core::ConnectionManager::deviceDisconnected);
        connection.connectedPorts.clear();
        QTRY_COMPARE_WITH_TIMEOUT(disconnected.size(), 1, 2000);
        QVERIFY(!manager->isConnected()); QCOMPARE(connection.closeCalls, 1);
        QVERIFY(manager->detectedFirmware().isEmpty()); QVERIFY(!manager->hasMspFirmware());
        connection.attach("DroniNG OMNIBUSF4SD", 0x20a0, 0x415b);
        const auto updated = connection.availableDevices(); QCOMPARE(updated.size(), 1);
        QVERIFY(manager->connectDevice({&connection, updated.first()}));
        QVERIFY(manager->firmwareDetectionActive()); QVERIFY(manager->detectedFirmware().isEmpty());
    }
    void serialPortReplacementAndIndependentProviders()
    {
        EnumeratedSerialConnection first, second;
        first.attach(); second.attach("Second controller");
        auto *old = first.availableDevices().first();
        QVERIFY(old != second.availableDevices().first());
        QVERIFY(manager->connectDevice({&first, old}));
        first.attach("DroniNG OMNIBUSF4SD", 0x20a0, 0x415b);
        auto *replacement = first.availableDevices().first();
        QVERIFY(replacement != old); QVERIFY(!manager->isConnected());
        QVERIFY(replacement->getDisplayName().startsWith("DroniNG"));
        QCOMPARE(second.availableDevices().first()->getDisplayName(), QString("Second controller - ttyACM0"));
    }
    void transportLossDuringInstallationClearsIdentity()
    {
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setFirmwareIdentity("BTFL", "OMNIBUSF4SD", true, "3.4.0");
        manager->beginFirmwareInstallation();
        QVERIFY(manager->detectedFirmware().isEmpty()); QVERIFY(manager->firmwareBoardTarget().isEmpty());
        QVERIFY(!manager->hasMspFirmware());
        manager->resumePolling();
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setNativeFirmwareIdentity("OmnibusF4SD", "DroniNG");
        emit connection.transportDisconnected(&connection);
        QVERIFY(!manager->isConnected()); QVERIFY(manager->detectedFirmware().isEmpty());
        manager->endFirmwareInstallation();
        QVERIFY(manager->detectedBootloader().isEmpty());
    }
    void closedTransportCannotDisconnectItsReplacement()
    {
        TestConnection first, second;
        QVERIFY(manager->connectDevice({&first, first.device}));
        first.io->close(); // Queued physical transport closure.
        manager->disconnectDevice();
        QVERIFY(manager->connectDevice({&second, second.device}));
        QCoreApplication::processEvents();
        QVERIFY(manager->isConnected()); QCOMPARE(manager->getCurrentDevice().connection, &second);
        second.io->close();
        QTRY_VERIFY(!manager->isConnected());
    }
    void verifiedInstallSurvivesPageRestartUntilMatchingLiveData()
    {
        const auto board = Core::FirmwareCatalog::instance().board("omnibusf4sd");
        const auto image = Core::FirmwareInstallImage::load(board, Core::FirmwareCatalog::imagePath(board,
            Core::FirmwareCatalog::ImageKind::FullInstall, {QDir(GCS_BUILD_TREE_PATH).filePath("firmware")}));
        QVERIFY2(image.valid(), qPrintable(image.error));
        manager->recordVerifiedInstallation(board.id, "1-3", image.descriptor, "Firmware verified. Starting DroniNG…");
        Core::FirmwarePage restarted(manager);
        restarted.resize(1100, 850); restarted.show();
        QVERIFY(restarted.findChild<QWidget *>("firmwarePendingInstallation")->isVisible());
        QCOMPARE(restarted.findChild<QLabel *>("firmwarePageTitle")->text(), QString("Finish controller startup"));
        QVERIFY(manager->detectedFirmware().isEmpty());
        const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captures.isEmpty()) { QCoreApplication::processEvents(); QVERIFY(restarted.grab().save(captures + "/firmware-pending-startup.png")); }
        TestConnection connection("installed controller", "1-3");
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->reportFirmwareDescription(image.descriptor); manager->reportFlightDataReceived();
        QVERIFY(!manager->verifiedInstallation().boardId.isEmpty()); // Profile is still required.
        manager->reportFirmwareProfile(1, "radix", "radix-f446-rom-v1", QByteArray(32, 'x'));
        QVERIFY(!manager->verifiedInstallation().boardId.isEmpty());
        manager->reportFirmwareProfile(1, board.id, board.layoutId, Core::FirmwareInstallImage::importedProfileHash(board.id));
        QVERIFY(manager->verifiedInstallation().boardId.isEmpty());
    }
    void failedDeviceOpenIsReleased()
    {
        TestConnection connection;
        connection.failOpen = true;
        MessageCapture messages;
        QVERIFY(!manager->connectDevice({&connection, connection.device}));
        QCOMPARE(connection.closeCalls, 1);
        QVERIFY(connection.io.isNull());
        QVERIFY(messages.warnings.join('\n').contains("simulated permission denied"));
    }
    void missingSerialDeviceExplainsFailure()
    {
        SerialConnection connection;
        Core::IDevice device;
        device.setName("droning-test-port-that-does-not-exist");
        MessageCapture messages;
        QVERIFY(!connection.openDevice(&device));
        QVERIFY(!connection.deviceOpened());
        QVERIFY(connection.errorString().contains(device.getName()));
        QVERIFY(messages.warnings.join('\n').contains("disappeared"));
    }
    void serialOpenErrorGuidance_data()
    {
        QTest::addColumn<QFile::Permissions>("permissions");
        QTest::addColumn<int>("error");
        QTest::addColumn<QString>("guidance");
        QTest::newRow("denied-read-and-write") << QFile::Permissions{}
            << int(QSerialPort::PermissionError) << QString("doesn't have permission");
        QTest::newRow("denied-write") << QFile::Permissions(QFile::ReadOwner)
            << int(QSerialPort::PermissionError) << QString("doesn't have permission");
        QTest::newRow("accessible-port-may-be-busy")
            << QFile::Permissions(QFile::ReadOwner | QFile::WriteOwner)
            << int(QSerialPort::PermissionError) << QString("Another app may be using");
        QTest::newRow("other-error-on-denied-file") << QFile::Permissions{}
            << int(QSerialPort::ResourceError) << QString();
    }
    void serialOpenErrorGuidance()
    {
        QFETCH(QFile::Permissions, permissions);
        QFETCH(int, error);
        QFETCH(QString, guidance);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("serial-test-port");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(file.setPermissions(permissions));
        const bool writable = permissions.testFlag(QFile::WriteOwner);
        QCOMPARE(file.open(QIODevice::ReadWrite), writable);
        file.close();
        const auto failure = SerialConnection::openFailure(path, 115200,
            QSerialPort::SerialPortError(error), "original serial error");
        QVERIFY(failure.details.contains(path));
        QVERIFY(failure.details.contains("115200 baud"));
        QVERIFY(failure.details.contains("original serial error"));
        QVERIFY(failure.details.contains(QString("Qt serial error %1").arg(error)));
        QVERIFY(!failure.message.contains("original serial error"));
        QVERIFY(!failure.message.contains("Qt serial error"));
        QVERIFY(!failure.message.contains("administrator", Qt::CaseInsensitive));
#ifdef Q_OS_LINUX
        if (!guidance.isEmpty())
            QVERIFY(failure.message.contains(guidance));
#endif
        // Joining a group cannot repair this user's own file permissions, and
        // an accessible port must not get a group-membership recommendation.
        QVERIFY(failure.command.isEmpty());
        QVERIFY(!failure.message.contains("chmod"));
    }
    void missingPortDoesNotInventGroupDiagnosis()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto failure = SerialConnection::openFailure(
            directory.filePath("missing-port"), 115200, QSerialPort::PermissionError,
            "original serial error");
#ifdef Q_OS_LINUX
        QVERIFY(failure.details.contains("could not be checked"));
#endif
        QVERIFY(failure.command.isEmpty());
    }
    void serialPermissionGuidanceReachesFirmwareAndLog()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("serial-test-port");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(file.setPermissions({}));
        TestConnection connection;
        connection.returnNull = true;
        connection.failure = SerialConnection::openFailure(
            path, 115200, QSerialPort::PermissionError, "Permission denied");
        MessageCapture messages;
        QSignalSpy connected(manager, &Core::ConnectionManager::deviceConnected);
        QVERIFY(QMetaObject::invokeMethod(manager, "onConnectClicked", Qt::DirectConnection));
        QCoreApplication::processEvents();
        auto *dialog = page;
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        auto *guidance = dialog->findChild<QLabel *>("connectionErrorGuidance");
        QVERIFY(guidance);
        QCOMPARE(guidance->text(), connection.failure.message);
        QCOMPARE(guidance->textFormat(), Qt::PlainText);
        QVERIFY(guidance->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
        QVERIFY(guidance->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));
        auto *details = dialog->findChild<QPlainTextEdit *>("connectionErrorDetails");
        QVERIFY(details);
        QVERIFY(!details->isVisible());
        QVERIFY(details->toPlainText().contains(connection.failure.message));
        QVERIFY(details->toPlainText().contains(connection.failure.details));
        QVERIFY(messages.warnings.join('\n').contains(connection.failure.details));
        QCOMPARE(connected.count(), 0);
        QVERIFY(!manager->isConnected());
        manager->disconnectDevice();
        QCOMPARE(guidance->text(), connection.failure.message);
        manager->clearControllerRecovery();
        QVERIFY(guidance->text().isEmpty());
    }
    void groupAccessCommandQuotesAccount_data()
    {
        QTest::addColumn<QString>("group");
        QTest::addColumn<QString>("username");
        QTest::addColumn<QString>("command");
        QTest::newRow("dialout") << QString("dialout") << QString("rfirmin")
            << QString("sudo usermod -aG 'dialout' 'rfirmin'");
        QTest::newRow("literal-shell-characters") << QString("device's group") << QString("user$(id)")
            << QString("sudo usermod -aG 'device'\\''s group' 'user$(id)'");
    }
    void groupAccessCommandQuotesAccount()
    {
        QFETCH(QString, group);
        QFETCH(QString, username);
        QFETCH(QString, command);
        const auto failure = SerialConnection::groupAccessFailure(group, username, "raw error");
        QCOMPARE(failure.command, command);
        QCOMPARE(failure.details, QString("raw error"));
        QCOMPARE(failure.message.count('.'), 3);
        QVERIFY(!failure.message.contains("raw error"));
        QVERIFY(!failure.message.contains("administrator", Qt::CaseInsensitive));
    }
    void firmwareDfuAccessStateCopiesCommand()
    {
        class AccessDeniedRunner final : public Core::DfuCommandRunner {
        public:
            void run(const QString &, const QStringList &arguments, int) override {
                QVERIFY(arguments.contains("-l"));
                QTimer::singleShot(0, this, [this] {
                    emit finished(0, "Cannot open DFU device 0483:df11 (LIBUSB_ERROR_ACCESS)");
                });
            }
            void cancel() override {}
        } runner;
        const auto board = Core::FirmwareCatalog::instance().board("hglrc-f4-v6pro");
        // The initial image is absent, so the widget's scheduled start cannot use real USB.
        Core::FirmwareInstallWidget installation(manager, board, "/missing-droning-image.hex", nullptr, &runner);
        installation.resize(700, 480); installation.show();
        auto *installer = installation.findChild<Core::DfuInstaller *>();
        QVERIFY(installer);
        QTRY_COMPARE(installer->stage(), Core::DfuInstaller::Stage::Failed);
        const QDir build(GCS_BUILD_TREE_PATH);
        installer->start(board, Core::FirmwareCatalog::imagePath(board,
            Core::FirmwareCatalog::ImageKind::FullInstall, {build.filePath("firmware")}),
            "1-3", build.filePath("bin/dfu-util"));
        QTRY_VERIFY(installer->usbAccessDenied());
        auto *command = installation.findChild<QLabel *>("firmwareInstallAccessCommand");
        auto *copy = installation.findChild<QPushButton *>("copyFirmwareInstallAccessCommand");
        auto *boot = installation.findChild<QWidget *>("firmwareInstallBootInstructions");
        auto *manual = installation.findChild<QLabel *>("firmwareInstallBoardManual");
        auto *progress = installation.findChild<QProgressBar *>("firmwareInstallProgress");
        auto *details = installation.findChild<QPlainTextEdit *>("firmwareInstallDetails");
        auto *status = installation.findChild<QLabel *>("firmwareInstallStatus");
        QVERIFY(command && copy && boot && manual && progress && details && status);
        QVERIFY(command->isVisible()); QVERIFY(copy->isVisible());
        QVERIFY(!command->text().isEmpty());
        QCOMPARE(command->text(), installer->accessCommand());
        QCOMPARE(command->textFormat(), Qt::PlainText);
        QVERIFY(!boot->isVisible()); QVERIFY(!manual->isVisible()); QVERIFY(!progress->isVisible());
        QVERIFY(!details->isVisible());
        QVERIFY(status->text().contains("The board is in DFU mode"));
        QApplication::clipboard()->clear(); QTest::mouseClick(copy, Qt::LeftButton);
        QCOMPARE(QApplication::clipboard()->text(), installer->accessCommand());
        const QString captureDirectory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captureDirectory.isEmpty())
            QVERIFY(installation.grab().save(captureDirectory + "/firmware-dfu-access.png"));
        installer->cancel();
        QVERIFY(!command->isVisible()); QVERIFY(!copy->isVisible());
        QVERIFY(!installer->usbAccessDenied());
    }
    void firmwareInstallAutomaticallyRebootsOmnibus_data()
    {
        QTest::addColumn<bool>("armed");
        QTest::newRow("disarmed-controller") << false;
        QTest::newRow("armed-controller-blocked") << true;
    }
    void firmwareInstallAutomaticallyRebootsOmnibus()
    {
        QFETCH(bool, armed);
        class BootConnection final : public TestConnection {
        public:
            bool armed = false;
            QList<QByteArray> commands;
            BootConnection() : TestConnection("OmnibusF4 - ttyACM0", "1-3") { openInPlugin = true; }
            QIODevice *openDevice(Core::IDevice *device) override {
                auto *result = TestConnection::openDevice(device);
                io->onWrite = [this](const QByteArray &bytes) {
                    commands.append(bytes);
                    if (bytes == "#") { io->receive("\r\nEntering CLI Mode, type 'exit' to return, or 'help'\r\n# "); return; }
                    if (bytes.size() != 6 || !bytes.startsWith("$M<")) return;
                    const int command = quint8(bytes[4]); QByteArray payload;
                    if (command == 1) payload = QByteArray::fromHex("000127");
                    if (command == 2) payload = "BTFL";
                    if (command == 3) payload = QByteArray::fromHex("030400");
                    if (command == 4) payload = QByteArray::fromHex("4f425344000002030b") + "OMNIBUSF4SD";
                    if (command == 119) payload = QByteArray::fromHex("010500");
                    if (command == 101) { payload = QByteArray(11, 0); payload[6] = armed ? 4 : 0; }
                    QByteArray response("$M>", 3); response.append(char(payload.size())); response.append(char(command)); response += payload;
                    quint8 checksum = 0; for (int i = 3; i < response.size(); ++i) checksum ^= quint8(response[i]);
                    response.append(char(checksum)); io->receive(response);
                };
                return result;
            }
        } connection;
        connection.armed = armed;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setFirmwareIdentity("BTFL", "OMNIBUSF4SD", true, "3.4.0");
        class DiscoveryRunner final : public Core::DfuCommandRunner {
        public:
            QList<QStringList> commands;
            void run(const QString &, const QStringList &args, int) override { commands.append(args); }
            void cancel() override {}
        } runner;
        const auto board = Core::FirmwareCatalog::instance().board("omnibusf4sd");
        const QString path = QDir(GCS_BUILD_TREE_PATH).filePath("firmware/ef_omnibusf4sd.hex");
        Core::FirmwareInstallWidget installation(manager, board, path, nullptr, &runner);
        installation.resize(700, 480); installation.show();
        auto *instructions = installation.findChild<QWidget *>("firmwareInstallBootInstructions");
        QVERIFY(!instructions->isVisible());
        QVERIFY(installation.findChild<QLabel *>("firmwareInstallStatus")->text().contains("automatically"));
        QTRY_VERIFY(connection.closeCalls >= 2);
        if (armed) {
            QVERIFY(!connection.commands.contains("#")); QVERIFY(!connection.commands.contains("bl\n"));
            QVERIFY(runner.commands.isEmpty());
            QVERIFY(installation.findChild<QLabel *>("firmwareInstallStatus")->text().contains("Disarm"));
            QVERIFY(installation.findChild<QPushButton *>("firmwareInstallRetry")->isEnabled());
            QVERIFY(instructions->isVisible());
        } else {
            QVERIFY(connection.commands.contains("bl\n")); QTRY_VERIFY(!runner.commands.isEmpty());
            QCOMPARE(runner.commands.first(), (QStringList{"-d", ",0483:df11", "-l"}));
            QCOMPARE(installation.findChild<QLabel *>("firmwareInstallHeading")->text(), QString("Restarting your controller"));
            QVERIFY(!instructions->isVisible());
        }
        installation.cancelInstallation();
    }
    void firmwareInstallFocusesCurrentTask_data()
    {
        QTest::addColumn<QString>("boardId");
        QTest::newRow("verified-hglrc-procedure") << QString("hglrc-f4-v6pro");
        QTest::newRow("generic-manual-procedure") << QString("revolution");
    }
    void firmwareInstallFocusesCurrentTask()
    {
        QFETCH(QString, boardId);
        class WaitingRunner final : public Core::DfuCommandRunner {
        public:
            void run(const QString &, const QStringList &arguments, int) override {
                QVERIFY(arguments.contains("-l"));
            }
            void cancel() override {}
            QString connectedUsbDevice(const QString &) const override { return "Normal controller firmware"; }
        } runner;
        const auto board = Core::FirmwareCatalog::instance().board(boardId);
        const QDir build(GCS_BUILD_TREE_PATH);
        Core::FirmwareInstallWidget installation(manager, board, "/missing-droning-image.hex", nullptr, &runner);
        installation.resize(700, 480); installation.show();
        auto *installer = installation.findChild<Core::DfuInstaller *>();
        QTRY_COMPARE(installer->stage(), Core::DfuInstaller::Stage::Failed);
        installer->start(board, Core::FirmwareCatalog::imagePath(board,
            Core::FirmwareCatalog::ImageKind::FullInstall, {build.filePath("firmware")}),
            "1-3", build.filePath("bin/dfu-util"));
        QTRY_COMPARE(installer->stage(), Core::DfuInstaller::Stage::Waiting);
        auto *card = installation.findChild<QWidget *>("firmwareInstallTaskCard");
        auto *boot = installation.findChild<QWidget *>("firmwareInstallBootInstructions");
        auto *manual = installation.findChild<QLabel *>("firmwareInstallBoardManual");
        auto *toggle = installation.findChild<QToolButton *>("firmwareInstallDetailsToggle");
        auto *details = installation.findChild<QPlainTextEdit *>("firmwareInstallDetails");
        auto *retry = installation.findChild<QPushButton *>("firmwareInstallRetry");
        auto *progress = installation.findChild<QProgressBar *>("firmwareInstallProgress");
        QVERIFY(card && boot && manual && toggle && details && retry && progress);
        QVERIFY(installation.styleSheet().isEmpty());
        QCOMPARE(installation.palette().color(QPalette::Window), QApplication::palette().color(QPalette::Window));
        QCOMPARE(installation.palette().color(QPalette::Highlight), QApplication::palette().color(QPalette::Highlight));
        QVERIFY(boot->isVisible()); QVERIFY(progress->isVisible()); QVERIFY(!retry->isVisible());
        QVERIFY(card->isAncestorOf(manual)); QVERIFY(card->isAncestorOf(toggle));
        QVERIFY(card->isAncestorOf(details)); QVERIFY(!details->isVisible());
        QStringList steps;
        for (int i = 1; i <= 3; ++i) {
            auto *step = installation.findChild<QLabel *>(QString("firmwareInstallBootStep%1").arg(i));
            QVERIFY(step && step->isVisible()); steps.append(step->text());
        }
        QVERIFY(steps[0].contains("flight battery"));
        QCOMPARE(manual->isVisible(), !board.manualUrl.isEmpty());
        if (boardId == "hglrc-f4-v6pro") {
            QVERIFY(steps[1].contains("BOOT button beside the USB socket"));
            QCOMPARE(steps[2], QString("Release BOOT."));
        } else {
            QVERIFY(steps[1].contains("board's manual"));
            QVERIFY(steps[2].contains("manual's USB bootloader procedure"));
            QVERIFY(!steps.join(' ').contains("beside the USB socket"));
        }
        emit runner.finished(0, {});
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::NeedsBoot);
        QVERIFY(progress->isVisible()); QCOMPARE(progress->maximum(), 0);
        QVERIFY(!retry->isVisible()); QVERIFY(boot->isVisible());
        QCOMPARE(installation.findChild<QLabel *>("firmwareInstallStatus")->text(),
            QString("Installation starts automatically when your controller reconnects in BOOT mode."));
        QTest::mouseClick(toggle, Qt::LeftButton);
        QVERIFY(details->isVisible()); QVERIFY(details->toPlainText().contains("Normal controller firmware"));
        QTest::mouseClick(toggle, Qt::LeftButton);
        QVERIFY(!details->isVisible());
        const auto directory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!directory.isEmpty()) QVERIFY(installation.grab().save(directory + "/firmware-boot-" + boardId + ".png"));
        emit runner.finished(1, "Simulated device error");
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::Failed);
        QVERIFY(retry->isVisible()); QVERIFY(retry->isEnabled()); QVERIFY(!progress->isVisible());
    }
    void firmwareInstallLabelsProgressAndOffersStart()
    {
        class InstallRunner final : public Core::DfuCommandRunner {
        public:
            QByteArray written;
            bool answerDiscovery = false;
            void run(const QString &, const QStringList &arguments, int) override {
                if (arguments.contains("-D")) {
                    QFile file(arguments[arguments.indexOf("-D") + 1]);
                    QVERIFY(file.open(QIODevice::ReadOnly)); written = file.readAll();
                } else if (arguments.contains("-U")) {
                    const bool leaving = arguments.join(' ').contains(":leave");
                    const auto bytes = leaving ? written.left(1) : written;
                    QFile file(arguments[arguments.indexOf("-U") + 1]);
                    QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size()));
                } else if (answerDiscovery) {
                    QTimer::singleShot(0, this, [this] { emit finished(0, {}); });
                }
            }
            void cancel() override {}
        } runner;
        auto board = Core::FirmwareCatalog::instance().board("hglrc-f4-v6pro");
        board.settings = {}; // Hardware configuration has its own integration tests.
        const QDir build(GCS_BUILD_TREE_PATH);
        const auto imagePath = Core::FirmwareCatalog::imagePath(board,
            Core::FirmwareCatalog::ImageKind::FullInstall, {build.filePath("firmware")});
        const auto image = Core::FirmwareInstallImage::load(board, imagePath);
        QVERIFY2(image.valid(), qPrintable(image.error));
        Core::FirmwareInstallWidget installation(manager, board, "/missing-droning-image.hex", nullptr, &runner);
        installation.resize(700, 420); installation.show();
        auto *installer = installation.findChild<Core::DfuInstaller *>();
        QTRY_COMPARE(installer->stage(), Core::DfuInstaller::Stage::Failed);
        installer->start(board, imagePath, "1-3", build.filePath("bin/dfu-util"));
        const QString listing = "Found DFU: [0483:df11] ver=2200, devnum=8, cfg=1, intf=0, path=\"1-3\", alt=0, name=\"@Internal Flash /0x08000000/04*016Kg,01*064Kg,07*128Kg\", serial=\"1234ABCD\"\n";
        emit runner.finished(0, listing); emit runner.finished(0, listing);
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::Writing);
        auto *label = installation.findChild<QLabel *>("firmwareInstallProgressLabel");
        auto *progress = installation.findChild<QProgressBar *>("firmwareInstallProgress");
        auto *hint = installation.findChild<QLabel *>("firmwareInstallReconnectHint");
        auto *start = installation.findChild<QPushButton *>("firmwareInstallStartController");
        auto *status = installation.findChild<QLabel *>("firmwareInstallStatus");
        auto *retry = installation.findChild<QPushButton *>("firmwareInstallRetry");
        QVERIFY(label && progress && hint && start && status && retry);
        QVERIFY(label->isVisible()); QVERIFY(progress->isVisible()); QVERIFY(!start->isVisible());
        emit runner.output("Erase   [=  ] 25%\r");
        QCOMPARE(label->text(), QString("Erasing previous firmware · 25%"));
        QCOMPARE(progress->accessibleName(), QString("Erasing previous firmware"));
        emit runner.output("Download [=  ] 40%\r");
        QCOMPARE(label->text(), QString("Writing firmware · 40%")); QCOMPARE(progress->value(), 40);
        const auto directory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!directory.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(installation.grab().save(directory + "/firmware-labelled-write.png"));
        }
        emit runner.finished(0, {});
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::Reading);
        emit runner.output("Upload [== ] 80%\r");
        QCOMPARE(label->text(), QString("Verifying firmware · 80%"));
        emit runner.finished(0, {}); emit runner.finished(0, {});
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::AwaitingTelemetry);
        QCOMPARE(status->text(), QString("Firmware installed and verified."));
        QCOMPARE(label->text(), QString("Waiting for live data…"));
        QVERIFY(hint->isVisible()); QVERIFY(hint->text().contains("disconnect USB and the flight battery"));
        QVERIFY(hint->text().contains("If you used a BOOT")); QVERIFY(start->isVisible()); QVERIFY(start->isEnabled());
        QVERIFY(!retry->isVisible());
        int visibleBars = 0;
        for (const auto *bar : installation.findChildren<QProgressBar *>()) if (bar->isVisible()) ++visibleBars;
        QCOMPARE(visibleBars, 1);
        if (!directory.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(installation.grab().save(directory + "/firmware-start-controller.png"));
        }
        runner.answerDiscovery = true;
        QSignalSpy bootRequests(manager, &Core::ConnectionManager::firmwareBootRequested);
        QSignalSpy bootCancelled(manager, &Core::ConnectionManager::firmwareBootCancelled);
        // A stalled start gets one automatic attempt, without a user click.
        QTRY_COMPARE_WITH_TIMEOUT(bootRequests.size(), 1, 6500);
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::Restarting);
        QCOMPARE(bootRequests[0][0].toString(), QString("1-3"));
        QCOMPARE(bootRequests[0][1].toByteArray(), image.descriptor);
        QVERIFY(start->isVisible()); QVERIFY(!start->isEnabled());
        emit manager->firmwareBootFinished(false, "The controller did not answer the start command.");
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::AwaitingTelemetry);
        QCOMPARE(status->text(), QString("The controller did not answer the start command."));
        QVERIFY(hint->isVisible()); QVERIFY(start->isEnabled()); QVERIFY(!retry->isVisible());
        QVERIFY(!bootCancelled.isEmpty());
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_COMPARE(bootRequests.size(), 2);
        emit manager->firmwareBootFinished(true, {});
        QCOMPARE(status->text(), QString("Firmware installed and verified."));
        emit manager->firmwareDescriptionReceived(image.descriptor);
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::AwaitingTelemetry);
        emit manager->flightDataReady();
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::Complete);
        QVERIFY(!hint->isVisible()); QVERIFY(!start->isVisible()); QVERIFY(!progress->isVisible());
        QVERIFY(!label->isVisible());
        TestConnection controller;
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->reportFlightDataReceived();
        manager->setControllerSetupRequired(true);
        QCOMPARE(installation.findChild<QLabel *>("firmwareInstallHeading")->text(), QString("Set up controller?"));
        auto *configure = installation.findChild<QPushButton *>("firmwareInstallConfigureAction");
        QCOMPARE(configure->text(), QString("Launch setup wizard"));
        QVERIFY(configure->isVisible());
        QSignalSpy setupRequests(manager, &Core::ConnectionManager::setupWizardRequested);
        QSignalSpy closed(&installation, &Core::FirmwareInstallWidget::closed);
        QTest::mouseClick(configure, Qt::LeftButton);
        QCOMPARE(closed.size(), 1); QCOMPARE(setupRequests.size(), 1);
        QVERIFY(!manager->firmwareInstallationActive());
    }
    void firmwareInstallStopsAutomaticRetries_data()
    {
        QTest::addColumn<bool>("startupResets");
        QTest::addColumn<bool>("silentTransport");
        QTest::newRow("waiting-timeout") << false << false;
        QTest::newRow("repeated-startup-resets") << true << false;
        QTest::newRow("silent-native-transport") << false << true;
    }
    void firmwareInstallStopsAutomaticRetries()
    {
        QFETCH(bool, startupResets);
        QFETCH(bool, silentTransport);
        class CompletedRunner final : public Core::DfuCommandRunner {
        public:
            QByteArray written;
            void run(const QString &, const QStringList &arguments, int) override {
                QString output;
                if (arguments.contains("-D")) {
                    QFile file(arguments[arguments.indexOf("-D") + 1]);
                    QVERIFY(file.open(QIODevice::ReadOnly)); written = file.readAll();
                } else if (arguments.contains("-U")) {
                    const auto bytes = arguments.join(' ').contains(":leave") ? written.left(1) : written;
                    QFile file(arguments[arguments.indexOf("-U") + 1]);
                    QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size()));
                } else if (written.isEmpty()) {
                    output = "Found DFU: [0483:df11] ver=2200, devnum=8, cfg=1, intf=0, path=\"1-3\", alt=0, name=\"@Internal Flash /0x08000000/04*016Kg,01*064Kg,07*128Kg\", serial=\"1234ABCD\"\n";
                }
                QTimer::singleShot(0, this, [this, output] { emit finished(0, output); });
            }
            void cancel() override {}
        } runner;
        auto board = Core::FirmwareCatalog::instance().board("hglrc-f4-v6pro"); board.settings = {};
        const QDir build(GCS_BUILD_TREE_PATH);
        Core::FirmwareInstallWidget installation(manager, board, "/missing-droning-image.hex", nullptr, &runner);
        installation.resize(700, 420); installation.show();
        auto *installer = installation.findChild<Core::DfuInstaller *>();
        QTRY_COMPARE(installer->stage(), Core::DfuInstaller::Stage::Failed);
        installer->start(board, Core::FirmwareCatalog::imagePath(board,
            Core::FirmwareCatalog::ImageKind::FullInstall, {build.filePath("firmware")}),
            "1-3", build.filePath("bin/dfu-util"));
        const auto response = connect(manager, &Core::ConnectionManager::firmwareBootRequested,
            manager, [this] { emit manager->firmwareBootFinished(false, "The controller is not available to start over USB."); });
        const auto disconnectResponse = qScopeGuard([response] { QObject::disconnect(response); });
        QTRY_COMPARE(installer->stage(), Core::DfuInstaller::Stage::AwaitingTelemetry);
        std::unique_ptr<TestConnection> silent;
        if (silentTransport) {
            silent = std::make_unique<TestConnection>("Silent native controller", "1-3"); silent->transport = "USB";
            QVERIFY(manager->autoConnectDevice({silent.get(), silent->device}));
            QVERIFY(manager->firmwareDetectionActive());
            // Exercise the same timeout transition with a transport still open.
            installer->startupTimedOut();
            QVERIFY(manager->isConnected());
        }
        if (startupResets) {
            TestConnection unstable("OMNIBUSF4SD", "1-3"); unstable.transport = "USB";
            QVERIFY(manager->autoConnectDevice({&unstable, unstable.device}));
            emit unstable.transportDisconnected(&unstable);
            QTRY_COMPARE_WITH_TIMEOUT(installer->stage(), Core::DfuInstaller::Stage::StartupFailed, 2000);
        }
        auto *progress = installation.findChild<QProgressBar *>("firmwareInstallProgress");
        QTRY_VERIFY_WITH_TIMEOUT(!progress->isVisible(), 70000);
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::StartupFailed);
        QVERIFY(!manager->firmwareDetectionActive());
        if (silentTransport) {
            QCOMPARE(manager->firmwareStatus(), QString("Controller needs attention"));
            QVERIFY(manager->controllerRecovery().problem.message.contains("written and verified"));
            manager->endFirmwareInstallation();
            QVERIFY(!manager->firmwareDetectionActive());
            QVERIFY(manager->controllerRecovery().problem.message.contains("written and verified"));
        }
        QCOMPARE(installation.findChild<QLabel *>("firmwareInstallHeading")->text(), QString("Controller connection needs attention"));
        QVERIFY(installation.findChild<QLabel *>("firmwareInstallStatus")->text().contains(startupResets
            ? "Automatic retries have stopped" : "written and verified"));
        QCOMPARE(installation.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->text(), QString("Close"));
        QVERIFY(installation.findChild<QLabel *>("firmwareInstallReconnectHint")->isVisible());
        QVERIFY(installation.findChild<QPushButton *>("firmwareInstallStartController")->isEnabled());
        const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captures.isEmpty()) QVERIFY(installation.grab().save(captures + "/firmware-startup-failed.png"));
        if (silentTransport) {
            installation.cancelInstallation();
            QVERIFY(installer->log().contains("Verified firmware remains on the controller"));
            return;
        }
        TestConnection controller("Reconnected DroniNG", "1-3"); controller.transport = "USB";
        emit controller.availableDevChanged(&controller);
        QTest::qWait(1200);
        QVERIFY(!manager->isConnected());
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        QTRY_VERIFY_WITH_TIMEOUT(manager->isConnected(), 2000);
        QCOMPARE(manager->getCurrentDevice().device, controller.device);
        installation.cancelInstallation();
        QVERIFY(installer->log().contains("Verified firmware remains on the controller"));
        QVERIFY(!installer->log().contains("Installation cancelled"));
    }
    void startupUsbRulesDetection_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<int>("state");
        using State = Core::UsbAccessSetup::State;
        for (const auto &name : {"absent", "outdated", "masked", "too-late", "partial"})
            QTest::newRow(name) << QString(name) << int(State::InstallRules);
        for (const auto &name : {"installed", "different-name", "formatted"})
            QTest::newRow(name) << QString(name) << int(State::Ready);
        QTest::newRow("missing-package-rules") << QString("missing-package-rules") << int(State::MissingRules);
        QTest::newRow("missing-package-path") << QString("missing-package-path") << int(State::MissingRules);
    }
    void startupUsbRulesDetection()
    {
        QFETCH(QString, scenario);
        QFETCH(int, state);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir build(GCS_BUILD_TREE_PATH);
        const auto source = build.filePath("../../shared/usb_ids/droning.udev");
        QFile supplied(source); QVERIFY(supplied.open(QIODevice::ReadOnly));
        const auto contents = supplied.readAll();
        const QString vendor = directory.filePath("vendor"), local = directory.filePath("local");
        QVERIFY(QDir().mkpath(vendor)); QVERIFY(QDir().mkpath(local));
        const auto write = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        Core::UsbAccessSetup::Paths paths{source, {vendor, local}, directory.filePath("sys"), directory.filePath("dev")};
        if (scenario == "missing-package-rules") paths.bundledRules = directory.filePath("missing");
        else if (scenario == "missing-package-path") paths.bundledRules.clear();
        else if (scenario == "outdated") {
            auto old = contents; old.replace("MODE=\"0660\"", "MODE=\"0664\", GROUP=\"plugdev\"");
            QVERIFY(write(local + "/45-droning.rules", old));
        } else if (scenario == "masked") {
            QVERIFY(write(vendor + "/45-droning.rules", contents));
            QVERIFY(QFile::link("/dev/null", local + "/45-droning.rules"));
        } else if (scenario == "too-late") QVERIFY(write(local + "/99-droning.rules", contents));
        else if (scenario == "partial") {
            auto partial = contents;
            partial.replace("ATTR{idProduct}==\"df11\"", "ATTR{idProduct}==\"ffff\"");
            QVERIFY(write(local + "/45-droning.rules", partial));
        } else if (scenario == "formatted") {
            QByteArray formatted("# Locally formatted rules\n");
            for (const auto &line : contents.split('\n')) {
                if (line.isEmpty() || line.startsWith('#')) continue;
                auto fields = line.split(','); std::reverse(fields.begin(), fields.end());
                formatted += fields.join(",  ") + "\n";
            }
            QVERIFY(write(local + "/50-controller-access.rules", formatted));
        } else if (scenario != "absent") {
            QVERIFY(write(local + (scenario == "different-name" ? "/60-controller-access.rules" : "/45-droning.rules"), contents));
        }
        MessageCapture messages;
        QCOMPARE(int(Core::UsbAccessSetup::inspect(paths)), state);
        QVERIFY(messages.warnings.isEmpty());
    }
    void startupUsbNoticeCopiesCommandAndResolves()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir build(GCS_BUILD_TREE_PATH);
        const auto source = build.filePath("../../shared/usb_ids/droning.udev");
        const auto rulesDirectory = directory.filePath("rules");
        QVERIFY(QDir().mkpath(rulesDirectory));
        Core::UsbAccessSetup setup({source, {rulesDirectory}, directory.filePath("sys"), directory.filePath("dev")});
        QCOMPARE(setup.state(), Core::UsbAccessSetup::State::InstallRules);
        QVERIFY(setup.required()); QVERIFY(!setup.copied());
        QCOMPARE(setup.command(), Core::DfuInstaller::usbAccessRulesCommand(source));
        QSignalSpy changed(&setup, &Core::UsbAccessSetup::statusChanged);
        QSignalSpy copied(&setup, &Core::UsbAccessSetup::copiedChanged);
        QApplication::clipboard()->clear(); QVERIFY(setup.copyCommand());
        QCOMPARE(QApplication::clipboard()->text(), setup.command());
        QVERIFY(setup.copied()); QCOMPARE(copied.count(), 1);
        setup.refresh(); QCOMPARE(changed.count(), 0); QVERIFY(setup.copied());
        const auto installed = rulesDirectory + "/45-droning.rules";
        QVERIFY(QFile::copy(source, installed));
        // The timer resolves setup without another click or an application restart.
        QTRY_COMPARE_WITH_TIMEOUT(setup.state(), Core::UsbAccessSetup::State::Ready, 4000);
        QVERIFY(!setup.required()); QVERIFY(!setup.copied());
        QVERIFY(setup.command().isEmpty()); QVERIFY(!setup.copyCommand());
        QCOMPARE(changed.count(), 1);
        QVERIFY(QFile::remove(installed));
        QTRY_COMPARE_WITH_TIMEOUT(setup.state(), Core::UsbAccessSetup::State::InstallRules, 4000);
        QVERIFY(setup.required()); QVERIFY(!setup.copied()); QCOMPARE(changed.count(), 2);
    }
    void startupUsbConnectedDeviceNeedsReconnect()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir build(GCS_BUILD_TREE_PATH);
        const auto source = build.filePath("../../shared/usb_ids/droning.udev");
        const auto rulesDirectory = directory.filePath("rules");
        const auto sysDirectory = directory.filePath("sys/3-1");
        const auto devDirectory = directory.filePath("dev/003");
        for (const auto &path : {rulesDirectory, sysDirectory, devDirectory}) QVERIFY(QDir().mkpath(path));
        QVERIFY(QFile::copy(source, rulesDirectory + "/45-droning.rules"));
        const auto write = [](const QString &path, const QByteArray &value) {
            QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
        };
        QVERIFY(write(sysDirectory + "/idVendor", "0483\n"));
        QVERIFY(write(sysDirectory + "/idProduct", "df11\n"));
        QVERIFY(write(sysDirectory + "/busnum", "3\n"));
        QVERIFY(write(sysDirectory + "/devnum", "18\n"));
        const auto node = devDirectory + "/018";
        QVERIFY(write(node, {})); QVERIFY(QFile::setPermissions(node, QFile::ReadOwner));
        Core::UsbAccessSetup setup({source, {rulesDirectory}, directory.filePath("sys"), directory.filePath("dev")});
        QCOMPARE(setup.state(), Core::UsbAccessSetup::State::Reconnect);
        QVERIFY(setup.command().isEmpty()); QVERIFY(!setup.copyCommand());
        QVERIFY(setup.message().contains("USB access is set up"));
        // Unrelated USB devices must never get a DroniNG permission diagnosis.
        QVERIFY(write(sysDirectory + "/idProduct", "ffff\n")); setup.refresh();
        QCOMPARE(setup.state(), Core::UsbAccessSetup::State::Ready);
        QVERIFY(write(sysDirectory + "/idProduct", "df11\n")); setup.refresh();
        QCOMPARE(setup.state(), Core::UsbAccessSetup::State::Reconnect);
        QVERIFY(QFile::setPermissions(node, QFile::ReadOwner | QFile::WriteOwner));
        QTRY_COMPARE_WITH_TIMEOUT(setup.state(), Core::UsbAccessSetup::State::Ready, 4000);
        QVERIFY(!setup.required());
    }
    void firmwareAccessStateCopiesCommand()
    {
        TestConnection connection("OmnibusF4 - ttyACM0");
        connection.returnNull = true;
        connection.failure = SerialConnection::groupAccessFailure("dialout", "rfirmin",
            "Could not open /dev/ttyACM0 at 115200 baud (8N1, no flow control): "
            "Permission denied (Qt serial error 2)");
        MessageCapture messages;
        QVERIFY(QMetaObject::invokeMethod(manager, "onConnectClicked", Qt::DirectConnection));
        QCoreApplication::processEvents();
        auto *dialog = page;
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        auto *guidance = dialog->findChild<QLabel *>("connectionErrorGuidance");
        auto *command = dialog->findChild<QLabel *>("connectionErrorCommand");
        auto *copy = dialog->findChild<QPushButton *>("copyConnectionErrorCommand");
        auto *toggle = dialog->findChild<QToolButton *>("connectionErrorDetailsToggle");
        auto *details = dialog->findChild<QPlainTextEdit *>("connectionErrorDetails");
        QVERIFY(guidance && command && copy && toggle && details);
        QCOMPARE(guidance->text(), connection.failure.message);
        QCOMPARE(command->text(), connection.failure.command);
        QCOMPARE(command->textFormat(), Qt::PlainText);
        QVERIFY(command->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
        QVERIFY(command->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));
        QVERIFY(!details->isVisible());
        QVERIFY(!toggle->isChecked());
        const QString captureDirectory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captureDirectory.isEmpty())
            QVERIFY(dialog->grab().save(captureDirectory + "/connection-access-dialog.png"));
        QApplication::clipboard()->clear();
        QTest::mouseClick(copy, Qt::LeftButton);
        QCOMPARE(QApplication::clipboard()->text(), connection.failure.command);
        QVERIFY(dialog->isVisible());
        QVERIFY(!manager->isConnected());
        QTest::mouseClick(toggle, Qt::LeftButton);
        QVERIFY(details->isVisible());
        QVERIFY(details->isReadOnly());
        QVERIFY(details->toPlainText().contains(connection.failure.message));
        QVERIFY(details->toPlainText().contains(connection.failure.details));
        QVERIFY(messages.warnings.join('\n').contains(connection.failure.details));
        QTest::mouseClick(toggle, Qt::LeftButton);
        QVERIFY(!details->isVisible());
        manager->disconnectDevice();
        QCOMPARE(guidance->text(), connection.failure.message);
        manager->clearControllerRecovery();
        QVERIFY(guidance->text().isEmpty());
    }
    void oldConnectionDestructionDoesNotCloseNewConnection()
    {
        auto oldConnection = std::make_unique<TestConnection>();
        QVERIFY(manager->connectDevice({oldConnection.get(), oldConnection->device}));
        QVERIFY(manager->disconnectDevice());
        oldConnection.reset();
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        QCoreApplication::processEvents();
        QVERIFY(manager->isConnected());
        QCOMPARE(manager->getCurrentConnection(), connection.io.data());
        QVERIFY(connection.io->isOpen());
    }
    void rejectedAndDestroyedDeviceCountAsTransmitErrors()
    {
        MessageCapture messages;
        auto device = std::make_unique<TestDevice>();
        QVERIFY(device->open(QIODevice::ReadWrite));
        UAVTalk protocol(device.get(), objects);
        device->failWrite = true;
        auto *attitude = AttitudeActual::GetInstance(objects);
        QVERIFY(!protocol.sendObject(attitude, false, false));
        auto stats = protocol.getStats();
        QCOMPARE(stats.txBytes, quint32(0));
        QCOMPARE(stats.txErrors, quint32(1));
        device.reset();
        QVERIFY(!protocol.sendObject(attitude, false, false));
        QCOMPARE(protocol.getStats().txErrors, quint32(1));
    }
    void handshakeTimeoutAndRecovery_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("silent-port") << QByteArray();
        QTest::newRow("unrecognized-protocol") << QByteArray("not UAVTalk\n");
        QTest::newRow("valid-object-without-handshake")
            << objectFrame(AttitudeActual::GetInstance(objects));
    }
    void handshakeTimeoutAndRecovery()
    {
        QFETCH(QByteArray, input);
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        auto *flight = FlightTelemetryStats::GetInstance(objects);
        auto flightData = flight->getData();
        flightData.Status = FlightTelemetryStats::STATUS_DISCONNECTED;
        flight->setData(flightData);
        MessageCapture messages;
        UAVTalk protocol(connection.io, objects);
        Telemetry telemetry(&protocol, objects);
        TelemetryMonitor monitor(objects, &telemetry);
        connection.io->receive(input);
        QTRY_VERIFY_WITH_TIMEOUT(messages.warnings.join('\n').contains("UAVTalk handshake timed out"), 10000);
        const QString warning = messages.warnings.join('\n');
        QVERIFY2(warning.contains("Serial: test controller"), qPrintable(warning));
        QVERIFY(warning.contains(QString("rxBytes=%1,").arg(input.size())));
        QVERIFY(warning.contains(input.startsWith(char(0x3c)) ? "validRxObjects=1," : "validRxObjects=0,"));
        const auto tx = QRegularExpression("txBytes=([0-9]+)").match(warning);
        QVERIFY(tx.hasMatch());
        QVERIFY(tx.captured(1).toInt() > 0);
        QVERIFY(manager->isConnected());
        QTRY_VERIFY_WITH_TIMEOUT(!manager->firmwareError().message.isEmpty(), 2000);
        auto *dialog = page;
        QVERIFY(dialog->isVisible());
        QCOMPARE(manager->firmwareStatus(), QString("Controller needs attention"));
        auto *guidance = dialog->findChild<QLabel *>("connectionErrorGuidance");
        QVERIFY(guidance);
        QVERIFY(guidance->text().contains(input.isEmpty() ? "sent no data"
            : input.startsWith(char(0x3c)) ? "connection could not finish" : "data could not be read"));
        auto *details = dialog->findChild<QPlainTextEdit *>("connectionErrorDetails");
        QVERIFY(details);
        QVERIFY(!details->isVisible());
        QVERIFY(details->toPlainText().contains(warning));
        QCOMPARE(connection.io->written.contains("$M<"), !input.startsWith(char(0x3c)));
        const QString captureDirectory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captureDirectory.isEmpty() && input.isEmpty())
            QVERIFY(dialog->grab().save(captureDirectory + "/silent-telemetry-dialog.png"));
        QTest::qWait(700);
        QCOMPARE(messages.warnings.size(), 1);

        FlightTelemetryStats boardFlight;
        flightData.Status = FlightTelemetryStats::STATUS_HANDSHAKEACK;
        boardFlight.setData(flightData);
        connection.io->receive(objectFrame(&boardFlight));
        QVERIFY(messages.information.join('\n').contains("UAVTalk handshake completed"));
        // A late handshake alone must not make the failed card blip back to detection.
        QVERIFY(!guidance->text().isEmpty());
        QVERIFY(!manager->firmwareDetectionActive());
        connection.io->receive(objectFrame(AttitudeActual::GetInstance(objects)));
        QTRY_VERIFY(guidance->text().isEmpty());
        QCOMPARE(messages.warnings.size(), 1);
    }
    void firmwareProbeRejectsCorruptAndUnsolicitedIdentity()
    {
        MspFirmwareProbe probe;
        QVERIFY(probe.receive(mspFrame(2, "BTFL")).isEmpty());
        QCOMPARE(probe.start(), QByteArray::fromHex("244d3c000101"));
        probe.receive(mspFrame(2, "BTFL"));
        QVERIFY(probe.variant().isEmpty());
        const QByteArray api = mspFrame(1, QByteArray::fromHex("00012a"));
        QByteArray corrupt = api;
        corrupt[corrupt.size() - 1] = char(quint8(corrupt.at(corrupt.size() - 1)) ^ 1);
        QVERIFY(probe.receive(QByteArray(4096, 'x') + corrupt).isEmpty());
        QVERIFY(!probe.responded());
        QVERIFY(probe.receive(api.left(4)).isEmpty());
        QCOMPARE(probe.receive(api.mid(4)), QByteArray::fromHex("244d3c000202244d3c000303244d3c000404"));
        QVERIFY(probe.responded());
        const QByteArray identity = mspFrame(2, "BTFL") + mspFrame(3, QByteArray::fromHex("040501"))
            + mspFrame(4, QByteArray::fromHex("4f424634000002"));
        for (char byte : identity)
            QVERIFY(probe.receive(QByteArray(1, byte)).isEmpty());
        QCOMPARE(probe.variant(), QString("BTFL"));
        QCOMPARE(probe.boardTarget(), QString("OBF4"));
        QCOMPARE(probe.description(), QString("MSP API 1.42; firmware Betaflight 4.5.1; board target OBF4"));
        probe.stop();
        probe.receive(mspFrame(2, "INAV"));
        QCOMPARE(probe.variant(), QString("BTFL"));
    }
    void firmwareProbeDoesNotQueryUnsupportedApi()
    {
        MspFirmwareProbe probe;
        probe.start();
        QVERIFY(probe.receive(mspFrame(1, QByteArray::fromHex("000201"))).isEmpty());
        QVERIFY(probe.responded());
        probe.receive(mspFrame(2, "BTFL"));
        QVERIFY(probe.variant().isEmpty());
        QVERIFY(probe.description().contains("API 2.1"));
    }
    void firmwareProbeBoardInfoLengths()
    {
        MspFirmwareProbe probe;
        probe.start();
        probe.receive(mspFrame(4, QByteArray::fromHex("4f424634000002")));
        QVERIFY(probe.boardTarget().isEmpty());
        probe.receive(mspFrame(1, QByteArray::fromHex("00012a")));
        probe.receive(mspFrame(4, "OBF4")); // Too short for BOARD_INFO.
        QVERIFY(probe.boardTarget().isEmpty());
        probe.receive(mspFrame(4, QByteArray::fromHex("4f424634000002")));
        QCOMPARE(probe.boardTarget(), QString("OBF4"));
        QByteArray modern = QByteArray::fromHex("4f4d4e4900000200");
        modern.append(char(9));
        modern.append("OMNIBUSF4");
        probe.receive(mspFrame(4, modern));
        QCOMPARE(probe.boardTarget(), QString("OMNIBUSF4"));
        modern[8] = char(100); // Truncated target name: retain the valid identifier.
        probe.receive(mspFrame(4, modern));
        QCOMPARE(probe.boardTarget(), QString("OMNI"));
    }
    void catalogCoversEveryNativeFirmwareTarget()
    {
        const auto &catalog = Core::FirmwareCatalog::instance();
        QVERIFY2(catalog.error().isEmpty(), qPrintable(catalog.error()));
        QCOMPARE(catalog.boards().size(), 20);
        const QDir targets(QFINDTESTDATA("../../flight/targets"));
        QVERIFY(targets.exists());
        int count = 0;
        for (const auto &target : targets.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile definitions(targets.filePath(target + "/target-defs.mk"));
            if (!definitions.exists())
                continue; // Shared bootloader/packaging directories are not board targets.
            QVERIFY(definitions.open(QIODevice::ReadOnly));
            if (!QRegularExpression("(?m)^ALL_BOARDS\\s*\\+=\\s*" + target + "\\s*$")
                .match(QString::fromUtf8(definitions.readAll())).hasMatch())
                continue; // The POSIX simulator has no installable board image.
            const auto board = catalog.board(target);
            QVERIFY2(board.isValid(), qPrintable(target));
            QCOMPARE(board.mapping, QString("native"));
            QCOMPARE(board.target, target);
            QFile info(targets.filePath(target + "/board-info/board-info.mk"));
            QVERIFY(info.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(info.readAll());
            for (const auto &field : {QString("BOARD_TYPE"), QString("BOARD_REVISION")}) {
                const auto match = QRegularExpression(field + "\\s*:=\\s*(0x[0-9a-fA-F]+)").match(text);
                QVERIFY(match.hasMatch());
                QCOMPARE(field == "BOARD_TYPE" ? board.boardType : board.boardRevision,
                         match.captured(1).toInt(nullptr, 16));
            }
            ++count;
        }
        QCOMPARE(count, 19);
        QCOMPARE(catalog.board("TauLink").target, QString("pipxtreme"));
        QCOMPARE(catalog.board("Sprf3e").target, QString("sprf3e"));
        QVERIFY(!catalog.board("Simulation").isValid());
    }
    void catalogHglrcAndUnknownBoards()
    {
        const auto &catalog = Core::FirmwareCatalog::instance();
        const auto board = catalog.board("  hglrc f4v6pro  ");
        QCOMPARE(board.target, QString("revolution"));
        QCOMPARE(board.mapping, QString("compatible"));
        QCOMPARE(board.boardType, 9);
        QCOMPARE(board.boardRevision, 3);
        QCOMPARE(board.settings.size(), 5);
        QCOMPARE(Core::FirmwareCatalog::imageName(board, Core::FirmwareCatalog::ImageKind::FullInstall),
                 QString("ef_revolution.hex"));
        QCOMPARE(catalog.candidatesForMspTarget("obf4").size(), 1);
        QCOMPARE(catalog.candidatesForMspTarget("OMNIBUSF4").first().id, board.id);
        QVERIFY(catalog.candidatesForMspTarget("UNKNOWN").isEmpty());
        QVERIFY(!catalog.board("OmnibusF4").isValid()); // A shared USB product is not a retail model.
        QVERIFY(!catalog.board("HGLRC F405 V2").isValid());
        QVERIFY(Core::FirmwareCatalog::imagePath({}, Core::FirmwareCatalog::ImageKind::FullInstall).isEmpty());
    }
    void catalogRejectsInvalidMappingsAndKeepsMspAmbiguity()
    {
        QFile file(QFINDTESTDATA("../../shared/firmware/board-catalog.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = QJsonDocument::fromJson(file.readAll()).object();
        auto root = original;
        for (const auto &field : {"systemBootloader", "bootInstructions", "manualUrl"}) {
            for (const auto &value : {QJsonValue(42), QJsonValue(QString())}) {
                root = original;
                auto boards = root["boards"].toArray();
                auto hglrc = boards.last().toObject();
                hglrc[field] = value;
                boards[boards.size() - 1] = hglrc;
                root["boards"] = boards;
                QVERIFY(!Core::FirmwareCatalog::fromJson(QJsonDocument(root).toJson()).error().isEmpty());
            }
        }
        for (const auto &url : {"javascript:alert(1)", "file:///tmp/manual.pdf", "https:/manual.pdf"}) {
            root = original;
            auto boards = root["boards"].toArray();
            auto hglrc = boards.last().toObject();
            hglrc["manualUrl"] = url;
            boards[boards.size() - 1] = hglrc;
            root["boards"] = boards;
            QVERIFY(!Core::FirmwareCatalog::fromJson(QJsonDocument(root).toJson()).error().isEmpty());
        }
        root = original;
        root["schemaVersion"] = 2;
        QVERIFY(!Core::FirmwareCatalog::fromJson(QJsonDocument(root).toJson()).error().isEmpty());
        for (const auto &target : {QString("../revolution"), QString("no-such-target")}) {
            root = original;
            auto boards = root["boards"].toArray();
            auto hglrc = boards.last().toObject();
            hglrc["target"] = target;
            boards[boards.size() - 1] = hglrc;
            root["boards"] = boards;
            const auto invalid = Core::FirmwareCatalog::fromJson(QJsonDocument(root).toJson());
            QVERIFY(!invalid.error().isEmpty());
            QVERIFY(invalid.boards().isEmpty());
        }
        root = original;
        auto boards = root["boards"].toArray();
        auto first = boards.first().toObject();
        first["aliases"] = QJsonArray{"HGLRC F4V6PRO"};
        boards[0] = first;
        root["boards"] = boards;
        QVERIFY(!Core::FirmwareCatalog::fromJson(QJsonDocument(root).toJson()).error().isEmpty());
        first.remove("aliases");
        first["mspTargets"] = QJsonArray{"OBF4"};
        boards[0] = first;
        root["boards"] = boards;
        const auto ambiguous = Core::FirmwareCatalog::fromJson(QJsonDocument(root).toJson());
        QVERIFY(ambiguous.error().isEmpty());
        QCOMPARE(ambiguous.candidatesForMspTarget("OBF4").size(), 2);
    }
    void catalogResolvesImagesSeparatelyFromCompatibility()
    {
        using Catalog = Core::FirmwareCatalog;
        const auto hglrc = Catalog::instance().board("hglrc-f4-v6pro");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QStringList roots{directory.path()};
        QVERIFY(Catalog::imagePath(hglrc, Catalog::ImageKind::FullInstall, roots).isEmpty());
        QFile application(directory.filePath("fw_revolution.tlfw"));
        QVERIFY(application.open(QIODevice::WriteOnly));
        application.write("application fixture");
        application.close();
        QCOMPARE(Catalog::imagePath(hglrc, Catalog::ImageKind::Application, roots), application.fileName());
        QVERIFY(Catalog::imagePath(hglrc, Catalog::ImageKind::FullInstall, roots).isEmpty());
        QVERIFY(QDir(directory.path()).mkdir("ef_revolution"));
        QFile full(directory.filePath("ef_revolution/ef_revolution.hex"));
        QVERIFY(full.open(QIODevice::WriteOnly));
        full.close();
        QVERIFY(Catalog::imagePath(hglrc, Catalog::ImageKind::FullInstall, roots).isEmpty());
        QVERIFY(full.open(QIODevice::WriteOnly));
        full.write("full image fixture"); // Availability only; installer must validate actual Intel HEX.
        full.close();
        QCOMPARE(Catalog::imagePath(hglrc, Catalog::ImageKind::FullInstall, roots), full.fileName());
        auto unsafe = hglrc;
        unsafe.target = "../revolution";
        QVERIFY(Catalog::imagePath(unsafe, Catalog::ImageKind::FullInstall, roots).isEmpty());
    }
    void installedBoardProfilePersistsAndRestarts()
    {
        AttitudeSettings::GetInstance(objects)->getField("BoardRotation")->setValue(0, 2);
        HwRevolution::GetInstance(objects)->getField("SPIPeripheral")->setValue("Radio");
        HwRevolution::GetInstance(objects)->getField("MainPort")->setValue("GPS");
        FlightStatus::GetInstance(objects)->getField("Armed")->setValue("Disarmed");
        ProfileBoard flight(objects);
        BoardSettingsInstaller settings(objects);
        QSignalSpy result(&settings, &BoardSettingsInstaller::finished);
        settings.start(Core::FirmwareCatalog::instance().board("hglrc-f4-v6pro").settings);
        QTRY_COMPARE_WITH_TIMEOUT(result.size(), 1, 5000);
        QVERIFY(result.first().first().toBool());
        QCOMPARE(flight.writes, 2);
        QCOMPARE(flight.saved.size(), 2);
        QCOMPARE(AttitudeSettings::GetInstance(objects)->getField("BoardRotation")->getValue(2).toInt(), -9000);
        QCOMPARE(HwRevolution::GetInstance(objects)->getField("SPIPeripheral")->getValue().toString(), QString("MAX7456 OSD"));
        QCOMPARE(HwRevolution::GetInstance(objects)->getField("Magnetometer")->getValue().toString(), QString("None"));
        QCOMPARE(HwRevolution::GetInstance(objects)->getField("MainPort")->getValue().toString(), QString("GPS"));
        QCOMPARE(flight.rebootCommands, QList<int>({1122, 2233, 4455}));
    }
    void installedBoardProfileRejectsFailures_data()
    {
        QTest::addColumn<QString>("failure");
        for (const auto &name : {"field", "overflow", "enum", "element", "duplicate", "nack", "save", "retention", "disconnect", "armed"})
            QTest::newRow(name) << QString(name);
    }
    void installedBoardProfileRejectsFailures()
    {
        QFETCH(QString, failure);
        AttitudeSettings::GetInstance(objects)->getField("BoardRotation")->setValue(0, 2);
        FlightStatus::GetInstance(objects)->getField("Armed")->setValue(failure == "armed" ? "Armed" : "Disarmed");
        ProfileBoard flight(objects);
        flight.rejectWrite = failure == "nack"; flight.saveError = failure == "save";
        flight.corruptSave = failure == "retention";
        BoardSettingsInstaller settings(objects);
        QSignalSpy result(&settings, &BoardSettingsInstaller::finished);
        auto profile = Core::FirmwareCatalog::instance().board("hglrc-f4-v6pro").settings;
        auto entry = profile[2].toObject();
        if (failure == "field") entry["field"] = "Nonexistent";
        if (failure == "overflow") entry["value"] = 40000;
        if (failure == "element") entry["element"] = "Nonexistent";
        if (failure == "enum") { entry["object"] = "HwRevolution"; entry["field"] = "SPIPeripheral"; entry.remove("element"); entry["value"] = "Nonexistent"; }
        profile[2] = entry;
        if (failure == "duplicate") profile.append(entry);
        settings.start(profile);
        if (failure == "disconnect") settings.disconnected();
        QTRY_COMPARE(result.size(), 1);
        QVERIFY(!result.first().first().toBool());
        QVERIFY(!result.first().last().toString().isEmpty());
        QVERIFY(flight.rebootCommands.isEmpty());
        if (failure == "field" || failure == "overflow" || failure == "enum" || failure == "element" || failure == "duplicate")
            QCOMPARE(flight.writes, 0);
    }
    void firmwarePageManualSelectionAndUnknownTarget()
    {
        manager->setFirmwareIdentity("BTFL", "UNKNOWN", true, "3.2.3");
        auto *board = page->findChild<QComboBox *>("firmwareBoardModel");
        QVERIFY(board);
        QCOMPARE(board->currentIndex(), 0);
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), QString("Not identified"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedTarget")->text(), QString("UNKNOWN"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedFirmware")->text(), QString("Betaflight 3.2.3"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBootloader")->text(), QString("Not reported"));
        board->setCurrentIndex(board->findData("hglrc-f4-v6pro"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), QString("HGLRC F4 V6PRO"));
        board->setCurrentIndex(board->findData("sparky"));
        QCOMPARE(board->currentData().toString(), QString("sparky"));
        board->setCurrentIndex(0);
        QVERIFY(page->findChild<QLabel *>("firmwareRecommendation")->text().contains("Unlisted boards"));
    }
    void firmwareControllerCandidates_data()
    {
        QTest::addColumn<quint16>("vendor"); QTest::addColumn<quint16>("product");
        QTest::addColumn<QString>("description"); QTest::addColumn<bool>("candidate");
        QTest::newRow("betaflight-stm32") << quint16(0x0483) << quint16(0x5740) << QString("OmnibusF4") << true;
        QTest::newRow("native-dronin") << quint16(0x20a0) << quint16(0x4250) << QString("USB device") << true;
        QTest::newRow("catalog-model") << quint16(0x1234) << quint16(0x5678) << QString("HGLRC F4 V6PRO") << true;
        QTest::newRow("firmware-name") << quint16(0x1234) << quint16(0x5678) << QString("Betaflight STM32F7") << true;
        QTest::newRow("usb-serial-adapter") << quint16(0x1a86) << quint16(0x7523) << QString("USB2.0-Serial") << false;
        QTest::newRow("keyboard") << quint16(0x046d) << quint16(0xc31c) << QString("USB Keyboard") << false;
        QTest::newRow("builtin-serial") << quint16(0) << quint16(0) << QString("ttyS0") << false;
        QTest::newRow("missing-usb-identity") << quint16(0) << quint16(0) << QString("Betaflight") << false;
    }
    void firmwareControllerCandidates()
    {
        QFETCH(quint16, vendor); QFETCH(quint16, product); QFETCH(QString, description); QFETCH(bool, candidate);
        QCOMPARE(Core::IDevice::plausibleControllerUsb(vendor, product, description), candidate);
    }
    void firmwareConnectPageFiltersHotplugAndConnectsSelectedController()
    {
        TestConnection first("OmnibusF4 - ttyACM0", "1-1");
        TestConnection adapter("USB serial adapter - ttyUSB0", "1-2");
        adapter.device->setControllerCandidate(false); emit adapter.availableDevChanged(&adapter);
        auto *list = page->findChild<QComboBox *>("firmwareControllerList");
        auto *connectButton = page->findChild<QPushButton *>("firmwareConnectAction");
        QCOMPARE(list->count(), 1); QCOMPARE(list->currentText(), first.device->getDisplayName());
        QVERIFY(connectButton->isEnabled());
        connectButton->ensurePolished();
        QCOMPARE(connectButton->palette().color(QPalette::ButtonText),
                 QApplication::palette().color(QPalette::HighlightedText));
        QVERIFY(manager->isVisible()); // The telemetry footer is shared by every workspace.
        QCOMPARE(page->findChild<QLabel *>("firmwareUsbTitle")->text(), QString("USB controller detected"));
        QVERIFY(!page->findChild<QLabel *>("firmwareUsbHint")->text().contains("Plug"));
        QVERIFY(!page->findChild<QPushButton *>("firmwareInstallAction")->isVisible());
        QVERIFY(!page->findChild<QWidget *>("firmwareRecoveryBody")->isVisible());
        captureFirmwarePage("firmware-controller-found");
        first.attachController();
        QCOMPARE(list->count(), 2); QCOMPARE(list->currentText(), first.device->getDisplayName());
        list->setCurrentIndex(1);
        emit first.availableDevChanged(&first);
        QCOMPARE(list->currentText(), QString("New controller"));
        list->showPopup(); QCoreApplication::processEvents();
        QVERIFY(list->view()->window()->isVisible());
        QCOMPARE(list->view()->palette().color(QPalette::Base), QApplication::palette().color(QPalette::Base));
        QCOMPARE(list->view()->palette().color(QPalette::Highlight), QApplication::palette().color(QPalette::Highlight));
        const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captures.isEmpty()) QVERIFY(list->view()->window()->grab().save(captures + "/firmware-controller-popup.png"));
        list->hidePopup();
        QTest::mouseClick(connectButton, Qt::LeftButton);
        QVERIFY(manager->isConnected()); QCOMPARE(manager->getCurrentDevice().device->getName(), QString("new-serial-port"));
        QVERIFY(!page->findChild<QPushButton *>("firmwareInstallAction")->isVisible());
        QVERIFY(!page->findChild<QWidget *>("firmwareConnectCard")->isVisible());
        captureFirmwarePage("firmware-connecting");
        auto *disconnect = page->findChild<QPushButton *>("firmwareDisconnectAction");
        QTRY_VERIFY(disconnect->height() > 0);
        QTest::mouseClick(disconnect, Qt::LeftButton);
        QVERIFY(!manager->isConnected()); QVERIFY(page->findChild<QWidget *>("firmwareConnectCard")->isVisible());
    }
    void firmwareUsbGuidanceTracksPresence()
    {
        auto *usb = page->findChild<QLabel *>("firmwareUsbTitle");
        auto *list = page->findChild<QComboBox *>("firmwareControllerList");
        QCOMPARE(list->count(), 0);
        QCOMPARE(usb->text(), QString("Plug in your controller"));
        {
            TestConnection controller;
            QCOMPARE(list->count(), 1);
            QCOMPARE(usb->text(), QString("USB controller detected"));
        }
        QCOMPARE(list->count(), 0);
        QCOMPARE(usb->text(), QString("Plug in your controller"));
        QVERIFY(!page->findChild<QPushButton *>("firmwareConnectAction")->isEnabled());
    }
    void firmwareControllerListPrefersNativeHidForSameBoard()
    {
        TestConnection serial("Controller serial", "1-1");
        TestConnection native("Controller USB", "1-1");
        native.transport = "USB"; emit native.availableDevChanged(&native);
        auto *list = page->findChild<QComboBox *>("firmwareControllerList");
        QCOMPARE(list->count(), 1); QCOMPARE(list->currentText(), QString("Controller USB"));
    }
    void unrelatedUsbAttachmentDoesNotChangeWorkspace()
    {
        TestConnection adapter("USB serial adapter", "1-4");
        adapter.device->setControllerCandidate(false); emit adapter.availableDevChanged(&adapter);
        QSignalSpy requested(manager, &Core::ConnectionManager::firmwarePageRequested);
        adapter.attachController(false);
        QCOMPARE(requested.count(), 0);
        // A device re-enumeration containing only the unchanged adapter must stay quiet.
        emit adapter.availableDevChanged(&adapter);
        QCOMPARE(requested.count(), 0);
    }
    void firmwareBootSelectionCanReturnToConnection()
    {
        auto *back = page->findChild<QToolButton *>("firmwareBackAction");
        QVERIFY(!back->isVisible());
        auto *maintenance = page->findChild<QToolButton *>("firmwareRecoveryToolsToggle");
        maintenance->setChecked(true);
        auto *boot = page->findChild<QPushButton *>("firmwareMaintenanceInstallAction");
        page->findChild<QScrollArea *>()->ensureWidgetVisible(boot);
        QCoreApplication::processEvents();
        QTest::mouseClick(boot, Qt::LeftButton);
        QVERIFY(back->isVisible());
        QVERIFY(!page->findChild<QWidget *>("firmwareConnectCard")->isVisible());
        auto *boards = page->findChild<QComboBox *>("firmwareBoardModel");
        boards->setCurrentIndex(boards->findData("hglrc-f4-v6pro"));
        QVERIFY(page->findChild<QLabel *>("firmwareDetectedBootloader")->text().startsWith("STM32 system bootloader"));
        QCOMPARE(page->findChild<QLabel *>("firmwareStatus")->text(), QString("Install the recommended firmware to continue."));
        QVERIFY(page->findChild<QPushButton *>("firmwareInstallAction")->isVisible());
        page->findChild<QScrollArea *>()->ensureWidgetVisible(back);
        QCoreApplication::processEvents();
        QTest::mouseClick(back, Qt::LeftButton);
        QVERIFY(page->findChild<QWidget *>("firmwareConnectCard")->isVisible());
        QCOMPARE(page->findChild<QLabel *>("firmwarePageTitle")->text(), QString("Connect your controller"));
    }
    void connectingRequestsFirmwareAndTimeoutStaysInline()
    {
        TestConnection connection("OmnibusF4 - ttyACM0");
        QSignalSpy requested(manager, &Core::ConnectionManager::firmwarePageRequested);
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        QCOMPARE(requested.count(), 1);
        QVERIFY(manager->firmwareDetectionActive());
        auto *progress = page->findChild<QProgressBar *>("firmwareDetectionProgress");
        QVERIFY(progress && progress->isVisible());
        QCOMPARE(progress->maximum(), 0);
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedFirmware")->text(), QString("Detecting…"));
        const QString captureDirectory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captureDirectory.isEmpty()) QVERIFY(page->grab().save(captureDirectory + "/firmware-detecting.png"));
        manager->setFirmwareIdentity("BTFL", "OBF4", true, "3.2.3");
        manager->telemetryConnectionFailed({"Install the recommended firmware.", {}, "rxBytes=0"});
        QVERIFY(!manager->firmwareDetectionActive());
        QVERIFY(!progress->isVisible());
        QCOMPARE(requested.count(), 1);
        QVERIFY(!page->findChild<QLabel *>("connectionErrorGuidance")->isVisible());
        QVERIFY(!manager->window()->findChild<QDialog *>("telemetryErrorDialog"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedFirmware")->text(), QString("Betaflight 3.2.3"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBootloader")->text(),
                 QString("STM32 system bootloader (USB DFU)"));
        QVERIFY(!page->findChild<QLabel *>("firmwareDetectedBoard")->isVisible());
        QVERIFY(page->findChild<QComboBox *>("firmwareBoardModel")->isVisible());
        auto *recommendation = page->findChild<QLabel *>("firmwareRecommendation");
        QVERIFY(!recommendation->isVisible() || !recommendation->text().contains("Recommended firmware:"));
        if (page->findChild<QPushButton *>("firmwareInstallAction")->isEnabled())
            QVERIFY(!recommendation->isVisible());
        QVERIFY(!page->findChild<QLabel *>("firmwareStatus")->isVisible());
        QVERIFY(page->findChild<QLabel *>("firmwareDetectedBootloader")->isVisible());
        QCOMPARE(page->findChild<QLabel *>("firmwareCurrentHeading")->text(), QString("Currently installed"));
        QVERIFY(page->findChild<QWidget *>("firmwareInstallSummary")->isVisible());
        QVERIFY(page->findChild<QLabel *>("firmwareToInstall")->text().contains("Revolution"));
        QCOMPARE(page->findChild<QLabel *>("firmwareBootloaderToInstall")->text(), QString("DroniNG"));
        QVERIFY(page->findChild<QLabel *>("firmwareSettingsToInstall")->isVisible());
        QVERIFY(!page->findChild<QWidget *>("firmwareControllerCard")->isAncestorOf(
            page->findChild<QToolButton *>("connectionErrorDetailsToggle")));
        captureFirmwarePage("firmware-betaflight-detected");
        manager->beginFirmwareInstallation();
        manager->telemetryConnectionFailed({"late timeout", {}, {}});
        QVERIFY(!manager->firmwareError().message.contains("late timeout"));
        QVERIFY(manager->firmwareVersion().isEmpty());
        manager->endFirmwareInstallation();
    }
    void initialDeviceInventoryDoesNotSelectFirmware()
    {
        QSignalSpy requested(manager, &Core::ConnectionManager::firmwarePageRequested);
        TestConnection connection("OmnibusF4 - ttyACM0", "1-2");
        QCOMPARE(requested.count(), 0);
        connection.attachController();
        QCOMPARE(requested.count(), 1);
        emit connection.availableDevChanged(&connection);
        QCOMPARE(requested.count(), 1);
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        QCOMPARE(requested.count(), 2);
        QVERIFY(manager->firmwareDetectionActive());
        manager->disconnectDevice();
        QVERIFY(!page->findChild<QProgressBar *>("firmwareDetectionProgress")->isVisible());
    }
    void radixOffersItsOwnImage_data()
    {
        QTest::addColumn<QString>("target");
        QTest::newRow("full-target-name") << QString("RADIX");
        QTest::newRow("legacy-four-byte-identifier") << QString("RADI");
    }
    void radixOffersItsOwnImage()
    {
        QFETCH(QString, target);
        TestConnection connection("BrainFPV RADIX - ttyACM0");
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setFirmwareIdentity("BTFL", target, true, "4.5.1");
        manager->telemetryConnectionFailed({"Foreign firmware detected", {}, {}});
        QCOMPARE(page->findChild<QLabel *>("firmwarePageTitle")->text(), QString("Install DroniNG"));
        auto *recommendation = page->findChild<QLabel *>("firmwareRecommendation");
        QVERIFY(recommendation->isVisible());
        QVERIFY(recommendation->text().contains("original RADIX"));
        QVERIFY(recommendation->text().contains("PWM/OneShot"));
        QVERIFY(recommendation->text().contains("FPGA OSD"));
        QVERIFY(recommendation->text().contains("DShot and SD logging are not available"));
        QVERIFY(!recommendation->text().contains("experimental", Qt::CaseInsensitive));
        QVERIFY(!recommendation->text().contains("validation", Qt::CaseInsensitive));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), QString("BrainFPV RADIX"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedFirmware")->text(), QString("Betaflight 4.5.1"));
        QVERIFY(page->findChild<QComboBox *>("firmwareBoardModel")->isVisible());
        QCOMPARE(page->findChild<QComboBox *>("firmwareBoardModel")->currentData().toString(), QString("radix"));
        QVERIFY(page->findChild<QPushButton *>("firmwareInstallAction")->isVisible());
        QVERIFY(page->findChild<QPushButton *>("firmwareInstallAction")->isEnabled());
        QCOMPARE(page->findChild<QPushButton *>("firmwareInstallAction")->text(), QString("Install DroniNG"));
        QCOMPARE(page->findChild<QLabel *>("firmwareBootloaderToInstall")->text(), QString("STM32 ROM · retained"));
        const auto radix = Core::FirmwareCatalog::instance().board("radix");
        QVERIFY(Core::FirmwareCatalog::imageName(radix, Core::FirmwareCatalog::ImageKind::Application).isEmpty());
        QVERIFY(Core::FirmwareCatalog::imageName(radix, Core::FirmwareCatalog::ImageKind::BootloaderUpdate).isEmpty());
        QVERIFY(page->findChild<QPushButton *>("firmwareDisconnectAction")->isVisible());
        captureFirmwarePage("firmware-radix-image");
        // Reconnecting a board with a reviewed mapping restores its offer.
        manager->disconnectDevice();
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setFirmwareIdentity("BTFL", "OBF4", true, "3.2.3");
        manager->telemetryConnectionFailed({"Foreign firmware detected", {}, {}});
        QCOMPARE(page->findChild<QComboBox *>("firmwareBoardModel")->currentData().toString(), QString("hglrc-f4-v6pro"));
        QVERIFY(page->findChild<QPushButton *>("firmwareInstallAction")->isVisible());
    }
    void truncatedRadixDoesNotSelectOtherModels_data()
    {
        QTest::addColumn<QString>("product");
        QTest::newRow("LI") << QString("BrainFPV RADIX LI - ttyACM0");
        QTest::newRow("2") << QString("BrainFPV RADIX 2 - ttyACM0");
        QTest::newRow("unknown") << QString("Betaflight - ttyACM0");
    }
    void truncatedRadixDoesNotSelectOtherModels()
    {
        QFETCH(QString, product); TestConnection connection(product);
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setFirmwareIdentity("BTFL", "RADI", true, "4.5.1");
        manager->telemetryConnectionFailed({"Foreign firmware detected", {}, {}});
        QCOMPARE(page->findChild<QComboBox *>("firmwareBoardModel")->currentIndex(), 0);
        QVERIFY(!page->findChild<QPushButton *>("firmwareInstallAction")->isEnabled());
    }
    void omnibusOffersItsOwnImage_data()
    {
        QTest::addColumn<QString>("target");
        QTest::newRow("reported-target") << QString("OMNIBUSF4SD");
        QTest::newRow("reported-four-byte-identifier") << QString("OBSD");
    }
    void omnibusOffersItsOwnImage()
    {
        QFETCH(QString, target);
        TestConnection connection("OmnibusF4 - ttyACM0");
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setFirmwareIdentity("BTFL", target, true, "3.4.0");
        manager->telemetryConnectionFailed({"Foreign firmware detected", {}, {}});
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), QString("Omnibus F4 SD"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedFirmware")->text(), QString("Betaflight 3.4.0"));
        QCOMPARE(page->findChild<QComboBox *>("firmwareBoardModel")->currentData().toString(), QString("omnibusf4sd"));
        auto *install = page->findChild<QPushButton *>("firmwareInstallAction");
        QVERIFY(install->isVisible()); QVERIFY(install->isEnabled());
        QCOMPARE(install->text(), QString("Install DroniNG"));
        const auto text = page->findChild<QLabel *>("firmwareRecommendation")->text();
        QVERIFY(text.contains("analog OSD")); QVERIFY(text.contains("UART6"));
        QVERIFY(!text.contains("experimental", Qt::CaseInsensitive));
        const auto board = Core::FirmwareCatalog::instance().board("omnibusf4sd");
        QCOMPARE(board.boardType, 225);
        QVERIFY(Core::FirmwareCatalog::imageName(board, Core::FirmwareCatalog::ImageKind::Application).isEmpty());
        captureFirmwarePage("firmware-omnibus-image");
    }
    void importedNativeConfigurationUsesCommonFields_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<QString>("name");
        QTest::newRow("Radix") << 224 << QString("BrainFPV RADIX");
        QTest::newRow("OmnibusF4SD") << 225 << QString("Omnibus F4 SD");
    }
    void importedNativeConfigurationUsesCommonFields()
    {
        QFETCH(int, type); QFETCH(QString, name); const bool radix = type == 224;
        TelemetryManager telemetry; UAVObjectUtilManager util;
        plugins.addObject(&telemetry); plugins.addObject(&util);
        const auto remove = qScopeGuard([&] { plugins.removeObject(&util); plugins.removeObject(&telemetry); });
        QPluginLoader brain(QDir(GCS_BUILD_TREE_PATH).filePath("lib/droning/plugins/droniNG/libDroning.so"));
        auto *plugin = qobject_cast<ExtensionSystem::IPlugin *>(brain.instance());
        QVERIFY2(plugin, qPrintable(brain.errorString()));
        QString error; QVERIFY2(plugin->initialize({}, &error), qPrintable(error));
        plugin->extensionsInitialized();
        const auto unload = qScopeGuard([&] { brain.unload(); });
        Core::IBoardType *board = nullptr;
        for (auto *candidate : plugins.getObjects<Core::IBoardType>())
            if (candidate->getBoardType() == type) board = candidate;
        QVERIFY(board); QCOMPARE(board->shortName(), name);
        QCOMPARE(board->getHwUAVO(), QString("HwImported"));
        QCOMPARE(board->queryMaxGyroRate(), radix ? 1600 : 1000);
        QCOMPARE(board->getChannelBanks(), (radix ? QVector<QVector<int>>{{1, 2}, {3}, {4}} : QVector<QVector<int>>{{1, 2}, {3, 4}}));
        QVERIFY(!board->supportsDShot());
        QCOMPARE(board->getAdcNames(), (QStringList{"I", "V", "R"}));
        QVERIFY(!board->queryCapabilities(Core::IBoardType::BOARD_CAPABILITIES_UPGRADEABLE));
        QVERIFY(!board->isInputConfigurationSupported(Core::IBoardType::INPUT_TYPE_PPM));
        auto *settings = HwImported::GetInstance(objects); const auto previous = settings->getData();
        const bool wasPresent = settings->getIsPresentOnHardware(); settings->setIsPresentOnHardware(true);
        const auto restore = qScopeGuard([&] { settings->setData(previous); settings->setIsPresentOnHardware(wasPresent); });
        QVERIFY(board->setInputType(Core::IBoardType::INPUT_TYPE_SBUS));
        QCOMPARE(board->getInputType(), Core::IBoardType::INPUT_TYPE_SBUS);
        std::unique_ptr<QWidget> widget(board->getBoardConfiguration(nullptr, true));
        widget->resize(650, 550); widget->show(); QCoreApplication::processEvents();
        QCOMPARE(widget->findChild<QComboBox *>("importedReceiver")->currentText(), QString("S.Bus"));
        auto fields = QStringList{"Telemetry", "GPS", "USB_VCPPort"};
        if (radix) fields << "ReceiverTelemetry" << "BuzzerType";
        else fields << "ReceiverPort";
        for (const auto &name : fields) {
            auto *combo = widget->findChild<QComboBox *>(QString("imported") + name);
            QVERIFY(combo); QVERIFY(combo->count() > 1);
        }
        if (!radix) {
            QVERIFY(!widget->findChild<QComboBox *>("importedReceiverTelemetry"));
            QVERIFY(!widget->findChild<QComboBox *>("importedBuzzerType"));
            auto *port = widget->findChild<QComboBox *>("importedReceiverPort");
            const auto hasLabel = [&widget](const QString &text) {
                for (auto *label : widget->findChildren<QLabel *>()) if (label->text() == text) return true;
                return false;
            };
            port->setCurrentIndex(port->findText("UART6"));
            QVERIFY(hasLabel("Receiver · UART6")); QVERIFY(hasLabel("GPS · UART3"));
            port->setCurrentIndex(port->findText("UART3"));
            QVERIFY(hasLabel("Receiver · UART3")); QVERIFY(hasLabel("GPS · UART6"));
        }
        auto *discard = widget->findChild<QPushButton *>("settingsDiscard");
        QVERIFY(discard);
        QVERIFY(widget->findChild<QWidget *>("settingsActions"));
        bool hasPortInstructions = false;
        for (auto *label : widget->findChildren<QLabel *>()) {
            QVERIFY(!label->text().contains("experimental", Qt::CaseInsensitive));
            hasPortInstructions |= label->text() == "Save and reboot after changing ports.";
        }
        QVERIFY(hasPortInstructions);
        const auto directory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!directory.isEmpty()) {
            const QString filename = radix ? "/radix-native-configuration.png" : "/omnibus-native-configuration.png";
            QVERIFY(widget->grab().save(directory + filename));
            widget->resize(1236, 950); QCoreApplication::processEvents();
            QVERIFY(QDir().mkpath(directory + "/fullscreen"));
            QVERIFY(widget->grab().save(directory + "/fullscreen" + filename));
        }
    }
    void controllerRecoveryOffersDetectedBoardWorkflow_data()
    {
        QTest::addColumn<QString>("boardId");
        QTest::newRow("radix") << QString("radix");
        QTest::newRow("omnibus") << QString("omnibusf4sd");
        QTest::newRow("hglrc") << QString("hglrc-f4-v6pro");
    }
    void controllerRecoveryOffersDetectedBoardWorkflow()
    {
        QFETCH(QString, boardId);
        const auto board = Core::FirmwareCatalog::instance().board(boardId);
        TestConnection controller(board.name, "2-4"); controller.transport = "USB";
        const Core::DevListItem device(&controller, controller.device);
        QVERIFY(manager->autoConnectDevice(device));
        emit controller.transportDisconnected(&controller);
        QCOMPARE(manager->controllerRecovery().boardId, board.id);
        auto *retry = page->findChild<QPushButton *>("firmwareRetryConnection");
        auto *recover = page->findChild<QPushButton *>("firmwareRecoverInstallation");
        auto *maintenance = page->findChild<QToolButton *>("firmwareRecoveryToolsToggle");
        QVERIFY(retry->isVisible()); QVERIFY(retry->isEnabled());
        QVERIFY(recover->isVisible()); QVERIFY(recover->isEnabled());
        const auto hint = page->findChild<QLabel *>("firmwareRecoveryHint")->text();
        const auto primary = recover->property("primary");
        QVERIFY(!maintenance->isChecked());
        QVERIFY(!page->findChild<QWidget *>("firmwareConnectCard")->isVisible());
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), board.name);
        // Inventory destruction must not erase the board or offer another controller's port.
        controller.detachController();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(retry->isEnabled());
        QCOMPARE(page->findChild<QLabel *>("firmwareRecoveryHint")->text(), hint);
        QCOMPARE(recover->property("primary"), primary);
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), board.name);
        const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        for (const int width : {640, 1280}) {
            page->resize(width, 900); QCoreApplication::processEvents();
            QVERIFY(recover->isVisible());
            QVERIFY(page->findChild<QScrollArea *>("firmwareScrollArea")->horizontalScrollBar()->maximum() == 0);
            if (!captures.isEmpty()) QVERIFY(page->grab().save(captures + QString("/recovery-%1-%2.png").arg(board.id).arg(width)));
        }
        QTest::mouseClick(recover, Qt::LeftButton);
        auto *installation = page->findChild<Core::FirmwareInstallWidget *>();
        QVERIFY(installation);
        const QPointer<Core::FirmwareInstallWidget> activeInstallation(installation);
        const auto cancelOnFailure = qScopeGuard([activeInstallation] {
            if (activeInstallation) activeInstallation->cancelInstallation();
        });
        QTRY_VERIFY(installation->isVisible());
        QVERIFY(!installation->autoFillBackground());
        QVERIFY(!installation->findChild<QWidget *>("firmwareInstallTaskCard")->property("uiCard").toBool());
        QVERIFY(!installation->findChild<QLabel *>("firmwareInstallHeading")->isVisible());
        QVERIFY(page->findChild<QWidget *>("firmwareOverview")->isAncestorOf(installation));
        QCOMPARE(page->findChild<QLabel *>("firmwarePageTitle")->text(), installation->title());
        QCOMPARE(installation->findChild<QLabel *>("firmwareInstallBootStep1")->text(), board.bootInstructions.section('\n', 0, 0));
        QVERIFY(!maintenance->isChecked());
        QCoreApplication::processEvents();
        if (!captures.isEmpty()) QVERIFY(page->grab().save(captures + QString("/recovery-%1-boot.png").arg(board.id)));
        // Returning from BOOT instructions retains the failed controller and its actions.
        installation->cancelInstallation();
        QVERIFY(!page->findChild<Core::FirmwareInstallWidget *>());
        QVERIFY(recover->isVisible());
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), board.name);
        TestConnection reconnected(board.name, "2-4"); reconnected.transport = "USB";
        emit reconnected.availableDevChanged(&reconnected);
        QVERIFY(!manager->isConnected());
        QVERIFY(retry->isEnabled());
        QTest::mouseClick(retry, Qt::LeftButton);
        QVERIFY(manager->isConnected()); QVERIFY(manager->firmwareDetectionActive());
        manager->setNativeFirmwareIdentity(board.name, "DroniNG");
        manager->reportFlightDataReceived();
        QVERIFY(!recover->isVisible()); QVERIFY(!retry->isVisible());
        QVERIFY(manager->controllerRecovery().problem.message.isEmpty());
    }
    void controllerRecoveryWaitsForRequestedControllerOnly()
    {
        TestConnection failed("OMNIBUSF4SD", "2-4"); failed.transport = "USB";
        QVERIFY(manager->connectDevice({&failed, failed.device}));
        emit failed.transportDisconnected(&failed);
        failed.detachController();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto *retry = page->findChild<QPushButton *>("firmwareRetryConnection");
        QVERIFY(retry->isEnabled());
        QTest::mouseClick(retry, Qt::LeftButton);
        QVERIFY(!retry->isEnabled());
        QVERIFY(page->findChild<QLabel *>("firmwareRecoveryHint")->text().contains("Reconnect USB"));
        TestConnection other("BrainFPV RADIX", "2-4"); other.transport = "USB";
        emit other.availableDevChanged(&other);
        QVERIFY(!manager->isConnected());
        QVERIFY(!retry->isEnabled());
        other.detachController();
        TestConnection ready("OMNIBUSF4SD", "2-4"); ready.transport = "USB";
        emit ready.availableDevChanged(&ready);
        QVERIFY(manager->isConnected());
        QCOMPARE(manager->getCurrentDevice().device.data(), ready.device);
    }
    void postInstallationUsbOpenFailuresHaveBoundedRetries()
    {
        const auto board = Core::FirmwareCatalog::instance().board("omnibusf4sd");
        const auto image = Core::FirmwareInstallImage::load(board,
            Core::FirmwareCatalog::imagePath(board, Core::FirmwareCatalog::ImageKind::FullInstall));
        QVERIFY(image.valid());
        manager->beginFirmwareInstallation();
        const auto finish = qScopeGuard([this] { manager->endFirmwareInstallation(); });
        manager->recordVerifiedInstallation(board.id, "2-4", image.descriptor);
        TestConnection settling("OMNIBUSF4SD", "2-4"); settling.transport = "USB"; settling.failOpen = true;
        const Core::DevListItem device(&settling, settling.device);
        for (int attempt = 0; attempt < 6; ++attempt) {
            QVERIFY(!manager->autoConnectDevice(device));
            QVERIFY(!manager->automaticConnectionBlocked("2-4"));
            QVERIFY(manager->controllerRecovery().problem.message.isEmpty());
        }
        settling.failOpen = false;
        QVERIFY(manager->autoConnectDevice(device));
        // A real startup reset must still stop immediately, after open succeeds.
        emit settling.transportDisconnected(&settling);
        QVERIFY(manager->automaticConnectionBlocked("2-4"));
        manager->resetAutomaticConnectionFailures("2-4");
        settling.failOpen = true;
        for (int attempt = 0; attempt < 7; ++attempt) QVERIFY(!manager->autoConnectDevice(device));
        QVERIFY(manager->automaticConnectionBlocked("2-4"));
        QVERIFY(manager->controllerRecovery().problem.details.contains("simulated permission denied"));
        QVERIFY(!manager->autoConnectDevice(device));
    }
    void rawHidOpenFailureIncludesNodeAndOperatingSystemError()
    {
        class Inventory : public RawHIDConnection {
        public:
            using RawHIDConnection::devicesForPorts;
        } connection;
        USBPortInfo port{};
        port.path = "/dev/droning-nonexistent-test-hid";
        port.serialNumber = "test-missing-hid";
        auto *device = static_cast<USBDevice *>(connection.devicesForPorts({port}).first());
        RawHID io(device);
        QVERIFY(!io.open(QIODevice::ReadWrite));
        QVERIFY(io.errorString().contains(device->getPath()));
        QVERIFY(io.errorString().contains("errno 2"));
        QVERIFY(!io.errorString().contains("Unknown error"));
    }
    void rawHidWorkerFailuresCloseTransport_data()
    {
        QTest::addColumn<bool>("readFailure");
        QTest::addColumn<int>("writeResult");
        QTest::addColumn<int>("failureErrno");
        QTest::newRow("unplugged-reader") << true << 64 << ENODEV;
        QTest::newRow("timed-out-writer") << false << -1 << ETIMEDOUT;
        QTest::newRow("io-error-writer") << false << -1 << EIO;
        QTest::newRow("short-report-writer") << false << 20 << 0;
    }
    void rawHidWorkerFailuresCloseTransport()
    {
        QFETCH(bool, readFailure);
        QFETCH(int, writeResult);
        QFETCH(int, failureErrno);
        HidTestInventory inventory;
        std::atomic_int closes{0}, writes{0};
        const RawHIDBackend backend{
            [](const char *) { return reinterpret_cast<hid_device *>(quintptr(1)); },
            [&](hid_device *, unsigned char *, size_t, int) {
                if (readFailure) { errno = failureErrno; return -1; }
                QThread::msleep(5); return 0;
            },
            [&](hid_device *, const unsigned char *, size_t) {
                ++writes; errno = failureErrno; return writeResult;
            },
            [&](hid_device *) { ++closes; }
        };
        MessageCapture messages;
        RawHID io(inventory.device, backend);
        QSignalSpy failed(&io, &RawHID::transportFailed);
        QSignalSpy closed(&io, &QIODevice::aboutToClose);
        QSignalSpy written(&io, &QIODevice::bytesWritten);
        QVERIFY(io.open(QIODevice::ReadWrite));
        if (!readFailure) QCOMPARE(io.write("request", 7), qint64(7));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!io.isOpen());
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closes.load(), 1);
        QCOMPARE(written.size(), 0); // Accepting bytes is not a successful USB write.
        QCOMPARE(writes.load(), readFailure ? 0 : 1);
        QVERIFY(io.errorString().contains(inventory.device->getPath()));
        QVERIFY(io.errorString().contains(readFailure ? "read" : "write"));
        if (writeResult == 20) QVERIFY(io.errorString().contains("20 of 64"));
        else QVERIFY(io.errorString().contains(QString("errno %1").arg(failureErrno)));
        QCOMPARE(io.write("late", 4), qint64(-1));
        io.close();
        QCOMPARE(closes.load(), 1);
    }
    void rawHidTransientWriteRetriesResetAfterSuccessfulReport()
    {
        HidTestInventory inventory;
        std::atomic_int attempts{0}, closes{0};
        QList<QByteArray> reports;
        const RawHIDBackend backend{
            [](const char *) { return reinterpret_cast<hid_device *>(quintptr(1)); },
            [](hid_device *, unsigned char *, size_t, int) { QThread::msleep(5); return 0; },
            [&](hid_device *, const unsigned char *data, size_t size) {
                const int attempt = ++attempts;
                if (attempt % 8) { errno = EAGAIN; return -1; }
                reports.append(QByteArray(reinterpret_cast<const char *>(data), size));
                return int(size);
            },
            [&](hid_device *) { ++closes; }
        };
        RawHID io(inventory.device, backend);
        QSignalSpy failed(&io, &RawHID::transportFailed);
        QSignalSpy written(&io, &QIODevice::bytesWritten);
        QVERIFY(io.open(QIODevice::ReadWrite));
        const QByteArray payload(100, 'x');
        QCOMPARE(io.write(payload), qint64(payload.size()));
        QTRY_COMPARE(written.size(), 2);
        QCOMPARE(attempts.load(), 16);
        QCOMPARE(written[0][0].toLongLong(), qint64(62));
        QCOMPARE(written[1][0].toLongLong(), qint64(38));
        QVERIFY(failed.isEmpty());
        io.close(); // Join workers before inspecting their report list.
        QCOMPARE(closes.load(), 1);
        QCOMPARE(reports.size(), 2);
        QCOMPARE(reports[0].left(2), QByteArray::fromHex("023e"));
        QCOMPARE(reports[1].left(2), QByteArray::fromHex("0226"));
        QCOMPARE(reports[0].mid(2, 62) + reports[1].mid(2, 38), payload);
    }
    void rawHidMalformedReportsCannotEscapeTheirBuffer()
    {
        HidTestInventory inventory;
        QList<QByteArray> reports{QByteArray::fromHex("01"), QByteArray::fromHex("013f00"),
            QByteArray::fromHex("02026162"), QByteArray::fromHex("0100"), QByteArray::fromHex("0103616263")};
        std::atomic_int reads{0}, closes{0};
        const RawHIDBackend backend{
            [](const char *) { return reinterpret_cast<hid_device *>(quintptr(1)); },
            [&](hid_device *, unsigned char *data, size_t, int) {
                const int index = reads++;
                if (index >= reports.size()) { QThread::msleep(5); return 0; }
                const auto &report = reports[index];
                std::memcpy(data, report.constData(), report.size()); return int(report.size());
            },
            [](hid_device *, const unsigned char *, size_t size) { return int(size); },
            [&](hid_device *) { ++closes; }
        };
        RawHID io(inventory.device, backend);
        QSignalSpy ready(&io, &QIODevice::readyRead);
        QSignalSpy failed(&io, &RawHID::transportFailed);
        QVERIFY(io.open(QIODevice::ReadWrite));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(io.readAll(), QByteArray("abc"));
        QVERIFY(failed.isEmpty());
        io.close();
        QCOMPARE(closes.load(), 1);
    }
    void liveTransportFailurePersistsInRecoveryCard_data()
    {
        QTest::addColumn<int>("failureError");
        QTest::newRow("stalled-usb") << ETIMEDOUT;
        QTest::newRow("normal-usb-unplug") << ENODEV;
    }
    void liveTransportFailurePersistsInRecoveryCard()
    {
        QFETCH(int, failureError);
        class FaultConnection : public RawHIDConnection {
        public:
            std::atomic_bool failWrite{false};
            std::atomic_int closes{0};
            int failureError;
            QPointer<RawHID> io;
            Core::IDevice *device;
            bool registered = false;
            explicit FaultConnection(int error) : failureError(error) {
                USBPortInfo port{};
                port.path = "/dev/test-hid"; port.serialNumber = "test-controller"; port.product = "OMNIBUSF4SD";
                device = devicesForPorts({port}).first(); device->setUsbTopologyPath("2-4");
            }
            ~FaultConnection() override {
                closeDevice({});
                if (registered) ExtensionSystem::PluginManager::instance()->removeObject(this);
            }
            void registerInventory() {
                registered = true;
                ExtensionSystem::PluginManager::instance()->addObject(this);
            }
            QList<Core::IDevice *> availableDevices() override { return {device}; }
        protected:
            RawHID *createTransport(USBDevice *device) override {
                const RawHIDBackend backend{
                    [](const char *) { return reinterpret_cast<hid_device *>(quintptr(1)); },
                    [](hid_device *, unsigned char *, size_t, int) { QThread::msleep(5); return 0; },
                    [this](hid_device *, const unsigned char *, size_t size) {
                        if (failWrite) { errno = failureError; return -1; }
                        return int(size);
                    },
                    [this](hid_device *) { ++closes; }
                };
                io = new RawHID(device, backend); return io;
            }
        } connection(failureError);
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        connection.registerInventory();
        manager->reportFlightDataReceived();
        connection.failWrite = true;
        QCOMPARE(connection.io->write("request", 7), qint64(7));
        QTRY_VERIFY(!manager->isConnected());
        QVERIFY(!manager->flightDataAvailable());
        const auto problem = manager->controllerRecovery().problem;
        QCOMPARE(connection.closes.load(), 1);
        QVERIFY(connection.io.isNull());
        if (failureError == ENODEV) {
            QVERIFY(problem.message.isEmpty());
            QVERIFY(!manager->automaticConnectionBlocked("2-4"));
            return;
        }
        QVERIFY(problem.message.contains("USB communication"));
        QVERIFY(problem.message.contains("retry the connection"));
        QVERIFY(problem.details.contains("/dev/test-hid"));
        QVERIFY(problem.details.contains("errno 110"));
        QVERIFY(manager->automaticConnectionBlocked("2-4"));
        QVERIFY(page->findChild<QPushButton *>("firmwareRetryConnection")->isVisible());
        captureFirmwarePage("live-usb-failure");
    }
    void receivedSamplesFollowValidatedUavTalkAndResetOnDisconnect()
    {
        TestDevice io; QVERIFY(io.open(QIODevice::ReadWrite));
        UAVTalk protocol(&io, objects);
        auto *baro = BaroAltitude::GetInstance(objects);
        const auto saved = baro->getData();
        const auto restore = qScopeGuard([&] { baro->setData(saved); baro->resetIsPresentOnHardware(); });
        baro->resetIsPresentOnHardware();
        auto data = saved; data.Pressure = 101.325f; data.Temperature = 22.5f;
        baro->setData(data);
        QVERIFY(!baro->getReceived()); // Editing local data cannot count as telemetry.
        baro->setIsPresentOnHardware(true);
        QVERIFY(!baro->getReceived()); // Presence alone cannot count either.
        auto frame = objectFrame(baro);
        auto corrupt = frame; corrupt.back() ^= 0x01;
        io.receive(corrupt);
        QVERIFY(!baro->getReceived());
        io.receive(frame);
        QVERIFY(baro->getReceived());
        QCOMPARE(baro->getData().Pressure, 101.325f);
        baro->setIsPresentOnHardware(true);
        QVERIFY(baro->getReceived());
        QSignalSpy presence(baro, QOverload<bool>::of(&UAVDataObject::presentOnHardwareChanged));
        baro->resetIsPresentOnHardware();
        QVERIFY(!baro->getReceived());
        QVERIFY(!baro->getPresenceKnown());
        QCOMPARE(presence.size(), 1);
        QCOMPARE(presence[0][0].toBool(), false);
    }
    void liveControllerHealthLogsAlarmChangesAndReasons()
    {
        TestConnection connection("Native controller", "2-4");
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->reportFlightDataReceived();
        auto *alarms = SystemAlarms::GetInstance(objects);
        const auto saved = alarms->getData();
        const auto restore = qScopeGuard([&] { alarms->setData(saved); });
        MessageCapture messages;
        UAVTalk protocol(connection.io, objects);
        Telemetry telemetry(&protocol, objects);
        TelemetryMonitor monitor(objects, &telemetry);
        auto *baro = BaroAltitude::GetInstance(objects);
        auto *gps = GPSPosition::GetInstance(objects);
        auto *stats = SystemStats::GetInstance(objects);
        const auto savedBaro = baro->getData();
        const auto savedGps = gps->getData();
        const auto savedStats = stats->getData();
        const auto restoreReadings = qScopeGuard([&] {
            baro->setData(savedBaro); gps->setData(savedGps); stats->setData(savedStats);
        });
        baro->resetIsPresentOnHardware(); gps->resetIsPresentOnHardware(); stats->resetIsPresentOnHardware();
        auto data = saved;
        for (auto &severity : data.Alarm) severity = SystemAlarms::ALARM_OK;
        data.Alarm[SystemAlarms::ALARM_SENSORS] = SystemAlarms::ALARM_CRITICAL;
        data.Alarm[SystemAlarms::ALARM_TEMPBARO] = SystemAlarms::ALARM_ERROR;
        data.Alarm[SystemAlarms::ALARM_MANUALCONTROL] = SystemAlarms::ALARM_CRITICAL;
        data.ManualControl = SystemAlarms::MANUALCONTROL_SETTINGS;
        data.StateEstimation = SystemAlarms::STATEESTIMATION_NONE;
        alarms->setData(data);
        QVERIFY(messages.information.join('\n').contains("Sensors=Critical"));
        QVERIFY(messages.information.join('\n').contains("TempBaro=Error"));
        QVERIFY(messages.information.join('\n').contains("ManualControl reason=Settings"));
        QVERIFY(messages.information.last().contains("BaroAltitude: no telemetry samples received"));
        QCOMPARE(messages.information.filter("Controller health:").size(), 1);
        alarms->setData(data);
        QCOMPARE(messages.information.filter("Controller health:").size(), 1);
        auto baroData = savedBaro; baroData.Pressure = 101.325f; baroData.Temperature = 22.5f;
        baro->setData(baroData); connection.io->receive(objectFrame(baro));
        auto gpsData = savedGps; gpsData.Status = GPSPosition::STATUS_NOGPS; gpsData.Satellites = 0;
        gps->setData(gpsData); connection.io->receive(objectFrame(gps));
        auto statsData = savedStats;
        statsData.ObjectManagerQueueID = AttitudeActual::OBJID;
        statsData.ObjectManagerCallbackID = 0x12345678;
        statsData.EventSystemWarningID = 0;
        stats->setData(statsData); connection.io->receive(objectFrame(stats));
        data.Alarm[SystemAlarms::ALARM_EVENTSYSTEM] = SystemAlarms::ALARM_WARNING;
        alarms->setData(data);
        QCOMPARE(messages.information.filter("Controller health:").size(), 2);
        const auto warning = messages.information.last();
        QVERIFY(warning.contains("ObjectManagerQueueID=AttitudeActual"));
        QVERIFY(warning.contains("ObjectManagerCallbackID=0x12345678"));
        const auto pressure = QRegularExpression("BaroAltitude[.]Pressure=([^; ]+)").match(warning);
        QVERIFY2(pressure.hasMatch(), qPrintable(warning));
        QVERIFY(qFuzzyCompare(pressure.captured(1).toFloat(), 101.325f));
        QVERIFY(warning.contains("BaroAltitude.Temperature=22.5"));
        QVERIFY(warning.contains("GPSPosition.Status=NoGPS"));
        QVERIFY(!warning.contains("no telemetry samples received"));
        for (auto &severity : data.Alarm) severity = SystemAlarms::ALARM_OK;
        data.ManualControl = SystemAlarms::MANUALCONTROL_NONE;
        alarms->setData(data);
        QCOMPARE(messages.information.filter("Controller health:").size(), 3);
        QVERIFY(messages.information.last().contains("No active alarms"));
        QVERIFY(!messages.information.last().contains("ObjectManagerQueueID")); // Saved IDs are historical after the alarm clears.
        manager->disconnectDevice();
        data.Alarm[SystemAlarms::ALARM_SENSORS] = SystemAlarms::ALARM_CRITICAL;
        alarms->setData(data);
        QCOMPARE(messages.information.filter("Controller health:").size(), 3);
    }
    void controllerRecoveryInstallationHandsOffToLiveDashboard_data()
    {
        QTest::addColumn<QString>("boardId");
        QTest::newRow("radix") << QString("radix");
        QTest::newRow("omnibus") << QString("omnibusf4sd");
        QTest::newRow("hglrc") << QString("hglrc-f4-v6pro");
    }
    void controllerRecoveryInstallationHandsOffToLiveDashboard()
    {
        QFETCH(QString, boardId);
        const auto board = Core::FirmwareCatalog::instance().board(boardId);
        const auto imagePath = Core::FirmwareCatalog::imagePath(board, Core::FirmwareCatalog::ImageKind::FullInstall);
        const auto image = Core::FirmwareInstallImage::load(board, imagePath);
        QVERIFY2(image.valid(), qPrintable(image.error));
        class InstallRunner final : public Core::DfuCommandRunner {
        public:
            QByteArray written;
            QString listing;
            int downloads = 0;
            void run(const QString &, const QStringList &arguments, int) override {
                QString output;
                if (arguments.contains("-D")) {
                    ++downloads;
                    QFile file(arguments[arguments.indexOf("-D") + 1]);
                    QVERIFY(file.open(QIODevice::ReadOnly)); written = file.readAll();
                } else if (arguments.contains("-U")) {
                    const auto bytes = arguments.join(' ').contains(":leave") ? written.left(1) : written;
                    QFile file(arguments[arguments.indexOf("-U") + 1]);
                    QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size()));
                } else if (written.isEmpty()) output = listing;
                QTimer::singleShot(0, this, [this, output] { emit finished(0, output); });
            }
            void cancel() override {}
        } runner;
        runner.listing = QString("Found DFU: [0483:df11] ver=2200, devnum=8, cfg=1, intf=0, path=\"2-4\", alt=0, name=\"@Internal Flash /0x08000000/04*016Kg,01*064Kg,%1*128Kg\", serial=\"1234ABCD\"\n")
            .arg(boardId == "radix" ? "03" : "07");
        Core::FirmwarePage dashboard(manager, nullptr, &runner);
        dashboard.resize(1280, 900); dashboard.show();
        const QStringList cards{"firmwareAttitudeCard", "firmwareModelCard", "firmwareHealthCard"};
        for (int index = 0; index < cards.size(); ++index) {
            auto *panel = new QLabel("Live instrument", &dashboard);
            dashboard.setFlightPanel(index, panel);
        }
        TestConnection controller(board.name, "2-4"); controller.transport = "USB";
        const Core::DevListItem device(&controller, controller.device);
        QVERIFY(manager->autoConnectDevice(device));
        emit controller.transportDisconnected(&controller);
        auto *recover = dashboard.findChild<QPushButton *>("firmwareRecoverInstallation");
        QVERIFY(recover->isVisible());
        QTest::mouseClick(recover, Qt::LeftButton);
        QPointer<Core::FirmwareInstallWidget> installation = dashboard.findChild<Core::FirmwareInstallWidget *>();
        QVERIFY(installation);
        const auto cancelOnFailure = qScopeGuard([installation] {
            if (installation) installation->cancelInstallation();
        });
        auto *installer = installation->findChild<Core::DfuInstaller *>();
        QTRY_COMPARE(installer->stage(), Core::DfuInstaller::Stage::AwaitingTelemetry);
        QVERIFY(runner.downloads > 0);
        QVERIFY(!dashboard.findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
        QVERIFY(!installation->findChild<QWidget *>("firmwareInstallTaskCard")->property("uiCard").toBool());
        QVERIFY(!installation->findChild<QLabel *>("firmwareInstallHeading")->isVisible());
        const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captures.isEmpty()) QVERIFY(dashboard.grab().save(captures + "/inline-install-" + boardId + ".png"));
        auto *logToggle = installation->findChild<QToolButton *>("firmwareInstallDetailsToggle");
        auto *cancel = installation->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel);
        QCoreApplication::processEvents();
        QVERIFY(std::abs(logToggle->mapTo(installation, logToggle->rect().center()).y()
            - cancel->mapTo(installation, cancel->rect().center()).y()) <= 4);
        QTest::mouseClick(logToggle, Qt::LeftButton);
        auto *logDetails = installation->findChild<QPlainTextEdit *>("firmwareInstallDetails");
        QVERIFY(logDetails->isVisible());
        QVERIFY(installation->findChild<QWidget *>("firmwareInstallTaskCard")->isAncestorOf(logDetails));
        QTest::mouseClick(logToggle, Qt::LeftButton);
        // An early node/access failure must recover through the install timer,
        // without requiring Start controller and without hiding the install.
        controller.failOpen = true;
        QVERIFY(!manager->autoConnectDevice(device));
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::AwaitingTelemetry);
        QVERIFY(!manager->automaticConnectionBlocked("2-4"));
        controller.failOpen = false;
        QTRY_VERIFY_WITH_TIMEOUT(manager->isConnected(), 3000);
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::AwaitingTelemetry);
        const auto reportLiveData = [&] {
            if (!manager->isConnected()) QVERIFY(manager->connectDevice(device));
            manager->setNativeFirmwareIdentity(board.name, "DroniNG 1.0");
            manager->setControllerSetupRequired(true);
            manager->reportFirmwareDescription(image.descriptor);
            if (!board.layoutId.isEmpty()) manager->reportFirmwareProfile(1, board.id, board.layoutId, image.profileHash);
            manager->reportFlightDataReceived();
        };
        reportLiveData();
        if (installer->stage() == Core::DfuInstaller::Stage::Configuring) {
            emit manager->boardSettingsFinished(true, "Board settings saved.");
            QVERIFY(!manager->isConnected());
            reportLiveData();
        }
        // Complete must reveal the ordinary card and all instruments without Done.
        QVERIFY(manager->flightDataAvailable());
        QVERIFY(!manager->firmwareInstallationActive());
        QVERIFY(!installation->isVisible());
        QCOMPARE(dashboard.findChild<QLabel *>("firmwarePageTitle")->text(), QString("Set up controller?"));
        QVERIFY(dashboard.findChild<QWidget *>("firmwareControllerCard")->isVisible());
        QVERIFY(!dashboard.findChild<QPushButton *>("firmwareInstallAction")->isVisible());
        int setupActions = 0;
        for (const auto *button : dashboard.findChildren<QPushButton *>())
            if (button->isVisible() && button->text() == "Launch setup wizard") ++setupActions;
        QCOMPARE(setupActions, 1);
        for (const auto &card : cards) QVERIFY(dashboard.findChild<QWidget *>(card)->isEnabled());
        QVERIFY(!dashboard.findChild<QDialog *>());
        QTRY_VERIFY(!installation);
        for (const int width : {640, 1280}) {
            dashboard.resize(width, 900); QCoreApplication::processEvents();
            QVERIFY(dashboard.findChild<QScrollArea *>("firmwareScrollArea")->horizontalScrollBar()->maximum() == 0);
            if (!captures.isEmpty()) QVERIFY(dashboard.grab().save(captures + QString("/installed-live-%1-%2.png").arg(boardId).arg(width)));
            for (const auto &card : cards) QVERIFY(dashboard.findChild<QWidget *>(card)->isEnabled());
        }
        // Firmware offers compare the descriptor, even after a fresh connection.
        manager->disconnectDevice();
        reportLiveData();
        auto *update = dashboard.findChild<QPushButton *>("firmwareInstallAction");
        QVERIFY(!update->isVisible());
        if (!board.layoutId.isEmpty()) {
            auto otherDescriptor = image.descriptor; otherDescriptor[8] ^= 1;
            manager->reportFirmwareDescription(otherDescriptor);
            QVERIFY(update->isVisible());
            QVERIFY(!update->property("primary").toBool());
            manager->reportFirmwareDescription(image.descriptor);
            QVERIFY(!update->isVisible());
        }
        QSignalSpy setupRequests(manager, &Core::ConnectionManager::setupWizardRequested);
        QTest::mouseClick(dashboard.findChild<QPushButton *>("firmwareConfigureAction"), Qt::LeftButton);
        QCOMPARE(setupRequests.size(), 1);
    }
    void legacyFirmwareUpdateIsInlineAndNonblocking_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("offerExpected");
        QTest::newRow("different-firmware") << QString("firmware") << true;
        QTest::newRow("matching-firmware") << QString("none") << false;
        QTest::newRow("metadata-only") << QString("metadata") << false;
        QTest::newRow("different-board-revision") << QString("board") << false;
    }
    void legacyFirmwareUpdateIsInlineAndNonblocking()
    {
        QFETCH(QString, change); QFETCH(bool, offerExpected);
        const auto board = Core::FirmwareCatalog::instance().board("revolution");
        QFile firmware(Core::FirmwareCatalog::imagePath(board, Core::FirmwareCatalog::ImageKind::Application));
        QVERIFY(firmware.open(QIODevice::ReadOnly));
        auto descriptor = firmware.readAll().right(100);
        QCOMPARE(descriptor.size(), 100);
        if (change == "firmware" || change == "metadata") descriptor[4] ^= 1;
        if (change == "firmware") descriptor[40] ^= 1;
        if (change == "board") { descriptor[13] ^= 1; descriptor[40] ^= 1; }

        USBMonitor monitor;
        TelemetryManager telemetry; UAVObjectUtilManager util;
        plugins.addObject(&telemetry); plugins.addObject(&util);
        const auto remove = qScopeGuard([&] { plugins.removeObject(&util); plugins.removeObject(&telemetry); });
        QPluginLoader boards(QDir(GCS_BUILD_TREE_PATH).filePath("lib/droning/plugins/droniNG/libOpenPilot.so"));
        auto *plugin = qobject_cast<ExtensionSystem::IPlugin *>(boards.instance());
        QVERIFY2(plugin, qPrintable(boards.errorString()));
        QString error; QVERIFY2(plugin->initialize({}, &error), qPrintable(error));
        plugin->extensionsInitialized();
        const auto unload = qScopeGuard([&] { boards.unload(); });
        auto *iap = FirmwareIAPObj::GetInstance(objects);
        const auto previous = iap->getData();
        const auto restore = qScopeGuard([&] { iap->setData(previous); });
        auto data = previous;
        data.BoardType = board.boardType; data.BoardRevision = board.boardRevision;
        std::memcpy(data.Description, descriptor.constData(), 100);
        iap->setData(data);
        QVERIFY(util.getBoardType());
        TestConnection controller("Revolution", "2-9");
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->reportFlightDataReceived();
        manager->setControllerSetupRequired(false);

        uploader::UploaderGadgetWidget uploader;
        plugins.addObject(&uploader);
        uploader.resize(1280, 900); uploader.show();
        // A nested prompt loop would run this timer before detection returns.
        // Dismiss it to make the regression fail promptly instead of hanging.
        bool timerRan = false;
        QTimer::singleShot(0, &uploader, [&] {
            timerRan = true;
            if (auto *prompt = uploader.findChild<QWidget *>("firmwarePrompt"))
                if (auto *buttons = prompt->findChild<QDialogButtonBox *>())
                    if (auto *ignore = buttons->button(QDialogButtonBox::Ignore)) ignore->click();
        });
        QVERIFY(QMetaObject::invokeMethod(&uploader, "onAutopilotReady", Qt::DirectConnection));
        QVERIFY(!timerRan);
        QVERIFY(!uploader.findChild<QWidget *>("firmwarePrompt"));
        QVERIFY(manager->isConnected());
        QCOMPARE(iap->getData().Command, data.Command);
        QCoreApplication::processEvents();
        auto *dashboard = uploader.findChild<Core::FirmwarePage *>();
        QVERIFY(dashboard);
        auto *overview = dashboard->findChild<QWidget *>("firmwareOverview");
        auto *notice = dashboard->findChild<QWidget *>("firmwareUpdateNotice");
        auto *update = dashboard->findChild<QPushButton *>("firmwareInstallAction");
        QVERIFY(overview->isAncestorOf(notice));
        QCOMPARE(notice->isVisible(), offerExpected);
        QCOMPARE(update->isVisible(), offerExpected);
        QVERIFY(dashboard->findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
        QVERIFY(!uploader.findChild<QDialog *>());
        if (offerExpected) {
            QVERIFY(update->isEnabled()); QVERIFY(!update->property("primary").toBool());
            QCOMPARE(update->text(), QString("Update firmware…"));
            QVERIFY(dashboard->findChild<QLabel *>("firmwareUpdateDetails")->text().contains("compatible firmware"));
            const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
            for (const int width : {640, 1280}) {
                uploader.resize(width, 900); QCoreApplication::processEvents();
                QVERIFY(dashboard->findChild<QScrollArea *>("firmwareScrollArea")->horizontalScrollBar()->maximum() == 0);
                if (!captures.isEmpty()) QVERIFY(uploader.grab().save(captures + QString("/inline-legacy-update-%1.png").arg(width)));
            }
        }
        auto *assistant = uploader.findChild<QWidget *>("firmwareUpgradeCard");
        QVERIFY(assistant); QVERIFY(overview->isAncestorOf(assistant));
        if (offerExpected) {
            assistant->show();
            QTRY_VERIFY(!dashboard->findChild<QWidget *>("firmwareControllerCard")->isVisible());
            QVERIFY(!update->isVisible());
            QCOMPARE(dashboard->findChild<QLabel *>("firmwarePageTitle")->text(), QString("Firmware maintenance"));
            const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
            if (!captures.isEmpty()) QVERIFY(uploader.grab().save(captures + "/inline-upgrade-progress.png"));
            assistant->hide(); QCoreApplication::processEvents();
        }
        manager->disconnectDevice(); QCoreApplication::processEvents();
        QVERIFY(!notice->isVisible()); QVERIFY(!update->isVisible());
    }
    void legacyUpdateActionAndOperationStayInControllerCard()
    {
        TestConnection controller("Revolution", "2-9");
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->setNativeFirmwareIdentity("Revolution", "dRonin old");
        manager->reportFlightDataReceived();
        manager->setControllerSetupRequired(false);
        page->setLegacyFirmwareUpdate("old", "new");
        auto *update = page->findChild<QPushButton *>("firmwareInstallAction");
        QSignalSpy requested(page, &Core::FirmwarePage::legacyFirmwareUpdateRequested);
        QVERIFY(update->isVisible()); QTest::mouseClick(update, Qt::LeftButton);
        QCOMPARE(requested.size(), 1);
        QVERIFY(manager->isConnected());
        auto *operation = new QLabel("Updating firmware", page);
        page->addOperationWidget(operation); page->setOperationActive(true); operation->show();
        QTRY_VERIFY(!page->findChild<QWidget *>("firmwareControllerCard")->isVisible());
        QVERIFY(page->findChild<QWidget *>("firmwareOverview")->isAncestorOf(operation));
        QVERIFY(!update->isVisible());
        QVERIFY(!page->findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
        operation->hide(); page->setOperationActive(false);
        QCoreApplication::processEvents(); delete operation;
        QVERIFY(update->isVisible());
        manager->disconnectDevice();
        QVERIFY(!update->isVisible());
        TestConnection other("Another controller", "2-10");
        QVERIFY(manager->connectDevice({&other, other.device}));
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG"); manager->reportFlightDataReceived();
        QVERIFY(!update->isVisible());
    }
    void legacyBootloaderOffersActionsWithoutTelemetry_data()
    {
        QTest::addColumn<bool>("canBoot");
        QTest::addColumn<bool>("updateAvailable");
        QTest::newRow("old-release-in-bootloader") << true << true;
        QTest::newRow("matching-firmware-in-bootloader") << true << false;
        QTest::newRow("missing-image-in-build") << true << false;
        QTest::newRow("blank-controller") << false << true;
    }
    void boardSelectorsShowEveryModelAlphabetically()
    {
        const auto boards = Core::FirmwareCatalog::instance().boards();
        for (const auto *name : {"firmwareBoardModel", "firmwareHardwareModel"}) {
            auto *models = page->findChild<QComboBox *>(name);
            QVERIFY(models); QCOMPARE(models->count(), boards.size() + 1);
            QVERIFY(models->itemData(0).toString().isEmpty());
            for (int row = 1; row < models->count(); ++row) {
                // Every row is a selectable board: no separators or target-based groups.
                const auto board = Core::FirmwareCatalog::instance().board(models->itemData(row).toString());
                QVERIFY(board.isValid()); QCOMPARE(models->itemText(row), board.name);
                QVERIFY(models->model()->flags(models->model()->index(row, 0)) & Qt::ItemIsSelectable);
                if (row > 1) QVERIFY(models->itemText(row - 1).compare(models->itemText(row), Qt::CaseInsensitive) < 0);
            }
            for (const auto &board : boards) QVERIFY(models->findData(board.id) > 0);
        }
        auto *models = page->findChild<QComboBox *>("firmwareHardwareModel");
        const QPersistentModelIndex row(models->model()->index(models->findData("hglrc-f4-v6pro"), 0));
        manager->setNativeFirmwareIdentity("Revolution", "dRonin old");
        manager->setNativeFirmwareIdentity("Omnibus F4 SD", "DroniNG 1.0");
        QVERIFY(row.isValid()); QCOMPARE(row.data().toString(), QString("HGLRC F4 V6PRO"));
    }
    void hardwareModelIsSeparateFromFirmwareTarget_data()
    {
        QTest::addColumn<bool>("bootloader");
        QTest::newRow("telemetry") << false;
        QTest::newRow("bootloader") << true;
    }
    void hardwareModelIsSeparateFromFirmwareTarget()
    {
        QFETCH(bool, bootloader);
        TestConnection controller("Revolution", "2-9");
        const auto restore = qScopeGuard([&] {
            page->setLegacyBootloaderState(false); page->setLegacyFirmwareUpdate();
            Core::ICore::instance()->settings()->remove("ControllerHardware");
        });
        if (!bootloader) QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->setControllerSerial("00112233445566778899AABB+BL");
        manager->setNativeFirmwareIdentity("Revolution", "dRonin Release-20180729", "dRonin 88");
        page->setLegacyFirmwareUpdate("2018", "bundled");
        if (bootloader) page->setLegacyBootloaderState(true, true);
        else manager->reportFlightDataReceived();
        auto *models = page->findChild<QComboBox *>("firmwareHardwareModel");
        auto *target = page->findChild<QLabel *>("firmwareDetectedTarget");
        auto *model = page->findChild<QLabel *>("firmwareDetectedBoard");
        auto *update = page->findChild<QPushButton *>("firmwareInstallAction");
        QVERIFY(models->isVisible()); QCOMPARE(models->currentIndex(), 0);
        QCOMPARE(model->text(), QString("Not identified")); QCOMPARE(target->text(), QString("Revolution"));
        QVERIFY(page->findChild<QLabel *>("firmwareHardwareHint")->text().contains("physical board model"));
        QVERIFY(models->findData("revolution") > 0); QVERIFY(models->findData("hglrc-f4-v6pro") > 0);
        const QPersistentModelIndex row(models->model()->index(models->findData("hglrc-f4-v6pro"), 0));
        manager->setFirmwareStatus("Ready to flash firmware");
        QVERIFY(row.isValid()); QCOMPARE(row.data().toString(), QString("HGLRC F4 V6PRO"));
        QVERIFY(update->isVisible()); QVERIFY(update->isEnabled());
        captureFirmwarePage(bootloader ? "hardware-unidentified-bootloader" : "hardware-unidentified-telemetry");
        QSignalSpy settingsWrites(manager, &Core::ConnectionManager::boardSettingsRequested);
        models->setCurrentIndex(models->findData("hglrc-f4-v6pro"));
        QCOMPARE(manager->hardwareBoardId(), QString("hglrc-f4-v6pro"));
        QCOMPARE(model->text(), QString("HGLRC F4 V6PRO"));
        QCOMPARE(target->text(), QString("Revolution"));
        QCOMPARE(Core::ICore::instance()->settings()->value("ControllerHardware/00112233445566778899aabb/board").toString(),
                 QString("hglrc-f4-v6pro"));
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG 1.0", "dRonin 88");
        QCOMPARE(models->currentData().toString(), QString("hglrc-f4-v6pro"));
        QCOMPARE(model->text(), QString("HGLRC F4 V6PRO")); QCOMPARE(target->text(), QString("Revolution"));
        QCOMPARE(settingsWrites.size(), 0); QVERIFY(update->isEnabled());
        const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        const auto previousSize = page->size();
        for (const int width : {640, 1280}) {
            page->resize(width, 900); QCoreApplication::processEvents();
            QVERIFY(page->findChild<QScrollArea *>("firmwareScrollArea")->horizontalScrollBar()->maximum() == 0);
            if (!captures.isEmpty()) QVERIFY(page->grab().save(captures
                + QString("/hardware-confirmed-%1-%2.png").arg(bootloader ? "bootloader" : "telemetry").arg(width)));
        }
        page->resize(previousSize);
        // USB mode changes and app reconnection retain a model by CPU ID, never by port or firmware target.
        page->setLegacyBootloaderState(false); manager->disconnectDevice();
        QVERIFY(manager->hardwareBoardId().isEmpty());
        manager->setControllerSerial("00112233445566778899aabb+FW");
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG 1.0");
        QCOMPARE(manager->hardwareBoardId(), QString("hglrc-f4-v6pro"));
        manager->disconnectDevice();
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->setControllerSerial("ffeeddccbbaa998877665544");
        manager->setNativeFirmwareIdentity("Revolution", "dRonin Release-20180729"); manager->reportFlightDataReceived();
        QVERIFY(manager->hardwareBoardId().isEmpty()); QCOMPARE(models->currentIndex(), 0);
        QCOMPARE(model->text(), QString("Not identified"));
        // Choosing the unknown entry explicitly forgets a saved model for this device.
        manager->disconnectDevice();
        manager->setControllerSerial("00112233445566778899aabb+FW");
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG 1.0");
        QCOMPARE(manager->hardwareBoardId(), QString("hglrc-f4-v6pro"));
        models->setCurrentIndex(0);
        QVERIFY(manager->hardwareBoardId().isEmpty());
        QVERIFY(!Core::ICore::instance()->settings()->contains("ControllerHardware/00112233445566778899aabb/board"));
        manager->disconnectDevice();
        manager->setControllerSerial("00112233445566778899aabb+BL");
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG 1.0");
        QVERIFY(manager->hardwareBoardId().isEmpty());
    }
    void hardwareSelectionHandlesWrongFirmwareAndUnknownSerial()
    {
        TestConnection controller("Revolution", "2-9");
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->setNativeFirmwareIdentity("Revolution", "dRonin old"); manager->reportFlightDataReceived();
        page->setLegacyFirmwareUpdate("old", "new");
        const auto restore = qScopeGuard([&] { page->setLegacyFirmwareUpdate(); });
        auto *models = page->findChild<QComboBox *>("firmwareHardwareModel");
        auto *update = page->findChild<QPushButton *>("firmwareInstallAction");
        models->setCurrentIndex(models->findData("omnibusf4sd"));
        QCOMPARE(manager->hardwareBoardId(), QString("omnibusf4sd"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), QString("Omnibus F4 SD"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedTarget")->text(), QString("Revolution"));
        QVERIFY(!page->findChild<QWidget *>("firmwareUpdateNotice")->isVisible());
        QCOMPARE(update->text(), QString("Install compatible firmware…"));
        QCOMPARE(update->isEnabled(), !Core::DfuInstaller::bundledHelper().isEmpty());
        QVERIFY(page->findChild<QLabel *>("firmwareHardwareHint")->text().contains("Install compatible firmware"));
        // Firmware reports must never overwrite a selected physical model, even when targets disagree.
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG 1.0");
        QCOMPARE(manager->hardwareBoardId(), QString("omnibusf4sd"));
        QVERIFY(!manager->confirmHardwareBoard("not-a-board"));
        if (update->isEnabled()) {
            QSignalSpy legacyUpdates(page, &Core::FirmwarePage::legacyFirmwareUpdateRequested);
            QTest::mouseClick(update, Qt::LeftButton);
            QCOMPARE(legacyUpdates.size(), 0);
            auto *installation = page->findChild<Core::FirmwareInstallWidget *>();
            QVERIFY(installation);
            QCOMPARE(installation->findChild<QLabel *>("firmwareInstallBootStep1")->text(),
                Core::FirmwareCatalog::instance().board("omnibusf4sd").bootInstructions.section('\n', 0, 0));
            installation->cancelInstallation();
            QVERIFY(!page->findChild<Core::FirmwareInstallWidget *>());
            if (!manager->isConnected()) QVERIFY(manager->connectDevice({&controller, controller.device}));
            manager->setNativeFirmwareIdentity("Revolution", "DroniNG 1.0"); manager->reportFlightDataReceived();
            page->setLegacyFirmwareUpdate("old", "new");
        }
        models->setCurrentIndex(models->findData("hglrc-f4-v6pro"));
        QCOMPARE(update->text(), QString("Update firmware…"));
        QVERIFY(page->findChild<QLabel *>("firmwareHardwareHint")->text().contains("for this connection"));
        manager->disconnectDevice();
        for (const auto &serial : {QString(), QString("Not Available"), QString("ttyACM0"), QString(24, '0'), QString(24, 'f')}) {
            manager->setControllerSerial(serial); QVERIFY(manager->controllerSerial().isEmpty());
            QVERIFY(manager->hardwareBoardId().isEmpty());
        }
    }
    void legacyBootloaderOffersActionsWithoutTelemetry()
    {
        QFETCH(bool, canBoot); QFETCH(bool, updateAvailable);
        QVERIFY(!manager->isConnected()); QVERIFY(!manager->flightDataAvailable());
        const QString board = QString::fromLatin1(QTest::currentDataTag()) == "missing-image-in-build" ? "AeroQuad32" : "Revolution";
        manager->setNativeFirmwareIdentity(board, canBoot ? "dRonin Release-20180729" : QString(), "dRonin 88");
        manager->setFirmwareStatus("Ready to flash firmware");
        page->setOperationActive(true);
        if (updateAvailable) page->setLegacyFirmwareUpdate(canBoot ? "old-release" : "not installed", "bundled");
        page->setLegacyBootloaderState(true, canBoot);
        const auto restore = qScopeGuard([&] {
            page->setLegacyBootloaderState(false); page->setLegacyFirmwareUpdate();
            page->setOperationActive(false); manager->setFirmwareIdentity({}, {});
        });
        auto *overview = page->findChild<QWidget *>("firmwareOverview");
        auto *update = page->findChild<QPushButton *>("firmwareInstallAction");
        auto *boot = page->findChild<QPushButton *>("firmwareBootAction");
        QCOMPARE(page->findChild<QLabel *>("firmwarePageTitle")->text(), QString("Controller in bootloader mode"));
        QCOMPARE(update->isVisible(), updateAvailable); QCOMPARE(boot->isVisible(), canBoot);
        QVERIFY(!page->findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
        QVERIFY(overview->isAncestorOf(update)); QVERIFY(overview->isAncestorOf(boot));
        QSignalSpy updates(page, &Core::FirmwarePage::legacyFirmwareUpdateRequested);
        QSignalSpy starts(page, &Core::FirmwarePage::legacyFirmwareBootRequested);
        if (updateAvailable) {
            QVERIFY(update->isEnabled()); QVERIFY(update->property("primary").toBool());
            QCOMPARE(update->text(), canBoot ? QString("Update firmware…") : QString("Install DroniNG"));
            QTest::mouseClick(update, Qt::LeftButton); QCOMPARE(updates.size(), 1);
        }
        if (canBoot) {
            QVERIFY(boot->isEnabled()); QCOMPARE(boot->property("primary").toBool(), !updateAvailable);
            QTest::mouseClick(boot, Qt::LeftButton); QCOMPARE(starts.size(), 1);
        }
        if (updateAvailable && canBoot) {
            const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
            const auto previousSize = page->size();
            for (const int width : {640, 1280}) {
                page->resize(width, 900); QCoreApplication::processEvents();
                QVERIFY(page->findChild<QScrollArea *>("firmwareScrollArea")->horizontalScrollBar()->maximum() == 0);
                QVERIFY(update->isVisible()); QVERIFY(boot->isVisible());
                if (!captures.isEmpty()) QVERIFY(page->grab().save(captures + QString("/legacy-bootloader-actions-%1.png").arg(width)));
            }
            page->resize(previousSize);
        }
        page->setOperationActive(true);
        QVERIFY(!update->isVisible()); QVERIFY(!boot->isEnabled());
        page->setLegacyBootloaderState(false); page->setLegacyFirmwareUpdate(); page->setOperationActive(false);
        QVERIFY(!update->isVisible()); QVERIFY(!boot->isVisible());
    }
    void controllerRecoveryRetainsTimeoutAndRequiresUnknownBoardSelection()
    {
        TestConnection controller("Unidentified controller", "2-8");
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->telemetryConnectionFailed({"No response from the controller. Reconnect USB, then retry.", {}, "Handshake timeout"});
        manager->disconnectDevice();
        QVERIFY(manager->firmwareError().message.contains("No response"));
        QVERIFY(!manager->firmwareDetectionActive());
        QVERIFY(manager->controllerRecovery().boardId.isEmpty());
        auto *recover = page->findChild<QPushButton *>("firmwareRecoverInstallation");
        QVERIFY(recover->isVisible()); QCOMPARE(recover->text(), QString("Select board…"));
        QVERIFY(page->findChild<QLabel *>("connectionErrorGuidance")->isVisible());
        QTest::mouseClick(recover, Qt::LeftButton);
        auto *boards = page->findChild<QComboBox *>("firmwareBoardModel");
        QVERIFY(boards->isVisible()); QCOMPARE(boards->currentData().toString(), QString());
        QVERIFY(!page->findChild<QPushButton *>("firmwareInstallAction")->isEnabled());
        QVERIFY(!page->findChild<Core::FirmwareInstallWidget *>());
        QTest::mouseClick(page->findChild<QToolButton *>("firmwareBackAction"), Qt::LeftButton);
        QVERIFY(recover->isVisible());
        QTest::mouseClick(page->findChild<QPushButton *>("firmwareOtherController"), Qt::LeftButton);
        QVERIFY(page->findChild<QWidget *>("firmwareConnectCard")->isVisible());
        QVERIFY(manager->controllerRecovery().problem.message.isEmpty());
    }
    void controllerRecoveryDoesNotReuseBoardForDifferentUsbProduct()
    {
        TestConnection known("BrainFPV RADIX", "2-6");
        QVERIFY(manager->connectDevice({&known, known.device}));
        manager->telemetryConnectionFailed({"No response", {}, {}});
        QCOMPARE(manager->controllerRecovery().boardId, QString("radix"));
        manager->disconnectDevice();
        TestConnection different("Unidentified controller", "2-6");
        QVERIFY(manager->connectDevice({&different, different.device}));
        manager->telemetryConnectionFailed({"No response", {}, {}});
        QVERIFY(manager->controllerRecovery().boardId.isEmpty());
        QCOMPARE(page->findChild<QPushButton *>("firmwareRecoverInstallation")->text(), QString("Select board…"));
    }
    void controllerRecoveryDoesNotApplyPreviousInstallToAnotherDetectedBoard()
    {
        const auto &catalog = Core::FirmwareCatalog::instance();
        const auto radix = catalog.board("radix");
        const auto image = Core::FirmwareInstallImage::load(radix, Core::FirmwareCatalog::imagePath(radix,
            Core::FirmwareCatalog::ImageKind::FullInstall));
        QVERIFY2(image.valid(), qPrintable(image.error));
        manager->recordVerifiedInstallation(radix.id, "2-7", image.descriptor);
        const auto other = catalog.board("omnibusf4sd");
        TestConnection controller(other.name, "2-7");
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->telemetryConnectionFailed({"No response", {}, {}});
        QCOMPARE(manager->controllerRecovery().boardId, other.id);
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), other.name);
        manager->setNativeFirmwareIdentity(other.name, "DroniNG");
        manager->reportFlightDataReceived(); manager->disconnectDevice();
        QVERIFY(page->findChild<QWidget *>("firmwareConnectCard")->isVisible());
        QVERIFY(!page->findChild<QPushButton *>("firmwareRecoverInstallation")->isVisible());
    }
    void controllerRecoveryExplainsMissingCompatibleImage()
    {
        const auto board = Core::FirmwareCatalog::instance().board("flyingpio");
        QVERIFY(board.isValid());
        QVERIFY(!Core::FirmwareInstallImage::load(board, Core::FirmwareCatalog::imagePath(board,
            Core::FirmwareCatalog::ImageKind::FullInstall)).valid());
        TestConnection controller(board.name, "2-9");
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->telemetryConnectionFailed({"No response from the controller.", {}, {}});
        auto *recover = page->findChild<QPushButton *>("firmwareRecoverInstallation");
        QVERIFY(recover->isVisible()); QVERIFY(!recover->isEnabled());
        QVERIFY(page->findChild<QLabel *>("firmwareRecoveryHint")->text().contains("missing from this build"));
        QVERIFY(page->findChild<QPushButton *>("firmwareRetryConnection")->isEnabled());
    }
    void nativeUsbStartupResetsStopAutomaticRetries()
    {
        TestConnection controller("OMNIBUSF4SD", "3-1.4.3.4"); controller.transport = "USB";
        const Core::DevListItem device(&controller, controller.device);
        QVERIFY(manager->autoConnectDevice(device));
        QVERIFY(manager->firmwareDetectionActive());
        emit controller.transportDisconnected(&controller);
        QVERIFY(!manager->isConnected());
        QVERIFY(!manager->firmwareDetectionActive());
        QVERIFY(manager->automaticConnectionBlocked("3-1.4.3.4"));
        QVERIFY(!manager->autoConnectDevice(device));
        QCOMPARE(controller.closeCalls, 1);
        QCOMPARE(manager->firmwareStatus(), QString("Controller needs attention"));
        QVERIFY(manager->firmwareError().message.contains("Automatic retries have stopped"));
        QCOMPARE(manager->connectionLog().count("OMNIBUSF4SD disconnected before live data was ready. USB path: 3-1.4.3.4."), 1);
        // Closing the installation and repeated inventories must retain the diagnosis.
        manager->endFirmwareInstallation();
        emit controller.availableDevChanged(&controller);
        QVERIFY(!manager->isConnected());
        QVERIFY(manager->firmwareError().details.contains("USB path: 3-1.4.3.4"));
        QSignalSpy changes(manager, &Core::ConnectionManager::firmwareStateChanged);
        for (int inventory = 0; inventory < 5; ++inventory) {
            emit controller.availableDevChanged(&controller);
            manager->telemetryHandshakeCompleted();
            manager->setFirmwareDetectionActive(true);
            QVERIFY(!manager->autoConnectDevice(device));
            QVERIFY(!manager->firmwareDetectionActive());
            QCOMPARE(page->findChild<QLabel *>("firmwarePageTitle")->text(), QString("Controller needs attention"));
        }
        QCOMPARE(changes.size(), 0);
        // Serial discovery can briefly lose its sysfs path during USB re-enumeration.
        TestConnection serial("OMNIBUSF4SD - ttyACM0", "");
        QVERIFY(!manager->autoConnectDevice({&serial, serial.device}));
        auto *error = page->findChild<QLabel *>("connectionErrorGuidance");
        QVERIFY(error); QVERIFY(error->text().contains("Automatic retries have stopped"));
        page->resize(900, 700);
        QCoreApplication::processEvents();
        QVERIFY(error->isVisible());
        const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captures.isEmpty()) QVERIFY(page->grab().save(captures + "/controller-startup-resets.png"));
        // A different controller remains usable, with its own connection state.
        {
            TestConnection other("Other controller", "1-6"); other.transport = "USB";
            QVERIFY(manager->autoConnectDevice({&other, other.device}));
            manager->reportFlightDataReceived();
            manager->disconnectDevice();
            QVERIFY(!manager->firmwareStatus().contains("Connected."));
            QVERIFY(manager->automaticConnectionBlocked("3-1.4.3.4"));
        }
        // Connect is an explicit new attempt. Successful telemetry clears the budget.
        QVERIFY(manager->connectDevice(device));
        QVERIFY(!manager->automaticConnectionBlocked("3-1.4.3.4"));
        manager->reportFlightDataReceived();
        emit controller.transportDisconnected(&controller);
        QVERIFY(!manager->automaticConnectionBlocked("3-1.4.3.4"));
        QVERIFY(manager->autoConnectDevice(device));
    }
    void nativeUsbManualDisconnectDoesNotCountAsStartupFailure()
    {
        TestConnection controller("OMNIBUSF4SD", "1-5"); controller.transport = "USB";
        const Core::DevListItem device(&controller, controller.device);
        for (int attempt = 0; attempt < 4; ++attempt) {
            QVERIFY(manager->autoConnectDevice(device));
            QVERIFY(manager->disconnectDevice());
        }
        QVERIFY(!manager->automaticConnectionBlocked("1-5"));
        QVERIFY(!manager->connectionLog().contains("USB path: 1-5."));
    }
    void nativeUsbInventoryLossStopsAutomaticRetries()
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
            manager->resetAutomaticConnectionFailures("1-7");
            TestConnection controller("OMNIBUSF4SD", "1-7"); controller.transport = "USB";
            QVERIFY(manager->autoConnectDevice({&controller, controller.device}));
            if (attempt == 1) {
                controller.io->close();
                QCoreApplication::processEvents();
            } else controller.detachController();
            QVERIFY(!manager->isConnected());
        }
        QVERIFY(manager->automaticConnectionBlocked("1-7"));
        TestConnection controller("OMNIBUSF4SD", "1-7"); controller.transport = "USB";
        QVERIFY(!manager->autoConnectDevice({&controller, controller.device}));
        manager->resetAutomaticConnectionFailures("1-7");
        QVERIFY(manager->autoConnectDevice({&controller, controller.device}));
    }
    void nativeUsbInventoryPreservesConnectionAndSelection()
    {
        manager->suspendPolling();
        const auto resume = qScopeGuard([this] { manager->resumePolling(); });
        USBMonitor monitor;
        class InventoryConnection : public RawHIDConnection {
        public:
            using RawHIDConnection::devicesForPorts;
            QList<USBPortInfo> ports;
            QPointer<TestDevice> io;
            bool registered = false;
            ~InventoryConnection() override {
                if (registered) {
                    ports.clear(); emit availableDevChanged(this);
                    ExtensionSystem::PluginManager::instance()->removeObject(this);
                }
            }
            QList<Core::IDevice *> availableDevices() override { return devicesForPorts(ports); }
            QIODevice *openDevice(Core::IDevice *) override { io = new TestDevice(this); return io; }
            void closeDevice(const QString &) override {
                if (io) { io->close(); io->deleteLater(); io = nullptr; }
            }
        } connection;
        USBPortInfo first{};
        first.vendorID = 0x20a0; first.productID = 0x415e; first.bcdDevice = 0x0902;
        first.path = "/dev/hidraw0"; first.serialNumber = "controller-one"; first.product = "Revolution";
        USBPortInfo second = first;
        second.path = "/dev/hidraw1"; second.serialNumber = "controller-two";
        connection.ports = {first, second};
        auto devices = connection.availableDevices();
        QCOMPARE(devices.size(), 2);
        QSignalSpy requested(manager, &Core::ConnectionManager::firmwarePageRequested);
        connection.registered = true;
        plugins.addObject(&connection);
        QCOMPARE(requested.count(), 0);
        auto *list = page->findChild<QComboBox *>("firmwareControllerList");
        list->setCurrentIndex(1);
        QVERIFY(manager->connectDevice({&connection, devices[1]}));
        auto *transport = manager->getCurrentConnection();
        QCOMPARE(requested.count(), 1);
        // A repeated scan, including a changed enumeration order, must keep the
        // actual connection and selected controller intact.
        connection.ports = {second, first};
        emit connection.availableDevChanged(&connection);
        QVERIFY(manager->isConnected()); QCOMPARE(manager->getCurrentConnection(), transport);
        QCOMPARE(manager->getCurrentDevice().device.data(), devices[1]);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(connection.availableDevices()[0], devices[1]);
        manager->disconnectDevice();
        emit connection.availableDevChanged(&connection);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(list->currentIndex(), 1);
        connection.ports = {first};
        emit connection.availableDevChanged(&connection);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        connection.ports = {first, second};
        emit connection.availableDevChanged(&connection);
        QCOMPARE(requested.count(), 2);
        connection.ports.clear(); emit connection.availableDevChanged(&connection);
        plugins.removeObject(&connection);
        connection.registered = false;
    }
    void firmwareContentUsesReadableWidths()
    {
        page->resize(1800, 600);
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setFirmwareIdentity("BTFL", "OBF4", true, "3.2.3");
        manager->telemetryConnectionFailed({"This controller answered as Betaflight. Install the recommended firmware below.", {}, {}});
        QCoreApplication::processEvents();
        auto *board = page->findChild<QComboBox *>("firmwareBoardModel");
        QVERIFY(board->width() <= 360);
        auto *canvas = page->findChild<QWidget *>("firmwareCanvas");
        QVERIFY(canvas);
        QCOMPARE(canvas->grab().toImage().pixelColor(2, 2), QApplication::palette().color(QPalette::Window));
        QVERIFY(board->palette().color(QPalette::Text).lightness() > 200);
        QVERIFY(page->styleSheet().isEmpty());
        QVERIFY(!qApp->styleSheet().isEmpty());
        QCOMPARE(page->palette().color(QPalette::Highlight), QApplication::palette().color(QPalette::Highlight));
        auto *guidance = page->findChild<QLabel *>("connectionErrorGuidance");
        QVERIFY(guidance->width() <= 720);
        QVERIFY(guidance->maximumWidth() == 720);
        QVERIFY(guidance->wordWrap());
        page->resize(650, 500);
    }
    void connectionHistoryExplainsSuccessFailureAndReconnect()
    {
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->telemetryHandshakeCompleted();
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG 1.0", "dRonin 0x88");
        manager->telemetryConnected();
        manager->reportFlightDataReceived();
        auto *toggle = page->findChild<QToolButton *>("connectionErrorDetailsToggle");
        auto *details = page->findChild<QPlainTextEdit *>("connectionErrorDetails");
        QVERIFY(toggle->isVisible());
        QVERIFY(!details->isVisible());
        QTest::mouseClick(toggle, Qt::LeftButton);
        QVERIFY(details->isVisible());
        QVERIFY(details->isReadOnly());
        const auto history = details->toPlainText();
        QVERIFY(history.contains("Connecting to"));
        QVERIFY(history.contains("Transport opened"));
        QVERIFY(history.contains("UAVTalk handshake completed"));
        QVERIFY(history.contains("Revolution"));
        QVERIFY(history.contains("DroniNG 1.0"));
        QVERIFY(history.contains("dRonin 0x88"));
        QVERIFY(history.contains("Controller data retrieved"));
        QVERIFY(history.contains("sending flight data"));
        captureFirmwarePage("firmware-connected-history");
        QSignalSpy logChanges(manager, &Core::ConnectionManager::connectionLogChanged);
        for (int packet = 0; packet < 10; ++packet) manager->reportFlightDataReceived();
        QCOMPARE(logChanges.count(), 0);
        QCOMPARE(details->toPlainText(), history);
        manager->telemetryConnectionFailed({"Controller stopped responding", {}, "rxBytes=0"});
        QVERIFY(details->toPlainText().contains("Controller stopped responding"));
        QVERIFY(details->toPlainText().contains("rxBytes=0"));
        manager->disconnectDevice();
        QVERIFY(details->toPlainText().contains("Disconnected from"));
        QVERIFY(details->toPlainText().contains("Controller stopped responding"));
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        QVERIFY(details->toPlainText().contains("Connecting to"));
        QVERIFY(!details->toPlainText().contains("Controller stopped responding"));
        QVERIFY(!details->toPlainText().contains("dRonin 0x88"));
    }
    void nativeBootloaderAndFlightDataState()
    {
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setNativeFirmwareIdentity("Revolution", "dRonin 20261010", "dRonin 0x88");
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBootloader")->text(), QString("dRonin 0x88"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedFirmware")->text(), QString("dRonin 20261010"));
        manager->reportFlightDataReceived();
        QVERIFY(page->findChild<QLabel *>("firmwareStatus")->text().contains("sending flight data"));
        QVERIFY(!page->findChild<QPushButton *>("firmwareFlightAction"));
        QVERIFY(page->findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
        QVERIFY(page->findChild<QPushButton *>("firmwareDisconnectAction")->isVisible());
        QVERIFY(!page->findChild<QComboBox *>("firmwareBoardModel")->isVisible());
        QVERIFY(!page->findChild<QPushButton *>("firmwareInstallAction")->isVisible());
        QVERIFY(!page->findChild<QWidget *>("firmwareRecoveryBody")->isVisible());
        QVERIFY(page->findChild<QPushButton *>("firmwareConfigureAction")->property("primary").toBool());
        QVERIFY(!page->findChild<QLabel *>("firmwareStep1"));
        captureFirmwarePage("firmware-connected-ready");
        manager->disconnectDevice();
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBootloader")->text(), QString("Not detected"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedFirmware")->text(), QString("Not detected"));
    }
    void firmwareReadyStateTracksTelemetryLossAndRecovery()
    {
        TestConnection controller;
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        manager->setNativeFirmwareIdentity("Revolution", "dRonin", "dRonin 0x88");
        manager->reportFlightDataReceived();
        QVERIFY(manager->flightDataAvailable());
        QVERIFY(page->findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
        manager->telemetryDisconnected();
        QVERIFY(!manager->flightDataAvailable()); QVERIFY(manager->firmwareDetectionActive());
        QVERIFY(!page->findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
        QVERIFY(page->findChild<QProgressBar *>("firmwareDetectionProgress")->isVisible());
        manager->reportFlightDataReceived();
        QVERIFY(manager->flightDataAvailable()); QVERIFY(!manager->firmwareDetectionActive());
        QVERIFY(page->findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
    }
    void configurationPagesRenderAndKeepKeyboardNavigation_data()
    {
        QTest::addColumn<QSize>("windowSize");
        QTest::newRow("compact") << QSize(1080, 820);
        QTest::newRow("full-screen-content") << QSize(1852, 1025);
    }
    void configurationPagesRenderAndKeepKeyboardNavigation()
    {
        QFETCH(QSize, windowSize);
        TelemetryManager telemetry;
        UAVObjectUtilManager util;
        plugins.addObject(&telemetry); plugins.addObject(&util);
        const auto remove = qScopeGuard([&] {
            plugins.removeObject(&util); plugins.removeObject(&telemetry);
        });
        QPluginLoader loader(QDir(GCS_BUILD_TREE_PATH).filePath("lib/droning/plugins/droniNG/libConfig.so"));
        auto *plugin = qobject_cast<ExtensionSystem::IPlugin *>(loader.instance());
        QVERIFY2(plugin, qPrintable(loader.errorString()));
        QString error;
        QVERIFY2(plugin->initialize({}, &error), qPrintable(error));
        const auto unload = qScopeGuard([&] { loader.unload(); });
        Core::IUAVGadgetFactory *factory = nullptr;
        for (auto *candidate : plugins.getObjects<Core::IUAVGadgetFactory>())
            if (candidate->classId() == "ConfigGadget") factory = candidate;
        QVERIFY(factory);
        std::unique_ptr<Core::IUAVGadget> gadget(factory->createGadget(nullptr));
        auto *widget = gadget->widget();
        widget->resize(windowSize); widget->show();
        auto *tabs = widget->findChild<MyTabbedStackWidget *>();
        QVERIFY(tabs);
        auto *navigation = tabs->findChild<QListWidget *>();
        QVERIFY(navigation);
        QTRY_COMPARE(navigation->count(), 12);
        // Preview capability-specific pages without attaching a physical controller.
        tabs->setHidden(10, false); tabs->setHidden(11, false);
        auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captures.isEmpty() && windowSize.width() > 1080) {
            captures += "/fullscreen";
            QVERIFY(QDir().mkpath(captures));
        }
        const auto renderingIssue = [&]() -> QString {
            for (auto *spin : tabs->currentWidget()->findChildren<QAbstractSpinBox *>()) {
                if (!spin->isVisible()) continue;
                auto *editor = spin->findChild<QLineEdit *>();
                if (!editor || editor->width() < editor->fontMetrics().horizontalAdvance(editor->text())
                        + editor->textMargins().left() + editor->textMargins().right()
                    || editor->height() < editor->fontMetrics().height())
                    return QString("%1 clips its numeric value").arg(spin->objectName());
            }
            for (auto *combo : tabs->currentWidget()->findChildren<QComboBox *>()) {
                if (!combo->isVisible()) continue;
                QStyleOptionComboBox option;
                option.initFrom(combo);
                option.editable = combo->isEditable();
                const QRect text = combo->style()->subControlRect(QStyle::CC_ComboBox, &option,
                    QStyle::SC_ComboBoxEditField, combo);
                if (text.width() < combo->fontMetrics().horizontalAdvance(combo->currentText()))
                    return QString("%1 clips its selected option").arg(combo->objectName());
            }
            for (auto *grid : tabs->currentWidget()->findChildren<QGridLayout *>()) {
                for (int first = 0; first < grid->count(); ++first) {
                    auto *a = grid->itemAt(first)->widget();
                    if (!a || !a->isVisible()
                        || !(qobject_cast<QAbstractSpinBox *>(a) || qobject_cast<QComboBox *>(a))) continue;
                    for (int second = first + 1; second < grid->count(); ++second) {
                        auto *b = grid->itemAt(second)->widget();
                        if (!b || !b->isVisible()
                            || !(qobject_cast<QAbstractSpinBox *>(b) || qobject_cast<QComboBox *>(b))) continue;
                        if (a->geometry().intersects(b->geometry())) {
                            return QString("%1 overlaps %2").arg(a->objectName(), b->objectName());
                        }
                    }
                }
            }
            return {};
        };
        navigation->setFocus();
        tabs->setCurrentIndex(0);
        for (int index = 0; index < navigation->count(); ++index) {
            if (index) QTest::keyClick(navigation, Qt::Key_Down);
            QTRY_COMPARE(tabs->currentWidget(), tabs->getWidget(index));
            QTest::qWait(30);
            QVERIFY(navigation->visualItemRect(navigation->item(index)).intersects(navigation->viewport()->rect()));
            auto issue = renderingIssue();
            if (!issue.isEmpty() && !captures.isEmpty())
                widget->grab().save(captures + "/settings-render-failure.png");
            QVERIFY2(issue.isEmpty(), qPrintable(issue));
            QString name = navigation->item(index)->text().toLower().replace(' ', '-');
            if (!captures.isEmpty()) QVERIFY(widget->grab().save(captures + "/settings-" + name + ".png"));
            if (auto *footer = tabs->currentWidget()->findChild<QWidget *>("settingsActions")) {
                QPushButton *primary = nullptr;
                for (auto *button : footer->findChildren<QPushButton *>()) {
                    if (!button->isVisible() || !button->property("primary").toBool()) continue;
                    primary = button;
                    QVERIFY2(button->mapTo(footer, QPoint()).x() <= 24,
                             "The primary configuration action must start at the left of the footer");
                }
                const QMap<int, QStringList> secondaryActions {
                    {4, {"resetButton", "reloadButton"}},
                    {5, {"stabilizationReloadBoardData_6"}},
                    {6, {"pushButton"}},
                    {7, {"camerastabilizationResetToDefaults", "pushButton"}},
                    {11, {"reloadButton"}}
                };
                for (const auto &name : secondaryActions.value(index)) {
                    auto *button = tabs->currentWidget()->findChild<QPushButton *>(name);
                    QVERIFY2(button, qPrintable(name));
                    QCOMPARE(button->parentWidget(), footer);
                    QVERIFY(primary);
                    QCOMPARE(button->geometry().center().y(), primary->geometry().center().y());
                }
            }
            for (auto *button : tabs->currentWidget()->findChildren<QPushButton *>()) {
                const auto relation = button->property("objrelation").toStringList();
                for (const auto &entry : relation)
                    if (entry.startsWith("url:"))
                        QVERIFY2(entry.startsWith("url:https://github.com/reubenfirmin/droniNG/wiki/"),
                                 qPrintable(entry));
            }
            for (auto *scroll : tabs->currentWidget()->findChildren<QScrollArea *>()) {
                if (!scroll->isVisible() || !scroll->verticalScrollBar()->isVisible()) continue;
                const auto right = scroll->mapTo(widget, scroll->rect().topRight()).x();
                QVERIFY2(widget->width() - right <= 40,
                         "Page scrollbars must be at the right edge, outside the compact form");
            }
            if (index == 5) {
                auto *sectionDefault = tabs->currentWidget()->findChild<QPushButton *>("pushButton_8");
                QVERIFY(sectionDefault);
                QVERIFY(sectionDefault->parentWidget()->objectName() != "settingsActions");
                auto *roll = tabs->currentWidget()->findChild<QDoubleSpinBox *>("fullStickRateRoll");
                auto *pitch = tabs->currentWidget()->findChild<QDoubleSpinBox *>("fullStickRatePitch");
                auto *yaw = tabs->currentWidget()->findChild<QDoubleSpinBox *>("fullStickRateYaw");
                QVERIFY(roll); QVERIFY(pitch); QVERIFY(yaw);
                QCOMPARE(pitch->x() - roll->geometry().right(), 13);
                QCOMPARE(yaw->x() - pitch->geometry().right(), 13);
            }
            if (index == 7) {
                auto *save = tabs->currentWidget()->findChild<QPushButton *>("camerastabilizationSaveSD");
                QVERIFY(save);
                const int bottom = save->mapTo(tabs->currentWidget(), save->rect().bottomRight()).y();
                QVERIFY2(tabs->currentWidget()->height() - bottom <= 16,
                         "The action row must not absorb height intended for the scrollable form");
            }
            // Extra window width belongs outside the fields, so a monitor resize
            // must not turn ordinary numeric inputs and selections into long bars.
            QList<QPair<QWidget *, int>> widths;
            for (auto *field : tabs->currentWidget()->findChildren<QWidget *>())
                if (field->isVisible() && (qobject_cast<QAbstractSpinBox *>(field)
                    || qobject_cast<QComboBox *>(field))) widths.append({field, field->width()});
            widget->resize(windowSize + QSize(520, 0));
            QTest::qWait(10);
            for (const auto &field : widths)
                QVERIFY2(field.first->width() <= field.second,
                         qPrintable(field.first->objectName() + " stretches with the window"));
            widget->resize(windowSize);
            QTest::qWait(10);
            for (auto *subtabs : tabs->currentWidget()->findChildren<QTabWidget *>()) {
                if (!subtabs->isVisible()) continue;
                for (int sub = 0; sub < subtabs->count(); ++sub) {
                    subtabs->setCurrentIndex(sub);
                    QTest::qWait(20);
                    issue = renderingIssue();
                    QVERIFY2(issue.isEmpty(), qPrintable(issue));
                    if (!captures.isEmpty()) QVERIFY(widget->grab().save(captures + QString("/settings-%1-%2-%3.png")
                        .arg(name, subtabs->objectName()).arg(sub)));
                }
                subtabs->setCurrentIndex(0);
            }
            if (index == 1) {
                QVERIFY(!tabs->currentWidget()->findChild<QLabel *>("settingsDraftStatus")->isVisible());
                auto *mixCaption = tabs->currentWidget()->findChild<QLabel *>("mrRollMixValue");
                QVERIFY(mixCaption);
                QVERIFY2(mixCaption->height() <= 32, "Mix captions must not absorb spare page height");
                if (windowSize.width() > 1080) {
                    auto *scroll = tabs->currentWidget()->findChild<QScrollArea *>("scrollArea_1");
                    QVERIFY(scroll);
                    QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
                }
                auto *airframe = tabs->currentWidget()->findChild<QComboBox *>("aircraftType");
                QVERIFY(airframe);
                const int original = airframe->currentIndex();
                for (int type = 0; type < airframe->count(); ++type) {
                    airframe->setCurrentIndex(type);
                    QTest::qWait(20);
                    issue = renderingIssue();
                    QVERIFY2(issue.isEmpty(), qPrintable(issue));
                    if (!captures.isEmpty()) QVERIFY(widget->grab().save(captures + QString("/settings-vehicle-type-%1.png").arg(type)));
                    for (auto *subtabs : tabs->currentWidget()->findChildren<QTabWidget *>()) {
                        if (!subtabs->isVisible()) continue;
                        for (int sub = 0; sub < subtabs->count(); ++sub) {
                            subtabs->setCurrentIndex(sub);
                            QTest::qWait(20);
                            issue = renderingIssue();
                            QVERIFY2(issue.isEmpty(), qPrintable(issue));
                            if (!captures.isEmpty()) QVERIFY(widget->grab().save(captures + QString("/settings-vehicle-type-%1-%2-%3.png")
                                .arg(type).arg(subtabs->objectName()).arg(sub)));
                        }
                        subtabs->setCurrentIndex(0);
                    }
                }
                airframe->setCurrentIndex(original);
                airframe->setCurrentIndex(airframe->findText("Multirotor"));
                auto *frames = tabs->currentWidget()->findChild<QComboBox *>("multirotorFrameType");
                auto *reverse = tabs->currentWidget()->findChild<QCheckBox *>("MultirotorRevMixercheckBox");
                auto *diagram = tabs->currentWidget()->findChild<QGraphicsView *>("quadShape");
                QVERIFY(frames); QVERIFY(reverse); QVERIFY(diagram);
                const int originalFrame = frames->currentIndex();
                for (int frame = 0; frame < frames->count(); ++frame) {
                    frames->setCurrentIndex(frame);
                    for (int motor = 1; motor <= 8; ++motor) {
                        auto *channel = tabs->currentWidget()->findChild<QComboBox *>(
                            QString("multiMotorChannelBox%1").arg(motor));
                        auto *label = tabs->currentWidget()->findChild<QLabel *>(
                            QString("MotorOutputLabel%1").arg(motor));
                        QCOMPARE(channel->isVisible(), label->isVisible());
                        if (channel->isVisible()) QVERIFY(!label->text().isEmpty());
                    }
                    for (bool reversed : {false, true}) {
                        reverse->setChecked(reversed);
                        QVERIFY(QMetaObject::invokeMethod(tabs->currentWidget(), "reverseMultirotorMotor"));
                        QTest::qWait(20);
                        const QRect rendered = diagram->mapFromScene(diagram->sceneRect()).boundingRect();
                        QVERIFY2(diagram->viewport()->rect().adjusted(-1, -1, 1, 1).contains(rendered),
                                 "Rotor directions must fit within the diagram viewport");
                        QVERIFY2(diagram->viewport()->visibleRegion().contains(rendered),
                                 "The complete rotor diagram must be visible without scrolling");
                        if (!captures.isEmpty()) QVERIFY(widget->grab().save(captures
                            + QString("/settings-rotors-%1-%2.png").arg(frame).arg(reversed ? "reverse" : "normal")));
                    }
                }
                reverse->setChecked(false);
                QVERIFY(QMetaObject::invokeMethod(tabs->currentWidget(), "reverseMultirotorMotor"));
                frames->setCurrentIndex(originalFrame);
                airframe->setCurrentIndex(original);
                navigation->setFocus();
            }
            if (index == 9) {
                QVERIFY(tabs->currentWidget()->findChildren<QTextEdit *>().isEmpty());
                auto *warning = tabs->currentWidget()->findChild<QLabel *>("autotuneWarning");
                QVERIFY(warning); QVERIFY(warning->wordWrap());
                QCOMPARE(warning->property("role").toString(), QString("notice"));
                auto *scroll = tabs->currentWidget()->findChild<QScrollArea *>("autotuneScrollArea");
                QVERIFY(scroll);
                QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
            }
            if (index == 2) {
                const auto rows = tabs->currentWidget()->findChildren<QLabel *>("channelName");
                QVERIFY(rows.size() >= 10);
                const auto columnsAligned = [&] {
                    const char *fields[] = {"channelName", "channelGroup", "channelNumberDropdown",
                        "channelMin", "channelNeutral", "channelMax", "channelCurrent", "btnReverse"};
                    const auto bounds = [&](QWidget *control) {
                        return QRect(control->mapTo(tabs->currentWidget(), QPoint()), control->size());
                    };
                    int previousRight = -1;
                    for (const char *field : fields) {
                        const auto reference = bounds(rows.first()->parentWidget()->findChild<QWidget *>(field));
                        if (reference.left() <= previousRight) return false;
                        previousRight = reference.right();
                        for (auto *row : rows) {
                            auto *control = row->parentWidget()->findChild<QWidget *>(field);
                            if (!control || control->isHidden()) continue;
                            const auto rect = bounds(control);
                            if (rect.left() != reference.left() || rect.width() != reference.width()) return false;
                        }
                    }
                    return true;
                };
                QTRY_VERIFY(columnsAligned());
                // A longer channel name must resize the shared column, not shift
                // one row's receiver and calibration fields sideways.
                auto *label = rows.last();
                const QString original = label->text();
                label->setText("Accessory channel");
                QTRY_VERIFY(columnsAligned());
                label->setText(original);
                QTRY_VERIFY(columnsAligned());
            }
            if (index == 3) {
                auto *neutral = tabs->currentWidget()->findChild<QSpinBox *>("actuatorNeutralValue");
                QVERIFY(neutral);
                auto *form = neutral->parentWidget();
                const auto rows = tabs->currentWidget()->findChildren<QSpinBox *>("actuatorNeutralValue");
                const auto columnsAligned = [&] {
                    const auto bounds = [&](QWidget *control) {
                        return QRect(control->mapTo(tabs->currentWidget(), QPoint()), control->size());
                    };
                    const char *fields[] = {"actuatorNumber", "actuatorName", "actuatorMin",
                        "actuatorNeutralValue", "actuatorMax", "pb_reverseActuator", "actuatorLink"};
                    const char *headings[] = {"legend6", "legend0", "legend1", "legend2",
                        "legend3", "legend3_2", "legend5"};
                    int previousRight = -1;
                    for (int column = 0; column < 7; ++column) {
                        const auto reference = bounds(form->findChild<QWidget *>(fields[column]));
                        if (reference.left() <= previousRight) return false;
                        previousRight = reference.right();
                        for (auto *row : rows) {
                            const auto rect = bounds(row->parentWidget()->findChild<QWidget *>(fields[column]));
                            if (rect.left() != reference.left() || rect.width() != reference.width()) return false;
                        }
                        const auto heading = bounds(form->findChild<QWidget *>(headings[column]));
                        if (column == 6) {
                            if (qAbs(heading.center().x() - reference.center().x()) > 1) return false;
                        } else if (heading.left() != reference.left() || heading.width() != reference.width()) return false;
                    }
                    return true;
                };
                QTRY_VERIFY(columnsAligned());
                // Different assignments, PWM ranges and digital protocols must
                // never move a channel out from under its column heading.
                for (int row = 0; row < rows.size(); ++row) {
                    auto *channel = rows[row]->parentWidget();
                    QVERIFY(QMetaObject::invokeMethod(channel, "setAssignment",
                        Q_ARG(QString, row % 2 ? QString("Camera pitch servo") : QString("Motor NW"))));
                    QVERIFY(QMetaObject::invokeMethod(channel, "updateChannelLimits", Q_ARG(int, row % 2 ? 1000 : 48),
                        Q_ARG(int, row % 2 ? 2000 : 2047), Q_ARG(bool, !(row % 2))));
                }
                QTest::qWait(30);
                QTRY_VERIFY(columnsAligned());
                if (!captures.isEmpty()) QVERIFY(widget->grab().save(captures + "/settings-output-assigned.png"));
                auto *slider = form->findChild<QSlider *>("actuatorNeutral");
                QVERIFY(slider);
                QSignalSpy outputChanges(form, SIGNAL(channelChanged(int,int)));
                QSignalSpy edits(form, SIGNAL(formChanged()));
                QVERIFY(outputChanges.isValid()); QVERIFY(edits.isValid());
                form->setEnabled(false);
                QVERIFY(QMetaObject::invokeMethod(form, "updateChannelLimits", Q_ARG(int, 1000),
                    Q_ARG(int, 2000), Q_ARG(bool, false)));
                QVERIFY(QMetaObject::invokeMethod(form, "setMinmax", Q_ARG(int, 1000), Q_ARG(int, 2000)));
                QVERIFY(neutral->isEnabledTo(form));
                form->setEnabled(true);
                neutral->setValue(1501);
                QCOMPARE(slider->value(), 1501);
                QVERIFY(edits.size() > 0);
                QCOMPARE(outputChanges.size(), 0); // Editing neutral never tests motors.
                QVERIFY(QMetaObject::invokeMethod(form, "enableChannelTest", Q_ARG(bool, true)));
                QVERIFY(slider->isVisible()); QVERIFY(!neutral->isVisible());
                QTRY_VERIFY(slider->width() >= 200);
                QTRY_VERIFY(columnsAligned());
                if (!captures.isEmpty()) QVERIFY(widget->grab().save(captures + "/settings-output-testing.png"));
                slider->setValue(1602);
                QCOMPARE(neutral->value(), 1602);
                QCOMPARE(outputChanges.size(), 1);
                QVERIFY(QMetaObject::invokeMethod(form, "enableChannelTest", Q_ARG(bool, false)));
                QVERIFY(neutral->isVisible()); QVERIFY(!slider->isVisible());
                QTRY_VERIFY(columnsAligned());
                QVERIFY(QMetaObject::invokeMethod(form, "setMinmax", Q_ARG(int, 2000), Q_ARG(int, 1000)));
                QCOMPARE(neutral->minimum(), 1000); QCOMPARE(neutral->maximum(), 2000);
                neutral->setValue(2500);
                QCOMPARE(slider->value(), 2000);
                QCOMPARE(outputChanges.size(), 1);
                QVERIFY(QMetaObject::invokeMethod(form, "updateChannelLimits", Q_ARG(int, 48),
                    Q_ARG(int, 2047), Q_ARG(bool, true)));
                QCOMPARE(neutral->minimum(), 48); QCOMPARE(neutral->maximum(), 2047);
                QVERIFY(neutral->suffix().isEmpty());
            }
        }
    }
    void configurationDraftsSurviveNavigationAndControllerUpdates()
    {
        TelemetryManager telemetry;
        UAVObjectUtilManager util;
        plugins.addObject(&telemetry); plugins.addObject(&util);
        const auto remove = qScopeGuard([&] {
            plugins.removeObject(&util); plugins.removeObject(&telemetry);
        });
        QPluginLoader loader(QDir(GCS_BUILD_TREE_PATH).filePath("lib/droning/plugins/droniNG/libConfig.so"));
        auto *plugin = qobject_cast<ExtensionSystem::IPlugin *>(loader.instance());
        QVERIFY2(plugin, qPrintable(loader.errorString()));
        QString error;
        QVERIFY2(plugin->initialize({}, &error), qPrintable(error));
        const auto unload = qScopeGuard([&] { loader.unload(); });
        Core::IUAVGadgetFactory *factory = nullptr;
        for (auto *candidate : plugins.getObjects<Core::IUAVGadgetFactory>())
            if (candidate->classId() == "ConfigGadget") factory = candidate;
        QVERIFY(factory);
        std::unique_ptr<Core::IUAVGadget> gadget(factory->createGadget(nullptr));
        auto *widget = gadget->widget();
        widget->resize(1852, 1025); widget->show();
        auto *tabs = widget->findChild<MyTabbedStackWidget *>();
        auto *navigation = tabs->findChild<QListWidget *>();
        QTRY_COMPARE(navigation->count(), 12);
        tabs->setCurrentIndex(5);
        QTRY_COMPARE(tabs->currentWidget(), tabs->getWidget(5));
        auto *tuning = tabs->currentWidget();
        auto *rate = tuning->findChild<QDoubleSpinBox *>("fullStickRateRoll");
        auto *status = tuning->findChild<QLabel *>("settingsDraftStatus");
        auto *discard = tuning->findChild<QPushButton *>("settingsDiscard");
        QVERIFY(rate); QVERIFY(status); QVERIFY(discard);
        QVERIFY(QMetaObject::invokeMethod(tuning, "discardChanges"));
        QVERIFY(!status->isVisible());
        const double original = rate->value();
        rate->setValue(original + 17);
        QVERIFY(status->isVisible()); QVERIFY(discard->isVisible());
        QVERIFY(navigation->item(5)->text().endsWith(" •"));
        auto *settings = objects->getObject("StabilizationSettings");
        QVERIFY(settings);
        const auto controllerRate = settings->getField("ManualRate")->getValue(0);
        QCOMPARE(controllerRate.toDouble(), original);
        // A received settings update must not overwrite the draft.
        QVERIFY(QMetaObject::invokeMethod(settings, "objectUpdated", Q_ARG(UAVObject *, settings)));
        QCOMPARE(rate->value(), original + 17);
        tabs->setCurrentIndex(2);
        QTRY_COMPARE(tabs->currentWidget(), tabs->getWidget(2));
        QVERIFY(QMetaObject::invokeMethod(settings, "objectUpdated", Q_ARG(UAVObject *, settings)));
        QCOMPARE(rate->value(), original + 17);
        tabs->setCurrentIndex(5);
        QTRY_COMPARE(tabs->currentWidget(), tuning);
        QCOMPARE(rate->value(), original + 17);
        for (auto *top : QApplication::topLevelWidgets())
            QVERIFY2(!qobject_cast<QMessageBox *>(top) || !top->isVisible(), "Navigation must not open an unsaved-changes modal");
        if (const auto captures = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR"); !captures.isEmpty())
            QVERIFY(widget->grab().save(captures + "/settings-unsaved-draft.png"));
        QTest::mouseClick(discard, Qt::LeftButton);
        QCOMPARE(rate->value(), controllerRate.toDouble());
        QVERIFY(!status->isVisible()); QVERIFY(!discard->isVisible());
        QVERIFY(!navigation->item(5)->text().endsWith(" •"));
    }
    void controllerSetupOfferAndExplicitWizard_data()
    {
        QTest::addColumn<int>("boardType");
        QTest::addColumn<bool>("dshotSupported");
        QTest::newRow("legacy-RE1") << 0x8b << true;
        QTest::newRow("native-Radix") << 224 << false;
        QTest::newRow("native-OmnibusF4SD") << 225 << false;
    }
    void controllerSetupOfferAndExplicitWizard()
    {
        QFETCH(int, boardType);
        QFETCH(bool, dshotSupported);
        TelemetryManager telemetry;
        UAVObjectUtilManager util;
        plugins.addObject(&telemetry); plugins.addObject(&util);
        auto *actuators = ActuatorSettings::GetInstance(objects);
        auto *iap = FirmwareIAPObj::GetInstance(objects);
        const auto previousActuators = actuators->getData();
        const bool previouslyPresent = actuators->getIsPresentOnHardware();
        const auto previousIap = iap->getData();
        const auto restore = qScopeGuard([&] {
            QMetaObject::invokeMethod(&telemetry, "onDisconnect", Qt::DirectConnection);
            actuators->setData(previousActuators); actuators->setIsPresentOnHardware(previouslyPresent);
            iap->setData(previousIap);
            plugins.removeObject(&util); plugins.removeObject(&telemetry);
        });
        const QDir build(GCS_BUILD_TREE_PATH);
        QPluginLoader brain(build.filePath("lib/droning/plugins/droniNG/libBrainFPV.so"));
        QPluginLoader imported(build.filePath("lib/droning/plugins/droniNG/libDroning.so"));
        QPluginLoader config(build.filePath("lib/droning/plugins/droniNG/libConfig.so"));
        QPluginLoader setup(build.filePath("lib/droning/plugins/droniNG/libSetupWizard.so"));
        auto *configPlugin = qobject_cast<ExtensionSystem::IPlugin *>(config.instance());
        auto *brainPlugin = qobject_cast<ExtensionSystem::IPlugin *>(brain.instance());
        auto *importedPlugin = qobject_cast<ExtensionSystem::IPlugin *>(imported.instance());
        auto *setupPlugin = qobject_cast<ExtensionSystem::IPlugin *>(setup.instance());
        QVERIFY2(configPlugin, qPrintable(config.errorString()));
        QVERIFY2(brainPlugin, qPrintable(brain.errorString()));
        QVERIFY2(importedPlugin, qPrintable(imported.errorString()));
        QVERIFY2(setupPlugin, qPrintable(setup.errorString()));
        QString error;
        QVERIFY2(brainPlugin->initialize({}, &error), qPrintable(error));
        brainPlugin->extensionsInitialized();
        QVERIFY2(importedPlugin->initialize({}, &error), qPrintable(error));
        importedPlugin->extensionsInitialized();
        QVERIFY2(configPlugin->initialize({}, &error), qPrintable(error));
        QVERIFY2(setupPlugin->initialize({}, &error), qPrintable(error));
        setupPlugin->extensionsInitialized();
        const auto unload = qScopeGuard([&] {
            for (auto *wizard : manager->window()->findChildren<QWizard *>("controllerSetupWizard"))
                wizard->reject();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            setup.unload(); config.unload(); imported.unload(); brain.unload();
        });
        auto metadata = previousIap;
        metadata.BoardType = boardType;
        metadata.BoardRevision = 1;
        std::fill(std::begin(metadata.Description), std::end(metadata.Description), 0);
        std::copy_n("TlFw", 4, metadata.Description);
        const auto hash = QByteArray::fromHex(util.getGcsHash().toLatin1());
        QCOMPARE(hash.size(), 4);
        for (int i = 0; i < 4; ++i) metadata.Description[4 + i] = quint8(hash[3 - i]);
        const auto uavoHash = Core::FirmwareInstallImage::gcsUavoHash();
        std::copy(uavoHash.begin(), uavoHash.end(), metadata.Description + 60);
        iap->setData(metadata);
        QVERIFY(util.firmwareHashMatchesGcs());
        auto outputs = previousActuators;
        for (quint32 i = 0; i < ActuatorSettings::CHANNELMIN_NUMELEM; ++i) outputs.ChannelMin[i] = 0;
        for (quint32 i = 0; i < ActuatorSettings::CHANNELMAX_NUMELEM; ++i) outputs.ChannelMax[i] = 0;
        actuators->setIsPresentOnHardware(true); actuators->setData(outputs);
        TestConnection controller;
        QVERIFY(manager->connectDevice({&controller, controller.device}));
        QVERIFY(!manager->controllerSetupRequired()); // Local defaults are not a detection result.
        QSignalSpy requests(manager, &Core::ConnectionManager::setupWizardRequested);
        QSignalSpy workspaceChanges(Core::ModeManager::instance(), &Core::ModeManager::currentModeChanged);
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG", "DroniNG");
        QVERIFY(QMetaObject::invokeMethod(&telemetry, "onConnect", Qt::DirectConnection));
        manager->reportFlightDataReceived();
        QVERIFY(manager->controllerSetupRequired());
        manager->telemetryConnectionFailed({"No live data", {}, {}});
        QVERIFY(!manager->controllerSetupRequired());
        manager->reportFlightDataReceived();
        QVERIFY(manager->controllerSetupRequired());
        QVERIFY(!manager->window()->findChild<QWizard *>("controllerSetupWizard"));
        QCOMPARE(requests.size(), 0);
        QCOMPARE(workspaceChanges.size(), 0);
        QCOMPARE(page->findChild<QLabel *>("firmwarePageTitle")->text(), QString("Set up controller?"));
        QVERIFY(page->findChild<QLabel *>("firmwareSetupIntro")->isVisible());
        QVERIFY(!page->findChild<QWidget *>("firmwareRecoveryBody")->isVisible());
        auto *launch = page->findChild<QPushButton *>("firmwareConfigureAction");
        QCOMPARE(launch->text(), QString("Launch setup wizard"));
        const QSize originalSize = page->size();
        auto *overview = page->findChild<QWidget *>("firmwareOverview");
        for (const auto &size : {QSize(1100, 740), QSize(1320, 1100), QSize(680, 740), QSize(800, 740)}) {
            page->resize(size); QCoreApplication::processEvents();
            QTRY_COMPARE(page->size(), size);
            auto *intro = page->findChild<QLabel *>("firmwareSetupIntro");
            const QRect introBounds(intro->mapTo(overview, QPoint()), intro->size());
            int actionBottom = 0;
            for (const auto *name : {"firmwareConfigureAction", "firmwareScopesAction", "firmwareDisconnectAction"}) {
                auto *button = page->findChild<QPushButton *>(name);
                if (!button->isVisible()) continue;
                const QRect bounds(button->mapTo(overview, QPoint()), button->size());
                QVERIFY(overview->rect().contains(bounds));
                QVERIFY2(bounds.top() >= introBounds.bottom(), "Setup actions must follow the controller details");
                actionBottom = qMax(actionBottom, bounds.bottom());
            }
            QVERIFY2(overview->height() - actionBottom <= 32, "Controller actions must stay at the bottom of the card");
            QVERIFY(overview->rect().contains(QRect(launch->mapTo(overview, QPoint()), launch->size())));
        }
        page->resize(originalSize); QCoreApplication::processEvents();
        captureFirmwarePage("firmware-controller-needs-setup");
        QTRY_VERIFY(launch->isVisible() && launch->isEnabled() && launch->height() > 0);
        QTest::mouseClick(launch, Qt::LeftButton);
        QPointer<QWizard> wizard = manager->window()->findChild<QWizard *>("controllerSetupWizard");
        QVERIFY(wizard); QVERIFY(wizard->isVisible()); QCOMPARE(requests.size(), 1);
        QCOMPARE(workspaceChanges.size(), 0);
        QWizardPage *outputsPage = nullptr;
        for (int id : wizard->pageIds())
            if (wizard->page(id)->findChild<QToolButton *>("dshot300Button")) outputsPage = wizard->page(id);
        QVERIFY(outputsPage);
        outputsPage->findChild<QToolButton *>("dshot300Button")->setChecked(true);
        outputsPage->initializePage();
        for (const auto *name : {"dshot300Button", "dshot600Button", "dshot1200Button"})
            QCOMPARE(outputsPage->findChild<QToolButton *>(name)->isEnabled(), dshotSupported);
        if (!dshotSupported) {
            QVERIFY(outputsPage->findChild<QToolButton *>("oneShot125Button")->isChecked());
            QVERIFY(outputsPage->validatePage());
            outputsPage->findChild<QToolButton *>("dshot600Button")->setChecked(true);
            QVERIFY(!outputsPage->validatePage());
        }
        // Cancellation must close immediately without a second question.
        QTimer cancellationGuard;
        bool suppressionPromptSeen = false;
        connect(&cancellationGuard, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *prompt = qobject_cast<QMessageBox *>(widget)) {
                    suppressionPromptSeen = true; prompt->reject();
                }
        });
        cancellationGuard.start(250);
        wizard->reject();
        cancellationGuard.stop();
        QVERIFY(!suppressionPromptSeen);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(wizard.isNull());
        QCOMPARE(launch->text(), QString("Launch setup wizard"));
        QVERIFY(manager->controllerSetupRequired());
        QTest::mouseClick(launch, Qt::LeftButton);
        wizard = manager->window()->findChild<QWizard *>("controllerSetupWizard");
        QVERIFY(wizard); QCOMPARE(requests.size(), 2);
        wizard->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        outputs.ChannelMin[0] = 1000; actuators->setData(outputs);
        QVERIFY(!manager->controllerSetupRequired());
        QCOMPARE(launch->text(), QString("Tune && configure"));
        QVERIFY(!page->findChild<QLabel *>("firmwareSetupIntro")->isVisible());
        captureFirmwarePage("firmware-controller-ready");
        outputs.ChannelMin[0] = 0; outputs.ChannelMax[0] = 2000; actuators->setData(outputs);
        QVERIFY(!manager->controllerSetupRequired());
        outputs.ChannelMax[0] = 0; actuators->setData(outputs);
        QVERIFY(manager->controllerSetupRequired());
        QVERIFY(QMetaObject::invokeMethod(&telemetry, "onDisconnect", Qt::DirectConnection));
        manager->telemetryDisconnected();
        QVERIFY(!manager->controllerSetupRequired());
        QVERIFY(!launch->isVisible());
        QVERIFY(QMetaObject::invokeMethod(&telemetry, "onConnect", Qt::DirectConnection));
        manager->reportFlightDataReceived();
        QVERIFY(manager->controllerSetupRequired());
        manager->disconnectDevice();
        QVERIFY(!manager->controllerSetupRequired());
        manager->requestControllerSetup();
        QCOMPARE(requests.size(), 2);
        QVERIFY(QMetaObject::invokeMethod(&telemetry, "onDisconnect", Qt::DirectConnection));
        outputs.ChannelMax[0] = 2000; actuators->setData(outputs);
        TestConnection configuredController;
        QVERIFY(manager->connectDevice({&configuredController, configuredController.device}));
        QVERIFY(QMetaObject::invokeMethod(&telemetry, "onConnect", Qt::DirectConnection));
        manager->reportFlightDataReceived();
        QVERIFY(!manager->controllerSetupRequired());
        QCOMPARE(launch->text(), QString("Tune && configure"));
        QCOMPARE(requests.size(), 2);
        // Unknown hardware objects and incompatible firmware are not evidence
        // that a controller needs its outputs configured.
        outputs.ChannelMax[0] = 0; actuators->setIsPresentOnHardware(false); actuators->setData(outputs);
        QVERIFY(!manager->controllerSetupRequired());
        metadata.Description[4] ^= 1; iap->setData(metadata);
        actuators->setIsPresentOnHardware(true); actuators->setData(outputs);
        QVERIFY(!manager->controllerSetupRequired());
    }
    void firmwareCompatibilitySurvivesApplicationCommits()
    {
        const auto board = Core::FirmwareCatalog::instance().board("revolution");
        const auto image = Core::FirmwareInstallImage::load(board, Core::FirmwareCatalog::imagePath(
            board, Core::FirmwareCatalog::ImageKind::FullInstall));
        QVERIFY2(image.valid(), qPrintable(image.error));
        auto *iap = FirmwareIAPObj::GetInstance(objects);
        const auto previous = iap->getData();
        const auto restore = qScopeGuard([&] { iap->setData(previous); });
        auto metadata = previous;
        std::copy(image.descriptor.begin(), image.descriptor.end(), metadata.Description);
        metadata.Description[4] ^= 1; // Same executable, a different application commit.
        iap->setData(metadata);
        UAVObjectUtilManager util;
        QVERIFY(util.firmwareHashMatchesGcs());
        metadata.Description[40] ^= 1; iap->setData(metadata);
        QVERIFY(!util.firmwareHashMatchesGcs());
        metadata.Description[40] ^= 1; metadata.Description[60] ^= 1; iap->setData(metadata);
        QVERIFY(!util.firmwareHashMatchesGcs());
    }
    void mspFirmwareDiagnosis_data()
    {
        QTest::addColumn<QByteArray>("variant");
        QTest::addColumn<QString>("guidance");
        QTest::newRow("betaflight") << QByteArray("BTFL") << QString("answered as Betaflight");
        QTest::newRow("dronin-msp-port") << QByteArray("DRON") << QString("dRonin on an MSP port");
        QTest::newRow("other-msp") << QByteArray("INAV") << QString("answered using MSP");
        QTest::newRow("unidentified-msp") << QByteArray() << QString("answered using MSP");
    }
    void mspFirmwareDiagnosis()
    {
        QFETCH(QByteArray, variant);
        QFETCH(QString, guidance);
        TestConnection connection("OmnibusF4 - ttyACM0");
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        auto *flight = FlightTelemetryStats::GetInstance(objects);
        auto flightData = flight->getData();
        flightData.Status = FlightTelemetryStats::STATUS_DISCONNECTED;
        flight->setData(flightData);
        MessageCapture messages;
        UAVTalk protocol(connection.io, objects);
        Telemetry telemetry(&protocol, objects);
        TelemetryMonitor monitor(objects, &telemetry);
        connection.io->onWrite = [&](const QByteArray &bytes) {
            if (!bytes.startsWith("$M<"))
                return;
            // Assert that every diagnostic request is a zero-payload identity getter.
            for (int i = 0; i < bytes.size(); i += 6) {
                QCOMPARE(bytes.mid(i, 3), QByteArray("$M<"));
                QCOMPARE(bytes[i + 3], char(0));
                const quint8 command = quint8(bytes[i + 4]);
                QVERIFY(command >= 1 && command <= 4);
                QCOMPARE(quint8(bytes[i + 5]), command);
                connection.io->receive(mspFrame(command, command == 1
                    ? QByteArray::fromHex("000124") : command == 2 ? variant
                    : command == 3 ? QByteArray::fromHex("030203")
                    : QByteArray::fromHex("4f424634000002")));
            }
        };
        monitor.processStatsUpdates();
        QTRY_VERIFY_WITH_TIMEOUT(!manager->firmwareError().message.isEmpty(), 2000);
        QVERIFY(messages.warnings.isEmpty());
        auto *dialog = page;
        QVERIFY(dialog->isVisible());
        QVERIFY(dialog->findChild<QLabel *>("connectionErrorGuidance")->text().contains(guidance));
        QCOMPARE(dialog->findChild<QLabel *>("connectionErrorGuidance")->isVisible(), variant == "DRON");
        if (variant == "DRON") {
            QVERIFY(!dialog->findChild<QPushButton *>("firmwareInstallAction")->isVisible());
            QVERIFY(!dialog->findChild<QLabel *>("firmwareStep1"));
        }
        QVERIFY(dialog->findChild<QPlainTextEdit *>("connectionErrorDetails")->toPlainText().contains("3.2.3"));
        QVERIFY(messages.information.join('\n').contains("Read-only firmware probe"));
        QVERIFY(connection.io->isOpen());
        QCOMPARE(manager->hasMspFirmware(), true);
        QVERIFY(!dialog->findChild<QPushButton *>("findFirmwareButton"));
        QCOMPARE(manager->firmwareVersion(), QString("3.2.3"));
        QVERIFY(!manager->firmwareError().message.contains("still retrying"));
        QVERIFY(!manager->firmwareDetectionActive());
        QTest::qWait(700); // Drain any request already queued before MSP identification.
        const auto written = connection.io->written;
        GCSTelemetryStats::GetInstance(objects)->updated();
        QTest::qWait(700);
        QCOMPARE(connection.io->written, written);
        const QString captureDirectory = qEnvironmentVariable("CONNECTION_DIALOG_CAPTURE_DIR");
        if (!captureDirectory.isEmpty() && variant == "BTFL")
            QVERIFY(dialog->grab().save(captureDirectory + "/betaflight-telemetry-dialog.png"));
        if (variant == "BTFL") {
            QTest::qWait(5500); // Cover the normal five-second GCS heartbeat too.
            QCOMPARE(connection.io->written, written);
            QCOMPARE(manager->firmwareBoardTarget(), QString("OBF4"));
            QCOMPARE(page->findChild<QComboBox *>("firmwareBoardModel")->currentData().toString(),
                     QString("hglrc-f4-v6pro"));
            QCOMPARE(page->findChild<QLabel *>("firmwareDetectedFirmware")->text(), QString("Betaflight 3.2.3"));
            QVERIFY(page->findChild<QPushButton *>("firmwareInstallAction"));
            if (!captureDirectory.isEmpty())
                QVERIFY(page->grab().save(captureDirectory + "/hglrc-firmware-state.png"));
            // A native stream arriving later resumes the handshake and clears
            // the foreign-firmware state without requiring a new connection.
            FlightTelemetryStats boardFlight;
            flightData.Status = FlightTelemetryStats::STATUS_HANDSHAKEACK;
            boardFlight.setData(flightData);
            connection.io->receive(objectFrame(&boardFlight));
            QVERIFY(messages.information.join('\n').contains("UAVTalk handshake completed"));
            QVERIFY(manager->firmwareError().message.isEmpty());
            QVERIFY(!manager->hasMspFirmware());
        }
        manager->disconnectDevice();
        if (variant == "DRON") {
            QVERIFY(!manager->firmwareError().message.isEmpty());
            manager->clearControllerRecovery();
        }
        QVERIFY(manager->firmwareError().message.isEmpty());
        QVERIFY(manager->firmwareBoardTarget().isEmpty());
        QVERIFY(!manager->hasMspFirmware());
    }
    void nativeRetrievalPreservesLiveData_data()
    {
        QTest::addColumn<bool>("earlyAttitude");
        QTest::addColumn<bool>("attitudePresent");
        QTest::newRow("attitude-arrives-during-retrieval") << true << true;
        QTest::newRow("attitude-first-retrieved-on-request") << false << true;
        QTest::newRow("missing-attitude-explained-and-later-recovered") << false << false;
    }
    void nativeRetrievalPreservesLiveData()
    {
        QFETCH(bool, earlyAttitude);
        QFETCH(bool, attitudePresent);
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        auto *flight = FlightTelemetryStats::GetInstance(objects);
        auto flightData = flight->getData();
        flightData.Status = FlightTelemetryStats::STATUS_DISCONNECTED;
        flight->setData(flightData);
        for (const auto &map : objects->getObjects())
            for (auto *object : map)
                if (auto *data = dynamic_cast<UAVDataObject *>(object)) data->resetIsPresentOnHardware();
        UAVTalk protocol(connection.io, objects);
        Telemetry telemetry(&protocol, objects);
        TelemetryMonitor monitor(objects, &telemetry);
        QSignalSpy retrieved(&monitor, &TelemetryMonitor::connected);
        AttitudeActual boardAttitude;
        auto attitudeData = boardAttitude.getData(); attitudeData.Roll = 12.5f;
        boardAttitude.setData(attitudeData);
        int attitudeRequests = 0;
        connection.io->onWrite = [&](const QByteArray &request) {
            if (request.size() < 9 || quint8(request[0]) != 0x3c || quint8(request[1]) != 0x21) return;
            quint32 id = 0;
            for (int byte = 0; byte < 4; ++byte) id |= quint32(quint8(request[4 + byte])) << (8 * byte);
            auto *object = objects->getObject(id);
            if (id == boardAttitude.getObjID()) ++attitudeRequests;
            if (!object || !object->isSingleInstance() || (!attitudePresent && id == boardAttitude.getObjID()))
                connection.io->receive(nackFrame(id));
            else connection.io->receive(objectFrame(id == boardAttitude.getObjID() ? &boardAttitude : object));
        };
        monitor.processStatsUpdates();
        FlightTelemetryStats boardFlight;
        flightData.Status = FlightTelemetryStats::STATUS_HANDSHAKEACK; boardFlight.setData(flightData);
        connection.io->receive(objectFrame(&boardFlight));
        manager->setNativeFirmwareIdentity("Revolution", "DroniNG 1.0");
        if (earlyAttitude) connection.io->receive(objectFrame(&boardAttitude));
        QTRY_COMPARE_WITH_TIMEOUT(retrieved.count(), 1, 6000);
        QCOMPARE(attitudeRequests, earlyAttitude ? 0 : 1);
        if (!attitudePresent) {
            QVERIFY(!manager->flightDataAvailable());
            QVERIFY(QMetaObject::invokeMethod(&monitor, "liveDataTimeoutCB", Qt::DirectConnection));
            QVERIFY(!manager->firmwareDetectionActive());
            QVERIFY(manager->firmwareError().message.contains("isn't sending live flight data"));
            QVERIFY(!page->findChild<QProgressBar *>("firmwareDetectionProgress")->isVisible());
            connection.io->receive(objectFrame(&boardAttitude));
            QVERIFY(manager->firmwareError().message.isEmpty());
        }
        QVERIFY(manager->flightDataAvailable());
        QVERIFY(!manager->firmwareDetectionActive());
        QCOMPARE(manager->detectedBoard(), QString("Revolution"));
        QCOMPARE(manager->detectedFirmware(), QString("DroniNG 1.0"));
        QVERIFY(page->findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
        QVERIFY(!page->findChild<QProgressBar *>("firmwareDetectionProgress")->isVisible());
        QCOMPARE(AttitudeActual::GetInstance(objects)->getData().Roll, 12.5f);
        captureFirmwarePage("firmware-native-retrieval-complete");
        // A later packet must still be observed after its retrieval request has
        // completed; completing that request must not remove live subscriptions.
        manager->telemetryDisconnected();
        QVERIFY(!manager->flightDataAvailable());
        attitudeData.Roll = 25.0f; boardAttitude.setData(attitudeData);
        connection.io->receive(objectFrame(&boardAttitude));
        QVERIFY(manager->flightDataAvailable());
        QVERIFY(!manager->firmwareDetectionActive());
        // Flight-stat subscriptions must survive retrieval too.
        flightData.Status = FlightTelemetryStats::STATUS_DISCONNECTED; boardFlight.setData(flightData);
        connection.io->receive(objectFrame(&boardFlight));
        QVERIFY(!manager->flightDataAvailable());
    }
    void nativeProfileIdentityRequiresReceivedPacket()
    {
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        UAVTalk protocol(connection.io, objects);
        Telemetry telemetry(&protocol, objects);
        TelemetryMonitor monitor(objects, &telemetry);
        auto *profile = BoardProfile::GetInstance(objects);
        profile->resetIsPresentOnHardware();
        QSignalSpy received(manager, &Core::ConnectionManager::firmwareProfileReceived);
        BoardProfile remote;
        auto data = remote.getData();
        const QByteArray id("radix"), layout("radix-f446-rom-v1");
        const auto hash = Core::FirmwareInstallImage::importedProfileHash();
        data.SchemaVersion = 1;
        memset(data.ProfileId, 0, sizeof(data.ProfileId));
        memset(data.LayoutId, 0, sizeof(data.LayoutId));
        memcpy(data.ProfileId, id.constData(), id.size());
        memcpy(data.LayoutId, layout.constData(), layout.size());
        memcpy(data.ProfileHash, hash.constData(), sizeof(data.ProfileHash));
        // A flight packet supplies this read-only object; its GCS setter must refuse writes.
        remote.unpack(reinterpret_cast<const quint8 *>(&data));
        profile->setData(data); // Cached values must not prove a live identity.
        QCOMPARE(received.count(), 0);
        connection.io->receive(objectFrame(&remote));
        QCOMPARE(received.count(), 1);
        QCOMPARE(received.first()[0].toInt(), 1);
        QCOMPARE(received.first()[1].toString(), QString::fromLatin1(id));
        QCOMPARE(received.first()[2].toString(), QString::fromLatin1(layout));
        QCOMPARE(received.first()[3].toByteArray(), hash);
        manager->disconnectDevice();
        profile->unpack(reinterpret_cast<const quint8 *>(&data));
        QCOMPARE(received.count(), 1); // The old transport cannot certify a new connection.
    }
    void successfulHandshakeDoesNotWarn()
    {
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        auto *flight = FlightTelemetryStats::GetInstance(objects);
        auto flightData = flight->getData();
        flightData.Status = FlightTelemetryStats::STATUS_DISCONNECTED;
        flight->setData(flightData);
        MessageCapture messages;
        UAVTalk protocol(connection.io, objects);
        Telemetry telemetry(&protocol, objects);
        TelemetryMonitor monitor(objects, &telemetry);
        monitor.processStatsUpdates();
        FlightTelemetryStats boardFlight;
        flightData.Status = FlightTelemetryStats::STATUS_HANDSHAKEACK;
        boardFlight.setData(flightData);
        connection.io->receive(objectFrame(&boardFlight));
        QVERIFY(messages.information.join('\n').contains("UAVTalk handshake completed"));
        flightData.Status = FlightTelemetryStats::STATUS_CONNECTED;
        boardFlight.setData(flightData);
        QTimer boardTimer;
        connect(&boardTimer, &QTimer::timeout, this, [&] {
            connection.io->receive(objectFrame(&boardFlight));
        });
        boardTimer.start(300);
        QTest::qWait(8500);
        QVERIFY2(messages.warnings.isEmpty(), qPrintable(messages.warnings.join('\n')));
    }
    void recoveryOrDisconnectDuringProbe_data()
    {
        QTest::addColumn<bool>("recover");
        QTest::newRow("late-uavtalk-handshake") << true;
        QTest::newRow("user-disconnect") << false;
    }
    void recoveryOrDisconnectDuringProbe()
    {
        QFETCH(bool, recover);
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        auto *flight = FlightTelemetryStats::GetInstance(objects);
        auto flightData = flight->getData();
        flightData.Status = FlightTelemetryStats::STATUS_DISCONNECTED;
        flight->setData(flightData);
        MessageCapture messages;
        UAVTalk protocol(connection.io, objects);
        Telemetry telemetry(&protocol, objects);
        TelemetryMonitor monitor(objects, &telemetry);
        monitor.processStatsUpdates();
        QVERIFY(QMetaObject::invokeMethod(&monitor, "handshakeTimeoutCB", Qt::DirectConnection));
        QVERIFY(connection.io->written.contains("$M<"));
        if (recover) {
            FlightTelemetryStats boardFlight;
            flightData.Status = FlightTelemetryStats::STATUS_HANDSHAKEACK;
            boardFlight.setData(flightData);
            connection.io->receive(objectFrame(&boardFlight));
            QVERIFY(messages.information.join('\n').contains("UAVTalk handshake completed"));
        } else {
            manager->disconnectDevice();
        }
        QTest::qWait(1500);
        QVERIFY(!manager->window()->findChild<QDialog *>("telemetryErrorDialog"));
        QVERIFY(!protocol.firmwareIdentity().active());
    }
    void transmitFailureShowsGuidanceWithoutMspProbe()
    {
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        connection.io->failWrite = true;
        auto *flight = FlightTelemetryStats::GetInstance(objects);
        auto data = flight->getData();
        data.Status = FlightTelemetryStats::STATUS_DISCONNECTED;
        flight->setData(data);
        MessageCapture messages;
        UAVTalk protocol(connection.io, objects);
        Telemetry telemetry(&protocol, objects);
        TelemetryMonitor monitor(objects, &telemetry);
        monitor.processStatsUpdates();
        QVERIFY(QMetaObject::invokeMethod(&monitor, "handshakeTimeoutCB", Qt::DirectConnection));
        auto *dialog = page;
        QVERIFY(dialog);
        QVERIFY(dialog->findChild<QLabel *>("connectionErrorGuidance")->text()
                    .contains("couldn't send connection requests"));
        QVERIFY(!protocol.firmwareIdentity().active());
        QVERIFY(connection.io->isOpen());
    }
    void cleanupTestCase()
    {
        QVERIFY(core.unload());
    }
};

QTEST_MAIN(ConnectionDiagnosticsTests)
#include "connection_diagnostics.moc"
