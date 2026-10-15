#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QDomDocument>
#include <QBuffer>
#include <utils/settingsxml.h>
#include <utils/svgimageprovider.h>
#include <utils/xmlconfig.h>
#include <aggregation/aggregate.h>

class ComponentA : public QObject { Q_OBJECT };
class ComponentB : public QObject { Q_OBJECT };

class QtCompatibilityTests : public QObject
{
    Q_OBJECT
private slots:
    void desktopSettingsRoundTrip()
    {
        QSettings::SettingsMap original;
        original.insert(QString::fromUtf8("Layout/0/café λ"), QString::fromUtf8("@Mode & <λ>"));
        original.insert("Layout/1/Binary", QByteArray("\0\x01\xff", 3));
        original.insert("Layout/1/Position", QPoint(-4, 8));
        original.insert("Layout/1/Size", QSize(640, 480));
        original.insert("Layout/1/Rectangle", QRect(1, 2, 3, 4));
        original.insert("Layout/1/List", QStringList({"first", QString::fromUtf8("café λ")}));
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        QVERIFY(XmlConfig::writeXmlFile(buffer, original));
        QVERIFY(buffer.data().contains("<arr_0>"));
        buffer.seek(0);
        QSettings::SettingsMap restored;
        QVERIFY(XmlConfig::readXmlFile(buffer, restored));
        QCOMPARE(restored, original);
        // Existing desktop layouts use the same registered XML settings format.
        QSettings developer(QFINDTESTDATA("../gcs/share/default_configurations/developer.xml"),
                            XmlConfig::XmlSettingsFormat);
        QVERIFY(!developer.allKeys().isEmpty());
        QCOMPARE(developer.status(), QSettings::NoError);
    }

    void aggregateQueriesAndTransfer()
    {
        Aggregation::Aggregate first;
        Aggregation::Aggregate second;
        auto *a = new ComponentA;
        auto *b = new ComponentB;
        auto *otherB = new ComponentB;
        first.add(a);
        first.add(b);
        first.add(otherB);
        QCOMPARE(Aggregation::query<ComponentB>(static_cast<QObject *>(a)), b);
        QCOMPARE(Aggregation::query_all<ComponentB>(static_cast<QObject *>(a)),
                 QList<ComponentB *>({b, otherB}));
        // Transfer calls remove() while add() holds the same write lock.
        second.add(b);
        QCOMPARE(Aggregation::Aggregate::parentAggregate(b), &second);
        QCOMPARE(first.component<ComponentB>(), otherB);
        QCOMPARE(second.component<ComponentB>(), b);
        first.remove(otherB);
        QVERIFY(first.components<ComponentB>().isEmpty());
        delete otherB;
    }

    void settingsXmlOrderingAndValues()
    {
        QDomDocument document;
        QVERIFY(document.setContent(QString::fromUtf8(
            "<uavobjects><version><hardware revision=\"2\" type=\"a\"/></version>"
            "<data><object name=\"Z\"/><object name=\"A\"/></data>"
            "<settings><object name=\"Zulu\" id=\"0x2\"><field name=\"First\" values=\"1,2\"/>"
            "<field name=\"Second\" values=\"3\"/></object>"
            "<object name=\"Alpha\" id=\"0x1\"><description>café λ &amp; &lt; &gt;</description>"
            "<field values=\"café &amp; λ &quot; &lt;\" name=\"Mode\"/></object>"
            "</settings></uavobjects>")));
        const QString output = Utils::serializeUavSettings(document);
        QVERIFY(output.indexOf("id=\"0x1\" name=\"Alpha\"") < output.indexOf("id=\"0x2\" name=\"Zulu\""));
        QVERIFY(output.indexOf("name=\"Z\"") < output.indexOf("name=\"A\""));
        QVERIFY(output.indexOf("name=\"First\"") < output.indexOf("name=\"Second\""));

        QDomDocument imported;
        QVERIFY(imported.setContent(output));
        const auto alpha = imported.documentElement().firstChildElement("settings").firstChildElement("object");
        QCOMPARE(alpha.attribute("name"), QStringLiteral("Alpha"));
        QCOMPARE(alpha.firstChildElement("description").text(), QString::fromUtf8("café λ & < >"));
        QCOMPARE(alpha.firstChildElement("field").attribute("values"), QString::fromUtf8("café & λ \" <"));
        QCOMPARE(Utils::serializeUavSettings(imported), output);
    }

    void shippedSvgElements_data()
    {
        QTest::addColumn<QString>("element");
        QTest::newRow("vertical-speed-background") << QStringLiteral("vsi-bg");
        QTest::newRow("altitude-background") << QStringLiteral("altitude-bg");
    }

    void shippedSvgElements()
    {
        QFETCH(QString, element);
        const QString file = QFINDTESTDATA("../gcs/share/pfd/default/pfd.svg");
        QVERIFY(!file.isEmpty());
        SvgImageProvider provider(QFileInfo(file).absolutePath() + '/');
        QSize size;
        const QImage image = provider.requestImage(file + '!' + element, &size, QSize(1, 1));
        QVERIFY(!image.isNull());
        QVERIFY(size.width() > 0 && size.height() > 0);
    }

    void transformedBoundsAndSlices()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString file = directory.filePath("fixture.svg");
        QFile svg(file);
        QVERIFY(svg.open(QIODevice::WriteOnly));
        svg.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\">"
                  "<g transform=\"translate(10,20)\"><rect id=\"box\" x=\"20\" y=\"10\" "
                  "width=\"60\" height=\"20\" fill=\"red\"/></g></svg>");
        svg.close();
        SvgImageProvider provider(directory.path() + '/');
        QCOMPARE(provider.scaledElementBounds(file, "box"), QRectF(0.15, 0.3, 0.3, 0.2));

        QSize size;
        const QImage image = provider.requestImage(file + "!box?hslice=1:3;border=1", &size,
                                                   QSize(60, 20));
        QCOMPARE(size, QSize(20, 20));
        QCOMPARE(image.size(), QSize(22, 22));
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(image.pixelColor(10, 10), QColor(Qt::red));
    }
};

QTEST_MAIN(QtCompatibilityTests)
#include "qt_compat.moc"
