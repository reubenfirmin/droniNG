TEMPLATE = app
TARGET = archive-compat-tests
CONFIG += console testcase c++17
QT += core testlib
QT -= gui
QT += core5compat
isEmpty(GCS_LIB_DIRECTORY):error("Set GCS_LIB_DIRECTORY to the selected build's lib/droning directory")
INCLUDEPATH += ../gcs/src/libs/quazip
LIBS += -L$$GCS_LIB_DIRECTORY -lquazip -lz
QMAKE_RPATHDIR += $$GCS_LIB_DIRECTORY
SOURCES += archive_compat.cpp
