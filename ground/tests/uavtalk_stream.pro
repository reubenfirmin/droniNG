TEMPLATE = app
TARGET = uavtalk-stream-tests
CONFIG += console testcase c++17
QT += core network testlib
QT -= gui
isEmpty(GCS_BUILD_TREE):error("Set GCS_BUILD_TREE to the selected build's ground/gcs directory")
include(../gcs/gcs.pri)
include(../gcs/src/plugins/uavobjects/uavobjects.pri)
include(../gcs/src/plugins/uavtalk/uavtalk.pri)
LIBS += -L$$GCS_PLUGIN_PATH/droniNG
QMAKE_RPATHDIR += $$GCS_LIBRARY_PATH $$GCS_PLUGIN_PATH/droniNG
INCLUDEPATH += ../gcs/src/plugins
SOURCES += uavtalk_stream.cpp
