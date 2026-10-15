#include <QTest>
#include <extensionsystem/pluginspec_p.h>

using ExtensionSystem::Internal::PluginSpecPrivate;

class PluginSpecTests : public QObject
{
    Q_OBJECT
private slots:
    void versions()
    {
        for (const QString &version : {"2", "53", "52_1", "3.12", "31.1_12", "31.1.0", "1.0.2_1"})
            QVERIFY2(PluginSpecPrivate::isValidVersion(version), qPrintable(version));
        for (const QString &version : {"", "1..0", "1.0_", "1.0.0.0", " 1", "1\n", "1suffix"})
            QVERIFY2(!PluginSpecPrivate::isValidVersion(version), qPrintable(version));
        QCOMPARE(PluginSpecPrivate::versionCompare("3.0.0_1", "3_1"), 0);
        QCOMPARE(PluginSpecPrivate::versionCompare("3.1_23", "3.1_12"), 1);
        QCOMPARE(PluginSpecPrivate::versionCompare("3.1", "3.1.0_23"), -1);
        // Keep the old invalid-input convention; read() validates first.
        QCOMPARE(PluginSpecPrivate::versionCompare("invalid", "3"), 0);
    }

    void metadataAndDependencyRange()
    {
        PluginSpecPrivate spec(nullptr);
        const QString fixture = QFINDTESTDATA(
            "../gcs/src/libs/extensionsystem/test/auto/pluginspec/testspecs/spec1.xml");
        QVERIFY(spec.read(fixture));
        QCOMPARE(spec.name, QStringLiteral("test"));
        QCOMPARE(spec.version, QStringLiteral("1.0.1"));
        QCOMPARE(spec.compatVersion, QStringLiteral("1.0.0"));
        QCOMPARE(spec.dependencies.size(), 2);
        QVERIFY(spec.provides("TEST", "1.0.0"));
        QVERIFY(spec.provides("test", "1.0.1"));
        QVERIFY(!spec.provides("test", "1.0.2"));
        QVERIFY(!spec.provides("test", "0.9"));
        QVERIFY(!spec.provides("other", "1.0.0"));
    }
};

QTEST_GUILESS_MAIN(PluginSpecTests)
#include "plugin_specs.moc"
