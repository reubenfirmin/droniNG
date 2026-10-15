#include <QFile>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include <QTest>
#include <memory>
#include <glc_context.h>
#include <glc_factory.h>
#include <glc_state.h>
#include <shading/glc_light.h>
#include <viewport/glc_viewport.h>
#ifdef GCS_ARCHIVE_ISOLATION_TEST
#include <QTemporaryDir>
#include <io/glc_worldto3dxml.h>
#include <quazip.h>
#include <quazipfile.h>
#endif

// A bounded rendering probe; this is not the production ModelView widget.
class ModelProbe : public QOpenGLWidget
{
public:
    explicit ModelProbe(QString filename, QWidget *parent)
        : QOpenGLWidget(parent), modelFile(std::move(filename))
    {
        QSurfaceFormat requested;
        requested.setVersion(3, 3);
        requested.setProfile(QSurfaceFormat::CompatibilityProfile);
        requested.setOption(QSurfaceFormat::DeprecatedFunctions);
        requested.setDepthBufferSize(24);
        setFormat(requested);
    }

    ~ModelProbe() override { cleanup(); }

    int frames = 0;
    int initializations = 0;
    int cleanups = 0;
    int texturedMaterials = 0;
    GLenum error = GL_NO_ERROR;
    double yaw = 0;
    bool useVbo = false;

    void moveCamera()
    {
        makeCurrent();
        viewport->cameraHandle()->rotateAroundTarget(glc::Z_AXIS, 0.3);
        doneCurrent();
        update();
    }

    bool hasRenderedTexture() const
    {
        for (auto *material : world->listOfMaterials())
            if (material->hasTexture() && material->textureIsLoaded())
                return true;
        return false;
    }

    bool pickVisibleModel()
    {
        makeCurrent();
        GLC_State::setSelectionMode(true);
        paintGL();
        // QOpenGLWidget renders into its own FBO, not the default GL_BACK buffer.
        const int w = qRound(width() * devicePixelRatioF());
        const int h = qRound(height() * devicePixelRatioF());
        QByteArray pixels(w * h * 4, '\0');
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        bool found = false;
        for (int offset = 0; offset < pixels.size(); offset += 4) {
            const auto id = glc::decodeRgbId(reinterpret_cast<const GLubyte *>(pixels.constData() + offset));
            if (id && world->collection()->contains(id)) {
                found = true;
                break;
            }
        }
        GLC_State::setSelectionMode(false);
        doneCurrent();
        update();
        return found;
    }

protected:
    void initializeGL() override
    {
        ++initializations;
        connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &ModelProbe::cleanup,
                Qt::DirectConnection);
        qInfo() << "Created context" << context()->format().version()
                << context()->format().profile() << context()->format().options();
        GLC_Context::current();
        viewport = std::make_unique<GLC_Viewport>();
        viewport->initGl();
        QFile file(modelFile);
        world = std::make_unique<GLC_World>(GLC_Factory::instance()->createWorldFromFile(file));
        world->collection()->setVboUsage(useVbo);
        light = std::make_unique<GLC_Light>();
        light->setPosition(4000, 40000, 80000);
        light->setAmbientColor(Qt::lightGray);
        viewport->cameraHandle()->setDefaultUpVector(glc::Z_AXIS);
        viewport->cameraHandle()->setIsoView();
        viewport->reframe(world->boundingBox());
        texturedMaterials = 0;
        for (auto *material : world->listOfMaterials())
            texturedMaterials += material->hasTexture();
        qInfo() << "OpenGL" << context()->format().version() << context()->format().profile()
                << reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    }

    void resizeGL(int w, int h) override
    {
        viewport->setWinGLSize(qRound(w * devicePixelRatioF()), qRound(h * devicePixelRatioF()));
    }

    void paintGL() override
    {
        if (!world)
            return;
        auto *state = GLC_Context::current();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_NORMALIZE);
        viewport->setDistMinAndMax(world->boundingBox());
        state->glcMatrixMode(GL_MODELVIEW);
        state->glcLoadIdentity();
        viewport->glExecuteCam();
        state->glcMultMatrix(GLC_Matrix4x4(glc::Z_AXIS, yaw));
        light->glExecute();
        world->render(0, glc::ShadingFlag);
        world->render(0, glc::TransparentRenderFlag);
        error = glGetError();
        ++frames;
    }

private:
    void cleanup()
    {
        if (!world)
            return;
        makeCurrent();
        world.reset();
        light.reset();
        viewport.reset();
        ++cleanups;
        disconnect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &ModelProbe::cleanup);
        doneCurrent();
    }

    QString modelFile;
    std::unique_ptr<GLC_World> world;
    std::unique_ptr<GLC_Viewport> viewport;
    std::unique_ptr<GLC_Light> light;
};

class GraphicsPrototypeTests : public QObject
{
    Q_OBJECT
private slots:
#ifdef GCS_ARCHIVE_ISOLATION_TEST
    void archiveIsolation()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString archive = temporary.filePath("model.3dxml");
        QFile model(QFINDTESTDATA("../gcs/share/models/boards/CC3D/CC3D.3ds"));
        GLC_World world = GLC_Factory::instance()->createWorldFromFile(model);
        QVERIFY(!world.isEmpty());
        // GLC writes through its private QuaZIP. Read that archive using the
        // public GCS QuaZIP in the same process, then load it back through GLC.
        {
            GLC_WorldTo3dxml exporter(world, false);
            QVERIFY(exporter.exportTo3dxml(archive, GLC_WorldTo3dxml::Compressed3dxml));
        }
        QuaZip zip(archive);
        QVERIFY(zip.open(QuaZip::mdUnzip));
        QVERIFY(zip.getFileNameList().contains(QStringLiteral("Manifest.xml")));
        QVERIFY(zip.setCurrentFile(QStringLiteral("Manifest.xml")));
        QuaZipFile manifest(&zip);
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QVERIFY(manifest.readAll().contains("Root"));
        manifest.close();
        QCOMPARE(manifest.getZipError(), UNZ_OK);
        zip.close();
        QFile exported(archive);
        GLC_World restored = GLC_Factory::instance()->createWorldFromFile(exported);
        QVERIFY(!restored.isEmpty());
        QCOMPARE(restored.collection()->size(), world.collection()->size());
    }
#endif

    void shippedModel_data()
    {
        QTest::addColumn<bool>("useVbo");
        QTest::newRow("client-arrays") << false;
        QTest::newRow("vertex-buffers") << true;
    }

    void shippedModel()
    {
        QFETCH(bool, useVbo);
        const QString filename = QFINDTESTDATA("../gcs/share/models/boards/CC3D/CC3D.3ds");
        QVERIFY(!filename.isEmpty());
        QWidget firstWindow, secondWindow;
        firstWindow.resize(480, 360);
        secondWindow.resize(500, 400);
        auto *view = new ModelProbe(filename, &firstWindow);
        view->useVbo = useVbo;
        view->resize(firstWindow.size());
        firstWindow.show();
        QVERIFY(QTest::qWaitForWindowExposed(&firstWindow));
        QTRY_VERIFY(view->frames > 0);
        QVERIFY(view->context()->isValid());
        QVERIFY(view->context()->format().profile() != QSurfaceFormat::CoreProfile);
        QVERIFY(view->texturedMaterials > 0);
        QVERIFY(view->hasRenderedTexture());
        QCOMPARE(view->error, GLenum(GL_NO_ERROR));
        const QImage original = view->grabFramebuffer();
        QVERIFY(!original.isNull());
        const QString artifactDirectory = qEnvironmentVariable("GCS_GRAPHICS_ARTIFACT_DIR");
        QVERIFY(!artifactDirectory.isEmpty());
        QVERIFY(original.save(artifactDirectory + (useVbo ? "/model-vbo.png" : "/model-arrays.png")));
        view->yaw = 0.6;
        view->update();
        QTRY_VERIFY(view->grabFramebuffer() != original);
        view->moveCamera();
        QVERIFY(view->pickVisibleModel());
        QCOMPARE(view->error, GLenum(GL_NO_ERROR));

        for (int i = 0; i < 3; ++i) {
            view->setParent(i % 2 == 0 ? &secondWindow : &firstWindow);
            view->resize(400 + i * 20, 300 + i * 10);
            view->show();
            secondWindow.show();
            const int before = view->frames;
            view->update();
            QTRY_VERIFY(view->frames > before);
            QVERIFY(view->hasRenderedTexture());
            QVERIFY(!view->grabFramebuffer().isNull());
            QCOMPARE(view->error, GLenum(GL_NO_ERROR));
        }
        if (QCoreApplication::testAttribute(Qt::AA_ShareOpenGLContexts))
            QCOMPARE(view->initializations, 1);
        else
            QVERIFY(view->initializations > 1);
        QCOMPARE(view->cleanups, view->initializations - 1);
        delete view;
    }

    void simultaneousViews()
    {
        const QString filename = QFINDTESTDATA("../gcs/share/models/boards/CC3D/CC3D.3ds");
        QVERIFY(!filename.isEmpty());
        QWidget firstWindow, secondWindow;
        firstWindow.resize(480, 360);
        secondWindow.resize(480, 360);
        auto *first = new ModelProbe(filename, &firstWindow);
        auto *second = new ModelProbe(filename, &secondWindow);
        first->useVbo = second->useVbo = true;
        first->resize(firstWindow.size());
        second->resize(secondWindow.size());
        firstWindow.show();
        secondWindow.show();
        QTRY_VERIFY(first->frames > 0 && second->frames > 0);
        QVERIFY(first->hasRenderedTexture());
        QVERIFY(second->hasRenderedTexture());
        QCOMPARE(QOpenGLContext::areSharing(first->context(), second->context()),
                 QCoreApplication::testAttribute(Qt::AA_ShareOpenGLContexts));
        delete first;
        firstWindow.close();
        const int before = second->frames;
        second->yaw = 0.8;
        second->update();
        QTRY_VERIFY(second->frames > before);
        QVERIFY(second->hasRenderedTexture());
        QVERIFY(second->pickVisibleModel());
        QCOMPARE(second->error, GLenum(GL_NO_ERROR));
        delete second;
    }
};

int main(int argc, char **argv)
{
    if (qEnvironmentVariable("GCS_GL_SHARE_CONTEXTS") == QStringLiteral("1"))
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication application(argc, argv);
    GraphicsPrototypeTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "glc_prototype.moc"
