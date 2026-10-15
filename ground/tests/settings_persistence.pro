TEMPLATE = app
TARGET = settings-persistence-tests
CONFIG += console testcase c++17
QT += widgets xml testlib
isEmpty(GCS_BUILD_TREE):error("Set GCS_BUILD_TREE to the selected build's ground/gcs directory")
include(../gcs/gcs.pri)
include(../gcs/src/libs/utils/utils.pri)
QMAKE_RPATHDIR += $$GCS_LIBRARY_PATH
INCLUDEPATH += ../gcs/src/libs
SOURCES += settings_persistence.cpp
