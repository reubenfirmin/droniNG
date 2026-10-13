TEMPLATE = app
TARGET = pfd-render-tests
CONFIG += console testcase c++17
QT += widgets quick quickwidgets testlib
isEmpty(GCS_BUILD_TREE):error("Set GCS_BUILD_TREE to the selected build's ground/gcs directory")
include(../gcs/gcs.pri)
include(../gcs/src/plugins/coreplugin/coreplugin.pri)
include(../gcs/src/plugins/uavobjects/uavobjects.pri)
include(../gcs/src/libs/utils/utils.pri)
include(../gcs/src/libs/extensionsystem/extensionsystem.pri)
LIBS += -L$$GCS_PLUGIN_PATH/droniNG
QMAKE_RPATHDIR += $$GCS_LIBRARY_PATH $$GCS_PLUGIN_PATH/droniNG
INCLUDEPATH += ../gcs/src/plugins
DEFINES += PFD_QML_FILE=\\\"$$PWD/../gcs/share/pfd/default/Pfd.qml\\\"
SOURCES += pfd_render.cpp ../gcs/src/plugins/pfdqml/pfdqmlgadgetwidget.cpp
HEADERS += ../gcs/src/plugins/pfdqml/pfdqmlgadgetwidget.h
SOURCES += ../gcs/src/plugins/qmlview/qmlviewgadgetwidget.cpp
HEADERS += ../gcs/src/plugins/qmlview/qmlviewgadgetwidget.h
