/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <utils/xmlconfig.h>
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QDateTime>
#include <QDockWidget>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QProcess>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

namespace {
bool readMap(const QString &path, QSettings::SettingsMap &map)
{
    QSettings settings(path, XmlConfig::XmlSettingsFormat);
    for (const QString &key : settings.allKeys())
        map.insert(key, settings.value(key));
    return settings.status() == QSettings::NoError;
}

bool writeMap(const QString &path, const QSettings::SettingsMap &map)
{
    QSettings settings(path, XmlConfig::XmlSettingsFormat);
    settings.clear();
    for (auto entry = map.cbegin(); entry != map.cend(); ++entry)
        settings.setValue(entry.key(), entry.value());
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QJsonObject canonicalValue(const QVariant &value, bool &valid)
{
    QJsonObject output;
    output.insert("type", QString::fromLatin1(value.typeName() ? value.typeName() : "invalid"));
    // Qt 5 writes QMap entries in reverse order and Qt 6 writes them forward.
    // Compare decoded members, while retaining type and byte checks for leaves.
    if (value.userType() == QMetaType::QVariantMap) {
        QJsonObject members;
        const auto map = value.toMap();
        for (auto entry = map.cbegin(); entry != map.cend(); ++entry)
            members.insert(entry.key(), canonicalValue(entry.value(), valid));
        output.insert("members", members);
    } else if (value.userType() == QMetaType::QVariantList) {
        QJsonArray members;
        for (const QVariant &entry : value.toList())
            members.append(canonicalValue(entry, valid));
        output.insert("members", members);
    } else {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_4_0);
        stream << value;
        valid = valid && stream.status() == QDataStream::Ok;
        output.insert("data", QString::fromLatin1(bytes.toBase64()));
    }
    return output;
}

bool normalize(const QString &source, const QString &destination)
{
    QSettings::SettingsMap map;
    if (!readMap(source, map))
        return false;
    QJsonObject output;
    bool valid = true;
    for (auto entry = map.cbegin(); entry != map.cend(); ++entry)
        output.insert(entry.key(), canonicalValue(entry.value(), valid));
    if (!valid)
        return false;
    QFile file(destination);
    const QByteArray bytes = QJsonDocument(output).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QSettings::SettingsMap modelValues()
{
    QSettings::SettingsMap values;
    values.insert("General/observations", QStringLiteral(""));
    values.insert("General/vehicle", QString::fromUtf8("café λ & <quad>"));
    values.insert("General/AutoConnect", false);
    values.insert("Serial/speed", 115200);
    values.insert("Numeric/unsigned", uint(0xffffffff));
    values.insert("Numeric/signed64", qlonglong(-123456789012345));
    values.insert("Numeric/unsigned64", qulonglong(123456789012345));
    values.insert("Numeric/scale", 1.25);
    values.insert("Literal/at", QStringLiteral("@Rect(1 2 3 4)"));
    values.insert("Literal/double-at", QStringLiteral("@@literal"));
    values.insert("Binary/data", QByteArray("\0\x01\xff\0", 4));
    values.insert("Binary/empty", QByteArray());
    values.insert("Geometry/rect", QRect(-10, 20, 640, 480));
    values.insert("Geometry/point", QPoint(-4, 8));
    values.insert("Geometry/size", QSize(640, 480));
    values.insert("Layout/0/splitterSizes", QVariantList({160, 480}));
    values.insert("Layout/1/café λ #50%", QStringLiteral("PFD"));
    values.insert("List/modes", QStringList({"Flight", QString::fromUtf8("配置 café")}));
    values.insert("List/empty", QStringList());
    values.insert("Map/nested", QVariantMap({{"enabled", true}, {"names", QStringList({"A", "B"})}}));
    values.insert("Date/day", QDate(2026, 10, 9));
    values.insert("Date/time", QDateTime(QDate(2026, 10, 9), QTime(1, 2, 3), Qt::UTC));
    values.insert("Url/source", QUrl("file:///tmp/UI%20Plan?mode=flight"));
    values.insert("Display/font", QFont("DejaVu Sans", 11, QFont::Bold, true));
    return values;
}

void populateWindow(QMainWindow &window, Qt::DockWidgetArea initialArea)
{
    window.setCentralWidget(new QLabel("Flight view"));
    for (const QString &name : {QStringLiteral("ObjectBrowser"), QStringLiteral("Telemetry")}) {
        auto *dock = new QDockWidget(name, &window);
        dock->setObjectName(name);
        dock->setWidget(new QLabel(name));
        window.addDockWidget(initialArea, dock);
    }
    window.resize(640, 400);
}

bool writeModel(const QString &path)
{
    auto values = modelValues();
    QMainWindow window;
    populateWindow(window, Qt::LeftDockWidgetArea);
    window.addDockWidget(Qt::RightDockWidgetArea, window.findChild<QDockWidget *>("Telemetry"));
    window.move(30, 50);
    window.show();
    QSplitter splitter(Qt::Horizontal);
    splitter.addWidget(new QLabel("PFD"));
    splitter.addWidget(new QLabel("Scope"));
    splitter.resize(640, 100);
    splitter.show();
    QCoreApplication::processEvents();
    splitter.setSizes({160, 480});
    values.insert("Widget/geometry", window.saveGeometry());
    values.insert("Widget/docks", window.saveState(17));
    values.insert("Widget/splitter", splitter.saveState());
    QVariantList sizes;
    for (int size : splitter.sizes())
        sizes.append(size);
    values.insert("Widget/sizes", sizes);
    if (!writeMap(path, values))
        return false;
    QSettings settings(path, XmlConfig::XmlSettingsFormat);
    settings.beginWriteArray("KeyBindings", 2);
    for (int index = 0; index < 2; ++index) {
        settings.setArrayIndex(index);
        settings.setValue("ID", QString("Mode%1").arg(index + 1));
        settings.setValue("Key", QString("Ctrl+%1").arg(index + 1));
    }
    settings.endArray();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool verifyModel(const QString &path)
{
    QSettings::SettingsMap values;
    if (!readMap(path, values))
        return false;
    const auto expected = modelValues();
    for (auto entry = expected.cbegin(); entry != expected.cend(); ++entry) {
        if (!values.contains(entry.key())) {
            qWarning().noquote() << "Missing model key:" << entry.key();
            return false;
        }
        QVariant actual = values.value(entry.key());
        const QVariant &wanted = entry.value();
        // Numeric settings are deliberately stored as strings in the legacy XML format.
        bool converted = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (actual.metaType() != wanted.metaType())
            converted = actual.convert(wanted.metaType());
#else
        if (actual.userType() != wanted.userType())
            converted = actual.convert(wanted.type());
#endif
        if (!converted || actual != wanted) {
            qWarning().noquote() << "Model value changed for key:" << entry.key();
            return false;
        }
    }
    QSettings settings(path, XmlConfig::XmlSettingsFormat);
    if (settings.beginReadArray("KeyBindings") != 2)
        return false;
    for (int index = 0; index < 2; ++index) {
        settings.setArrayIndex(index);
        if (settings.value("ID").toString() != QString("Mode%1").arg(index + 1)
            || settings.value("Key").toString() != QString("Ctrl+%1").arg(index + 1))
            return false;
    }
    settings.endArray();
    QMainWindow window;
    populateWindow(window, Qt::BottomDockWidgetArea);
    if (!window.restoreGeometry(values.value("Widget/geometry").toByteArray())
        || !window.restoreState(values.value("Widget/docks").toByteArray(), 17)
        || window.size() != QSize(640, 400))
        return false;
    if (window.dockWidgetArea(window.findChild<QDockWidget *>("ObjectBrowser")) != Qt::LeftDockWidgetArea
        || window.dockWidgetArea(window.findChild<QDockWidget *>("Telemetry")) != Qt::RightDockWidgetArea)
        return false;
    QSplitter splitter(Qt::Vertical);
    splitter.addWidget(new QLabel("PFD"));
    splitter.addWidget(new QLabel("Scope"));
    splitter.resize(640, 100);
    splitter.show();
    QCoreApplication::processEvents();
    if (!splitter.restoreState(values.value("Widget/splitter").toByteArray())
        || splitter.orientation() != Qt::Horizontal)
        return false;
    const auto sizes = splitter.sizes();
    const auto savedSizes = values.value("Widget/sizes").toList();
    return sizes.size() == 2 && savedSizes.size() == 2
           && qAbs(sizes[0] - savedSizes[0].toInt()) <= 1
           && qAbs(sizes[1] - savedSizes[1].toInt()) <= 1;
}

class FailingWriter : public QIODevice
{
public:
    qint64 accepted = -1;
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *, qint64 length) override { return qMin(length, accepted); }
};
}

class SettingsPersistenceTests : public QObject
{
    Q_OBJECT
private slots:
    void emptyStringsSurviveSerialization()
    {
        QSettings::SettingsMap expected;
        expected.insert("General/observations", QStringLiteral(""));
        expected.insert("Literal/at", QStringLiteral("@literal"));
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        QVERIFY(XmlConfig::writeXmlFile(buffer, expected));
        buffer.seek(0);
        QSettings::SettingsMap actual;
        QVERIFY(XmlConfig::readXmlFile(buffer, actual));
        QVERIFY(actual.contains("General/observations"));
        QCOMPARE(actual, expected);
    }

    void malformedSettingsReportFormatError()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("broken.xml");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("<gcs><General></gcs>") > 0);
        file.close();
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--probe-corrupt", path});
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 2);
        QVERIFY(child.readAllStandardError().contains("Parse error"));
    }

    void failedWrites_data()
    {
        QTest::addColumn<qint64>("accepted");
        QTest::newRow("rejected") << qint64(-1);
        QTest::newRow("partial") << qint64(4);
    }

    void failedWrites()
    {
        QFETCH(qint64, accepted);
        FailingWriter output;
        output.accepted = accepted;
        QVERIFY(output.open(QIODevice::WriteOnly));
        QSettings::SettingsMap values;
        values.insert("General/vehicle", QStringLiteral("quad"));
        QVERIFY(!XmlConfig::writeXmlFile(output, values));
    }
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    application.setStyle("Fusion");
    const QStringList args = application.arguments();
    if (args.size() >= 3) {
        const QString command = args[1];
        if (command == "--probe-corrupt") {
            QSettings settings(args[2], XmlConfig::XmlSettingsFormat);
            settings.allKeys();
            return settings.status() == QSettings::FormatError ? 2 : 1;
        }
        if (command == "--write-model")
            return writeModel(args[2]) ? 0 : 1;
        if (command == "--verify-model")
            return verifyModel(args[2]) ? 0 : 1;
        if (args.size() == 4 && command == "--normalize")
            return normalize(args[2], args[3]) ? 0 : 1;
        if (args.size() == 4 && command == "--rewrite") {
            QSettings::SettingsMap map;
            return readMap(args[2], map) && writeMap(args[3], map) ? 0 : 1;
        }
        return 1;
    }
    SettingsPersistenceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "settings_persistence.moc"
