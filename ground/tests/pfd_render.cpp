/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <pfdqml/pfdqmlgadgetwidget.h>
#include <qmlview/qmlviewgadgetwidget.h>
#include <extensionsystem/pluginmanager.h>
#include <uavobjects/uavobjectmanager.h>
#include <uavobjects/uavobjectsplugin.h>
#include <attitudeactual.h>
#include <QImage>
#include <QDir>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QTest>
#include <memory>

// The fixture exposes the actual generated objects through the normal pool.
struct ObjectPoolFixture
{
    ExtensionSystem::PluginManager plugins;
    UAVObjectsPlugin uavObjects;
    UAVObjectManager *objects = nullptr;
    ObjectPoolFixture()
    {
        QString error;
        uavObjects.initialize({}, &error);
        objects = plugins.getObject<UAVObjectManager>();
    }
};

class PfdRenderTests : public QObject
{
    Q_OBJECT
    static int changedPixels(const QImage &first, const QImage &second)
    {
        if (first.size() != second.size())
            return 0;
        int changed = 0;
        for (int y = 0; y < first.height(); ++y)
            for (int x = 0; x < first.width(); ++x)
                changed += first.pixel(x, y) != second.pixel(x, y);
        return changed;
    }

private slots:
    void displayAndAttitude_data()
    {
        QTest::addColumn<bool>("generalViewer");
        QTest::newRow("PFD") << false;
        QTest::newRow("QML-viewer") << true;
    }

    void displayAndAttitude()
    {
        QFETCH(bool, generalViewer);
        ObjectPoolFixture pool;
        QVERIFY(pool.objects);
        std::unique_ptr<QQuickView> holder;
        if (generalViewer)
            holder = std::make_unique<QmlViewGadgetWidget>();
        else
            holder = std::make_unique<PfdQmlGadgetWidget>();
        QQuickView &view = *holder;
        const auto reload = [&] {
            const QString file = qEnvironmentVariable("GCS_PFD_QML_FILE",
                                                      QString::fromUtf8(PFD_QML_FILE));
            if (generalViewer)
                static_cast<QmlViewGadgetWidget &>(view).setQmlFile(file);
            else
                static_cast<PfdQmlGadgetWidget &>(view).setQmlFile(file);
        };
        QStringList warnings;
        connect(view.engine(), &QQmlEngine::warnings, this,
                [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings.append(error.toString());
        });
        if (!generalViewer)
            static_cast<PfdQmlGadgetWidget &>(view).setSettingsMap({});
        view.resize(640, 480);
        reload();
        QTRY_COMPARE(view.status(), QQuickView::Ready);
        QVERIFY(view.rootObject());
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTest::qWait(100);
        const QImage level = view.grabWindow();
        QVERIFY(!level.isNull());
        int sky = 0, earth = 0;
        for (int y = 0; y < level.height(); ++y) {
            for (int x = 0; x < level.width(); ++x) {
                const QColor color = level.pixelColor(x, y);
                sky += color.blue() > 150 && color.green() > 100 && color.red() < 100;
                earth += color.red() > 100 && color.red() < 150 && color.green() < 100
                         && color.blue() < 100;
            }
        }
        QVERIFY(sky > 1000);
        QVERIFY(earth > 1000);
        auto attitude = AttitudeActual::GetInstance(pool.objects);
        QVERIFY(attitude);
        auto data = attitude->getData();
        data.Roll = 25;
        data.Pitch = 15;
        data.Yaw = 35;
        attitude->setData(data);
        QTRY_VERIFY(changedPixels(level, view.grabWindow()) > level.width() * level.height() / 20);

        view.resize(800, 600);
        QTRY_VERIFY(view.grabWindow().width() >= 800);
        reload();
        QTRY_COMPARE(view.status(), QQuickView::Ready);
        QTest::qWait(100);
        QVERIFY(!view.grabWindow().isNull());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        const QString artifacts = qEnvironmentVariable("GCS_PFD_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            const QString prefix = generalViewer ? "/qml-" : "/pfd-";
            QVERIFY(level.save(artifacts + prefix + "level.png"));
            QVERIFY(view.grabWindow().save(artifacts + prefix + "attitude.png"));
        }
    }
};

QTEST_MAIN(PfdRenderTests)
#include "pfd_render.moc"
