TEMPLATE = app
TARGET = uavobjects-compat-tests
CONFIG += console testcase c++17
QT += core testlib
QT -= gui
DEFINES += WITH_TESTS
isEmpty(GCS_LIB_DIRECTORY):error("Set GCS_LIB_DIRECTORY to a debug build's lib/droning directory")
INCLUDEPATH += ../gcs/src/libs ../gcs/src/plugins
LIBS += -L$$GCS_LIB_DIRECTORY/plugins/droniNG -lUAVObjects
QMAKE_RPATHDIR += $$GCS_LIB_DIRECTORY $$GCS_LIB_DIRECTORY/plugins/droniNG
SOURCES += uavobjects_compat.cpp
