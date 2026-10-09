/* SPDX-License-Identifier: GPL-3.0-or-later
 * Qt 6 implementation of the ModelView gadget. Attitude mapping follows the
 * original GPL implementation by the OpenPilot Team, Copyright (C) 2010.
 */
#include "modelviewgadgetwidget.h"
#include <extensionsystem/pluginmanager.h>
#include <glc_context.h>
#include <glc_state.h>
#include <viewport/glc_userinput.h>
#include <QDebug>
#include <QFile>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <algorithm>

const QString ModelViewGadgetWidget::fallbackAcFilename =
    QStringLiteral(":/modelview/models/warning_sign.obj");
const QString ModelViewGadgetWidget::fallbackBgFilename =
    QStringLiteral(":/modelview/models/black.jpg");

ModelViewGadgetWidget::ModelViewGadgetWidget(QWidget *parent)
    : QOpenGLWidget(parent), acFilename(fallbackAcFilename), bgFilename(fallbackBgFilename),
      vboEnable(false), attState(nullptr)
{
    QSurfaceFormat requested;
    requested.setVersion(3, 3);
    requested.setProfile(QSurfaceFormat::CompatibilityProfile);
    requested.setOption(QSurfaceFormat::DeprecatedFunctions);
    requested.setDepthBufferSize(24);
    requested.setSamples(4);
    setFormat(requested);
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
    setFocusPolicy(Qt::StrongFocus);
    auto manager = ExtensionSystem::PluginManager::instance()->getObject<UAVObjectManager>();
    if (manager)
        attState = AttitudeActual::GetInstance(manager);
    connect(&m_MotionTimer, &QTimer::timeout, this,
            QOverload<>::of(&ModelViewGadgetWidget::updateAttitude));
}

ModelViewGadgetWidget::~ModelViewGadgetWidget()
{
    cleanupGL();
}

void ModelViewGadgetWidget::cleanupGL()
{
    m_MotionTimer.stop();
    if (!m_GlView)
        return;
    makeCurrent();
    m_MoverController.reset();
    m_World.reset();
    m_Light.reset();
    m_GlView.reset();
    disconnect(context(), &QOpenGLContext::aboutToBeDestroyed, this,
               &ModelViewGadgetWidget::cleanupGL);
    doneCurrent();
}

void ModelViewGadgetWidget::setAcFilename(QString filename)
{
    acFilename = QFile::exists(filename) ? filename : fallbackAcFilename;
    if (m_GlView && acFilename == fallbackAcFilename)
        m_GlView->cameraHandle()->setFrontView();
}

void ModelViewGadgetWidget::setBgFilename(QString filename)
{
    bgFilename = QFile::exists(filename) ? filename : fallbackBgFilename;
}

void ModelViewGadgetWidget::setVboEnable(bool enabled)
{
    vboEnable = enabled;
    if (m_World) {
        makeCurrent();
        m_World->collection()->setVboUsage(enabled);
        doneCurrent();
        update();
    }
}

void ModelViewGadgetWidget::reloadScene()
{
    // Configuration can arrive before Qt has created this widget's context.
    if (!m_GlView)
        return;
    makeCurrent();
    CreateScene();
    doneCurrent();
    update();
}

void ModelViewGadgetWidget::initializeGL()
{
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this,
            &ModelViewGadgetWidget::cleanupGL, Qt::DirectConnection);
    GLC_Context::current();
    m_GlView = std::make_unique<GLC_Viewport>();
    connect(m_GlView.get(), &GLC_Viewport::updateOpenGL, this,
            QOverload<>::of(&ModelViewGadgetWidget::update));
    m_GlView->initGl();
    m_Light = std::make_unique<GLC_Light>();
    m_Light->setPosition(4000.0, 40000.0, 80000.0);
    m_Light->setAmbientColor(Qt::lightGray);
    m_GlView->cameraHandle()->setDefaultUpVector(glc::Z_AXIS);
    m_GlView->cameraHandle()->setRearView();
    const QColor moverColor = QColor::fromRgbF(1.0, 0.11372, 0.11372, 0.0);
    m_MoverController = std::make_unique<GLC_MoverController>(
        GLC_Factory::instance()->createDefaultMoverController(moverColor, m_GlView.get()));
    CreateScene();
    glEnable(GL_NORMALIZE);
    glEnable(GL_MULTISAMPLE);
    m_MotionTimer.start(100);
}

void ModelViewGadgetWidget::CreateScene()
{
    try {
        m_GlView->loadBackGroundImage(acFilename == fallbackAcFilename
                                        ? fallbackBgFilename : bgFilename);
        QFile aircraft(acFilename);
        auto world = std::make_unique<GLC_World>(
            GLC_Factory::instance()->createWorldFromFile(aircraft));
        world->collection()->setVboUsage(vboEnable);
        m_ModelBoundingBox = world->boundingBox();
        m_World = std::move(world);
        if (acFilename == fallbackAcFilename)
            m_GlView->cameraHandle()->setFrontView();
        m_GlView->reframe(m_ModelBoundingBox);
        updateAttitude();
    } catch (const GLC_Exception &error) {
        qWarning() << "ModelView scene loading failed:" << error.what();
    }
}

void ModelViewGadgetWidget::resizeGL(int width, int height)
{
    if (m_GlView)
        m_GlView->setWinGLSize(qRound(width * devicePixelRatioF()),
                             qRound(height * devicePixelRatioF()));
}

void ModelViewGadgetWidget::paintGL()
{
    if (!m_GlView || !m_World || m_World->isEmpty())
        return;
    try {
        auto state = GLC_Context::current();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        state->glcMatrixMode(GL_MODELVIEW);
        state->glcLoadIdentity();
        glEnable(GL_MULTISAMPLE);
        m_GlView->setDistMinAndMax(m_World->boundingBox());
        m_GlView->glExecuteCam();
        m_Light->glExecute();
        m_World->render(0, glc::ShadingFlag);
        m_World->render(0, glc::TransparentRenderFlag);
        m_MoverController->drawActiveMoverRep();
    } catch (const GLC_Exception &error) {
        qWarning() << "ModelView rendering failed:" << error.what();
    }
}

void ModelViewGadgetWidget::wheelEvent(QWheelEvent *event)
{
    if (!m_GlView || !m_World)
        return;
    makeCurrent();
    const double distance = m_GlView->cameraHandle()->distEyeTarget()
                            - event->angleDelta().y() / 4.0;
    m_GlView->cameraHandle()->setDistEyeTarget(std::max(0.01, distance));
    m_GlView->setDistMinAndMax(m_World->boundingBox());
    doneCurrent();
    update();
}

void ModelViewGadgetWidget::mousePressEvent(QMouseEvent *event)
{
    if (!m_MoverController || !m_World || m_World->isEmpty()
        || m_MoverController->hasActiveMover())
        return;
    if (event->button() == Qt::LeftButton) {
        makeCurrent();
        GLC_State::setGlobalDevicePixelRatio(devicePixelRatioF());
        const QPoint position = event->position().toPoint();
        m_MotionTimer.stop();
        m_MoverController->setActiveMover(GLC_MoverController::TurnTable,
                                         GLC_UserInput(position.x(), position.y(), event));
        doneCurrent();
        update();
    } else if (event->button() == Qt::RightButton) {
        makeCurrent();
        qInfo() << "ModelView renderer:" << reinterpret_cast<const char *>(glGetString(GL_RENDERER));
        doneCurrent();
    }
}

void ModelViewGadgetWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_MoverController || !m_World || !m_MoverController->hasActiveMover())
        return;
    makeCurrent();
    GLC_State::setGlobalDevicePixelRatio(devicePixelRatioF());
    const QPoint position = event->position().toPoint();
    m_MoverController->move(GLC_UserInput(position.x(), position.y(), event));
    m_GlView->setDistMinAndMax(m_World->boundingBox());
    doneCurrent();
    update();
}

void ModelViewGadgetWidget::mouseReleaseEvent(QMouseEvent *)
{
    if (!m_MoverController || !m_MoverController->hasActiveMover())
        return;
    makeCurrent();
    m_MoverController->setNoMover();
    doneCurrent();
    m_MotionTimer.start(100);
    update();
}

void ModelViewGadgetWidget::keyPressEvent(QKeyEvent *event)
{
    if (!m_GlView)
        return;
    auto camera = m_GlView->cameraHandle();
    switch (event->key()) {
    case Qt::Key_1: camera->setIsoView(); break;
    case Qt::Key_2: camera->setFrontView(); break;
    case Qt::Key_3:
        camera->setIsoView();
        camera->rotateAroundTarget(glc::Z_AXIS, glc::toRadian(90));
        break;
    case Qt::Key_4: camera->setLeftView(); break;
    case Qt::Key_5:
        camera->setTopView();
        camera->rotateAroundTarget(glc::Z_AXIS, glc::toRadian(180));
        break;
    case Qt::Key_6: camera->setRightView(); break;
    case Qt::Key_7:
        camera->setIsoView();
        camera->rotateAroundTarget(glc::Z_AXIS, glc::toRadian(-90));
        break;
    case Qt::Key_8: camera->setRearView(); break;
    case Qt::Key_9:
        camera->setIsoView();
        camera->rotateAroundTarget(glc::Z_AXIS, glc::toRadian(180));
        break;
    case Qt::Key_0:
        camera->setBottomView();
        camera->rotateAroundTarget(glc::Z_AXIS, glc::toRadian(180));
        break;
    default:
        QOpenGLWidget::keyPressEvent(event);
        return;
    }
    update();
}

void ModelViewGadgetWidget::updateAttitude()
{
    if (!attState || !m_World || m_World->isEmpty())
        return;
    const auto data = attState->getData();
    const double x = data.q3, y = data.q2, z = data.q4;
    const double w = data.q1 == 0.0 ? 1.0 : data.q1;
    QMatrix4x4 first;
    first.setRow(0, QVector4D(w, z, -y, x));
    first.setRow(1, QVector4D(-z, w, x, y));
    first.setRow(2, QVector4D(y, -x, w, z));
    first.setRow(3, QVector4D(-x, -y, -z, w));
    QMatrix4x4 second;
    second.setRow(0, QVector4D(w, z, -y, -x));
    second.setRow(1, QVector4D(-z, w, x, -y));
    second.setRow(2, QVector4D(y, -x, w, -z));
    second.setRow(3, QVector4D(x, y, z, w));
    const QMatrix4x4 rotation = first * second;
    auto root = m_World->rootOccurrence();
    root->structInstance()->setMatrix(GLC_Matrix4x4(rotation.constData()));
    root->updateChildrenAbsoluteMatrix();
    update();
}

void ModelViewGadgetWidget::updateAttitude(int)
{
    updateAttitude();
}
