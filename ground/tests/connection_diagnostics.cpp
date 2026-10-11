/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <coreplugin/connectionmanager.h>
#include <coreplugin/icore.h>
#include <coreplugin/modemanager.h>
#include <coreplugin/imode.h>
#include <coreplugin/firmwarecatalog.h>
#include <coreplugin/firmwarepage.h>
#include <coreplugin/firmwareinstallwidget.h>
#include <coreplugin/dfuinstaller.h>
#include <coreplugin/usbaccesssetup.h>
#include <extensionsystem/iplugin.h>
#include <extensionsystem/pluginmanager.h>
#include <serialconnection/serialplugin.h>
#include <rawhid/rawhidplugin.h>
#include <uavobjects/uavobjectsplugin.h>
#include <uavtalk/telemetry.h>
#include <uavtalk/telemetrymonitor.h>
#include <uavtalk/telemetrymanager.h>
#include <uavobjectutil/uavobjectutilmanager.h>
#include <uavtalk/boardsettingsinstaller.h>
#include <uavobjects/uavobjectfield.h>
#include <uavobjects/uavdataobject.h>
#include <objectpersistence.h>
#include <firmwareiapobj.h>
#include <flightstatus.h>
#include <attitudesettings.h>
#include <actuatorsettings.h>
#include <hwrevolution.h>
#include <utils/pathutils.h>
#include <utils/darktheme.h>
#include <attitudeactual.h>
#include <flighttelemetrystats.h>
#include <QPluginLoader>
#include <QAbstractItemView>
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
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QWizard>
#include <cstring>
#include <memory>
#include <functional>
#include <algorithm>

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
        for (const auto &name : {"AttitudeSettings", "HwRevolution", "FlightStatus", "FirmwareIAPObj", "ObjectPersistence"}) {
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
        page->findChild<QToolButton *>("connectionErrorDetailsToggle")->setChecked(false);
        QVERIFY(!manager->window()->findChild<QDialog *>("connectionErrorDialog"));
        QVERIFY(!manager->window()->findChild<QDialog *>("telemetryErrorDialog"));
        QVERIFY(!manager->window()->findChild<QDialog *>("firmwareCatalogDialog"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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
        QCOMPARE(details->toPlainText(), connection.failure.details);
        QVERIFY(messages.warnings.join('\n').contains(connection.failure.details));
        QCOMPARE(connected.count(), 0);
        QVERIFY(!manager->isConnected());
        manager->disconnectDevice();
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
        QVERIFY(!card->isAncestorOf(manual)); QVERIFY(!card->isAncestorOf(toggle));
        QVERIFY(!card->isAncestorOf(details)); QVERIFY(!details->isVisible());
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
        QVERIFY(hint->text().contains("BOOT released")); QVERIFY(start->isVisible()); QVERIFY(start->isEnabled());
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
    void firmwareInstallStillReconnectsAfterWaitingTimeout()
    {
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
        auto *progress = installation.findChild<QProgressBar *>("firmwareInstallProgress");
        QTRY_VERIFY_WITH_TIMEOUT(!progress->isVisible(), 70000);
        QCOMPARE(installer->stage(), Core::DfuInstaller::Stage::AwaitingTelemetry);
        QVERIFY(installation.findChild<QLabel *>("firmwareInstallReconnectHint")->isVisible());
        QVERIFY(installation.findChild<QPushButton *>("firmwareInstallStartController")->isEnabled());
        TestConnection controller("Reconnected DroniNG", "1-3"); controller.transport = "USB";
        emit controller.availableDevChanged(&controller);
        QTRY_VERIFY_WITH_TIMEOUT(manager->isConnected(), 2000);
        QCOMPARE(manager->getCurrentDevice().device, controller.device);
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
        QCOMPARE(details->toPlainText(), connection.failure.details);
        QVERIFY(messages.warnings.join('\n').contains(connection.failure.details));
        QTest::mouseClick(toggle, Qt::LeftButton);
        QVERIFY(!details->isVisible());
        manager->disconnectDevice();
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
        QCOMPARE(manager->firmwareStatus(), QString("Flight data unavailable"));
        auto *guidance = dialog->findChild<QLabel *>("connectionErrorGuidance");
        QVERIFY(guidance);
        QVERIFY(guidance->text().contains(input.isEmpty() ? "sent no data"
            : input.startsWith(char(0x3c)) ? "handshake didn't finish" : "can't read as UAVTalk"));
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
        QCOMPARE(catalog.boards().size(), 18);
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
        QCOMPARE(count, 17);
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
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBoard")->text(), QString("UNKNOWN"));
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
        QVERIFY(!manager->isVisible());
        QCOMPARE(page->findChild<QLabel *>("firmwareUsbTitle")->text(), QString("USB controller detected"));
        QVERIFY(!page->findChild<QLabel *>("firmwareUsbHint")->text().contains("Plug"));
        QVERIFY(!page->findChild<QPushButton *>("firmwareInstallAction")->isVisible());
        QVERIFY(!page->findChild<QWidget *>("firmwareRecoveryCard")->isVisible());
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
        QTest::mouseClick(page->findChild<QPushButton *>("firmwareDisconnectAction"), Qt::LeftButton);
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
        QCOMPARE(manager->firmwareVersion(), QString("3.2.3"));
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
    void nativeBootloaderAndFlightDataState()
    {
        TestConnection connection;
        QVERIFY(manager->connectDevice({&connection, connection.device}));
        manager->setNativeFirmwareIdentity("Revolution", "dRonin 20261010", "dRonin 0x88");
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedBootloader")->text(), QString("dRonin 0x88"));
        QCOMPARE(page->findChild<QLabel *>("firmwareDetectedFirmware")->text(), QString("dRonin 20261010"));
        manager->reportFlightDataReceived();
        QVERIFY(page->findChild<QLabel *>("firmwareStatus")->text().contains("sending flight data"));
        QVERIFY(page->findChild<QPushButton *>("firmwareFlightAction")->isVisible());
        QVERIFY(page->findChild<QPushButton *>("firmwareConfigureAction")->isVisible());
        QVERIFY(page->findChild<QPushButton *>("firmwareDisconnectAction")->isVisible());
        QVERIFY(!page->findChild<QComboBox *>("firmwareBoardModel")->isVisible());
        QVERIFY(!page->findChild<QPushButton *>("firmwareInstallAction")->isVisible());
        QVERIFY(!page->findChild<QWidget *>("firmwareRecoveryCard")->isVisible());
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
        QVERIFY(page->findChild<QPushButton *>("firmwareFlightAction")->isVisible());
        manager->telemetryDisconnected();
        QVERIFY(!manager->flightDataAvailable()); QVERIFY(manager->firmwareDetectionActive());
        QVERIFY(!page->findChild<QPushButton *>("firmwareFlightAction")->isVisible());
        QVERIFY(page->findChild<QProgressBar *>("firmwareDetectionProgress")->isVisible());
        manager->reportFlightDataReceived();
        QVERIFY(manager->flightDataAvailable()); QVERIFY(!manager->firmwareDetectionActive());
        QVERIFY(page->findChild<QPushButton *>("firmwareFlightAction")->isVisible());
    }
    void controllerSetupOfferAndExplicitWizard()
    {
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
        QPluginLoader config(build.filePath("lib/droning/plugins/droniNG/libConfig.so"));
        QPluginLoader setup(build.filePath("lib/droning/plugins/droniNG/libSetupWizard.so"));
        auto *configPlugin = qobject_cast<ExtensionSystem::IPlugin *>(config.instance());
        auto *setupPlugin = qobject_cast<ExtensionSystem::IPlugin *>(setup.instance());
        QVERIFY2(configPlugin, qPrintable(config.errorString()));
        QVERIFY2(setupPlugin, qPrintable(setup.errorString()));
        QString error;
        QVERIFY2(configPlugin->initialize({}, &error), qPrintable(error));
        QVERIFY2(setupPlugin->initialize({}, &error), qPrintable(error));
        setupPlugin->extensionsInitialized();
        const auto unload = qScopeGuard([&] {
            for (auto *wizard : manager->window()->findChildren<QWizard *>("controllerSetupWizard"))
                wizard->reject();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            setup.unload(); config.unload();
        });
        auto metadata = previousIap;
        std::fill(std::begin(metadata.Description), std::end(metadata.Description), 0);
        std::copy_n("TlFw", 4, metadata.Description);
        const auto hash = QByteArray::fromHex(util.getGcsHash().toLatin1());
        QCOMPARE(hash.size(), 4);
        for (int i = 0; i < 4; ++i) metadata.Description[4 + i] = quint8(hash[3 - i]);
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
        QVERIFY(!page->findChild<QWidget *>("firmwareRecoveryCard")->isVisible());
        auto *launch = page->findChild<QPushButton *>("firmwareConfigureAction");
        QCOMPARE(launch->text(), QString("Launch setup wizard"));
        captureFirmwarePage("firmware-controller-needs-setup");
        QTest::mouseClick(launch, Qt::LeftButton);
        QPointer<QWizard> wizard = manager->window()->findChild<QWizard *>("controllerSetupWizard");
        QVERIFY(wizard); QVERIFY(wizard->isVisible()); QCOMPARE(requests.size(), 1);
        QCOMPARE(workspaceChanges.size(), 0);
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
        connection.io->onWrite = [&](const QByteArray &request) {
            if (request.size() < 9 || quint8(request[0]) != 0x3c || quint8(request[1]) != 0x21) return;
            quint32 id = 0;
            for (int byte = 0; byte < 4; ++byte) id |= quint32(quint8(request[4 + byte])) << (8 * byte);
            auto *object = objects->getObject(id);
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
        QVERIFY(page->findChild<QPushButton *>("firmwareFlightAction")->isVisible());
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
