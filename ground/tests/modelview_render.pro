TEMPLATE = app
TARGET = modelview-render-tests
CONFIG += console testcase c++17
QT += widgets testlib
!equals(QT_MAJOR_VERSION, 6):error("This fixture checks the Qt 6 production widget")
isEmpty(GCS_BUILD_TREE):error("Set GCS_BUILD_TREE to the selected build's ground/gcs directory")
isEmpty(GLC_LIB_DIRECTORY):error("Set GLC_LIB_DIRECTORY to the Qt 6 GLC wrapper's library directory")
include(../gcs/gcs.pri)
include(../gcs/src/plugins/coreplugin/coreplugin.pri)
include(../gcs/src/plugins/uavobjects/uavobjects.pri)
include(../gcs/src/libs/glc_lib/glc_lib.pri)
include(../gcs/src/libs/extensionsystem/extensionsystem.pri)
LIBS = -L$$GLC_LIB_DIRECTORY $$LIBS -L$$GCS_PLUGIN_PATH/droniNG
QMAKE_RPATHDIR += $$GCS_LIBRARY_PATH $$GCS_PLUGIN_PATH/droniNG $$GLC_LIB_DIRECTORY
INCLUDEPATH += ../gcs/src/plugins
DEFINES += MODEL_FILE=\\\"$$PWD/../gcs/share/models/boards/CC3D/CC3D.3ds\\\"
SOURCES += modelview_render.cpp ../gcs/src/plugins/modelview/modelviewgadgetwidgetqt6.cpp
HEADERS += ../gcs/src/plugins/modelview/modelviewgadgetwidget.h
RESOURCES += ../gcs/src/plugins/modelview/modelview.qrc
