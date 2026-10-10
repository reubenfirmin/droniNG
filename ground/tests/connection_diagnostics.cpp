/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <coreplugin/connectionmanager.h>
#include <coreplugin/icore.h>
#include <extensionsystem/iplugin.h>
#include <extensionsystem/pluginmanager.h>
#include <serialconnection/serialplugin.h>
#include <uavobjects/uavobjectsplugin.h>
#include <uavtalk/telemetry.h>
#include <uavtalk/telemetrymonitor.h>
#include <utils/pathutils.h>
#include <utils/darktheme.h>
#include <attitudeactual.h>
#include <flighttelemetrystats.h>
#include <QPluginLoader>
#include <QClipboard>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <cstring>
#include <memory>

Q_DECLARE_METATYPE(QFile::Permissions)

class MessageCapture
{
    static MessageCapture *active;
    QtMessageHandler previous;
    static void capture(QtMsgType type, const QMessageLogContext &, const QString &message)
    {
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

class TestDevice : public QIODevice
{
    QByteArray incoming;
public:
    bool failOpen = false, failWrite = false;
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
    qint64 writeData(const char *, qint64 size) override
    {
        if (failWrite) {
            setErrorString("simulated write failure");
            return -1;
        }
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
    Core::ConnectionError failure;
    int closeCalls = 0;
    explicit TestConnection(const QString &displayName = "test controller")
    {
        device = new Core::IDevice;
        device->setParent(this);
        device->setName("test-serial-port");
        device->setDisplayName(displayName);
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
    QString connectionName() override { return "Serial"; }
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

class ConnectionDiagnosticsTests : public QObject
{
    Q_OBJECT
    QTemporaryDir settingsDirectory;
    ExtensionSystem::PluginManager plugins;
    UAVObjectsPlugin uavObjects;
    QPluginLoader core{QString::fromUtf8(CORE_PLUGIN_FILE)};
    Core::ConnectionManager *manager = nullptr;
    UAVObjectManager *objects = nullptr;

private slots:
    void initTestCase()
    {
        Utils::applyDarkTheme();
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
    }
    void cleanup()
    {
        manager->disconnectDevice();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void failedClickShowsPersistentDialog()
    {
        TestConnection connection;
        connection.returnNull = true;
        MessageCapture messages;
        QSignalSpy connected(manager, &Core::ConnectionManager::deviceConnected);
        QVERIFY(QMetaObject::invokeMethod(manager, "onConnectClicked", Qt::DirectConnection));
        QCoreApplication::processEvents();
        QPointer<QDialog> dialog = manager->window()->findChild<QDialog *>("connectionErrorDialog");
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        auto *guidance = dialog->findChild<QLabel *>("connectionErrorGuidance");
        QVERIFY(guidance);
        QVERIFY(!guidance->text().contains("simulated serial open failure"));
        auto *details = dialog->findChild<QPlainTextEdit *>("connectionErrorDetails");
        QVERIFY(details);
        QVERIFY(details->toPlainText().contains("simulated serial open failure"));
        QVERIFY(!details->isVisible());
        QVERIFY(!dialog->findChild<QPushButton *>("copyConnectionErrorCommand"));
        QCOMPARE(connected.count(), 0);
        QVERIFY(!manager->isConnected());
        QCOMPARE(messages.warnings.size(), 1);
        QVERIFY(messages.warnings.first().contains("Serial: test controller"));
        dialog->reject();
        QTRY_VERIFY(dialog.isNull());
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
    void serialPermissionGuidanceReachesDialogAndLog()
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
        QPointer<QDialog> dialog = manager->window()->findChild<QDialog *>("connectionErrorDialog");
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
        QCOMPARE(details->toPlainText(), connection.failure.details);
        QVERIFY(messages.warnings.join('\n').contains(connection.failure.details));
        QCOMPARE(connected.count(), 0);
        QVERIFY(!manager->isConnected());
        dialog->reject();
        QTRY_VERIFY(dialog.isNull());
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
    void groupAccessDialogCopiesCommandWithoutClosing()
    {
        TestConnection connection("OmnibusF4 - ttyACM0");
        connection.returnNull = true;
        connection.failure = SerialConnection::groupAccessFailure("dialout", "rfirmin",
            "Could not open /dev/ttyACM0 at 115200 baud (8N1, no flow control): "
            "Permission denied (Qt serial error 2)");
        MessageCapture messages;
        QVERIFY(QMetaObject::invokeMethod(manager, "onConnectClicked", Qt::DirectConnection));
        QCoreApplication::processEvents();
        QPointer<QDialog> dialog = manager->window()->findChild<QDialog *>("connectionErrorDialog");
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
        QCOMPARE(details->toPlainText(), connection.failure.details);
        QVERIFY(messages.warnings.join('\n').contains(connection.failure.details));
        QTest::mouseClick(toggle, Qt::LeftButton);
        QVERIFY(!details->isVisible());
        dialog->reject();
        QTRY_VERIFY(dialog.isNull());
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
        QTest::qWait(700);
        QCOMPARE(messages.warnings.size(), 1);

        FlightTelemetryStats boardFlight;
        flightData.Status = FlightTelemetryStats::STATUS_HANDSHAKEACK;
        boardFlight.setData(flightData);
        connection.io->receive(objectFrame(&boardFlight));
        QVERIFY(messages.information.join('\n').contains("UAVTalk handshake completed"));
        QCOMPARE(messages.warnings.size(), 1);
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
    void cleanupTestCase()
    {
        QVERIFY(core.unload());
    }
};

QTEST_MAIN(ConnectionDiagnosticsTests)
#include "connection_diagnostics.moc"
