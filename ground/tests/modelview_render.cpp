/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <modelview/modelviewgadgetwidget.h>
#include <extensionsystem/pluginmanager.h>
#include <uavobjects/uavobjectsplugin.h>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <cmath>
#include <memory>

class ModelViewRenderTests : public QObject
{
    Q_OBJECT
    static int coloredPixels(const QImage &frame)
    {
        int colored = 0;
        for (int y = 0; y < frame.height(); ++y)
            for (int x = 0; x < frame.width(); ++x) {
                const QColor pixel = frame.pixelColor(x, y);
                colored += pixel.red() > 40 || pixel.green() > 40 || pixel.blue() > 40;
            }
        return colored;
    }
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
    static void checkGL(ModelViewGadgetWidget &view)
    {
        view.makeCurrent();
        QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
        view.doneCurrent();
    }

private slots:
    void modelAndContextLifetime_data()
    {
        QTest::addColumn<bool>("useVbo");
        QTest::newRow("client-arrays") << false;
        QTest::newRow("vertex-buffers") << true;
    }

    void modelAndContextLifetime()
    {
        QFETCH(bool, useVbo);
        ExtensionSystem::PluginManager plugins;
        UAVObjectsPlugin objects;
        QString error;
        QVERIFY(objects.initialize({}, &error));
        auto manager = plugins.getObject<UAVObjectManager>();
        QVERIFY(manager);
        auto attitude = AttitudeActual::GetInstance(manager);
        QVERIFY(attitude);
        auto data = attitude->getData();
        data.q1 = 1;
        data.q2 = data.q3 = data.q4 = 0;
        attitude->setData(data);

        QWidget firstHost, secondHost;
        firstHost.resize(660, 500);
        secondHost.resize(660, 500);
        auto view = std::make_unique<ModelViewGadgetWidget>(&firstHost);
        view->resize(640, 480);
        const QString modelFile = qEnvironmentVariable("GCS_MODEL_FILE",
                                                       QString::fromUtf8(MODEL_FILE));
        view->setAcFilename(modelFile);
        view->setVboEnable(useVbo);
        view->reloadScene(); // Before initializeGL: configuration must be retained.
        firstHost.show();
        QVERIFY(QTest::qWaitForWindowExposed(&firstHost));
        QTRY_VERIFY(view->isValid());
        QTest::keyClick(view.get(), Qt::Key_1);
        QTRY_VERIFY(coloredPixels(view->grabFramebuffer()) > 1000);
        const QImage initial = view->grabFramebuffer();
        QCOMPARE(initial.size(), QSize(qRound(640 * view->devicePixelRatioF()),
                                      qRound(480 * view->devicePixelRatioF())));
        qInfo() << "Production ModelView context" << view->context()->format().version()
                << view->context()->format().profile();
        QVERIFY(view->context()->format().profile() != QSurfaceFormat::CoreProfile);
        checkGL(*view);

        data.q1 = std::cos(M_PI / 8);
        data.q4 = std::sin(M_PI / 8);
        attitude->setData(data);
        QTRY_VERIFY(changedPixels(initial, view->grabFramebuffer()) > 1000);
        view->setVboEnable(!useVbo);
        QTest::qWait(100);
        QVERIFY(coloredPixels(view->grabFramebuffer()) > 1000);
        checkGL(*view);

        const QImage beforeCamera = view->grabFramebuffer();
        QTest::keyClick(view.get(), Qt::Key_4);
        QTRY_VERIFY(changedPixels(beforeCamera, view->grabFramebuffer()) > 1000);
        QTest::mousePress(view.get(), Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
        QTest::mouseMove(view.get(), QPoint(350, 250), 30);
        QTest::mouseRelease(view.get(), Qt::LeftButton, Qt::NoModifier, QPoint(350, 250));
        checkGL(*view);

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString background = temporary.filePath("red.png");
        QImage red(32, 32, QImage::Format_RGB32);
        red.fill(Qt::red);
        QVERIFY(red.save(background));
        view->setBgFilename(background);
        view->reloadScene();
        QTest::qWait(100);
        const QImage withBackground = view->grabFramebuffer();
        int redPixels = 0;
        for (int y = 0; y < withBackground.height(); ++y)
            for (int x = 0; x < withBackground.width(); ++x) {
                const QColor pixel = withBackground.pixelColor(x, y);
                redPixels += pixel.red() > 200 && pixel.green() < 20 && pixel.blue() < 20;
            }
        QVERIFY(redPixels > withBackground.width() * withBackground.height() / 10);

        secondHost.show();
        QVERIFY(QTest::qWaitForWindowExposed(&secondHost));
        for (int iteration = 0; iteration < 3; ++iteration) {
            view->setParent(iteration % 2 ? &firstHost : &secondHost);
            view->resize(600 - iteration * 20, 440 - iteration * 20);
            view->show();
            QTRY_VERIFY(view->isValid());
            QTest::qWait(100);
            QVERIFY(coloredPixels(view->grabFramebuffer()) > 1000);
            checkGL(*view);
        }
        // Closing another viewer must not release this viewer's resources.
        auto other = std::make_unique<ModelViewGadgetWidget>(&firstHost);
        other->setAcFilename(modelFile);
        other->resize(400, 300);
        other->show();
        QTRY_VERIFY(other->isValid());
        QVERIFY(coloredPixels(other->grabFramebuffer()) > 1000);
        other.reset();
        QVERIFY(coloredPixels(view->grabFramebuffer()) > 1000);
        checkGL(*view);
        const QString artifacts = qEnvironmentVariable("GCS_MODEL_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(initial.save(artifacts + QString("/model-vbo%1.png").arg(useVbo)));
            QVERIFY(withBackground.save(artifacts + QString("/background-vbo%1.png").arg(useVbo)));
        }
    }
};

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsSet("GCS_SHARE_CONTEXTS"))
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication application(argc, argv);
    ModelViewRenderTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "modelview_render.moc"
