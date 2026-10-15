TEMPLATE = app
TARGET = qt-logging-tests
CONFIG += console testcase c++17
QT += widgets testlib
DEFINES += QTCREATOR_UTILS_STATIC_LIB
INCLUDEPATH += $$PWD/../gcs/src/libs
SOURCES += qt_logging.cpp ../gcs/src/libs/utils/qtlogging.cpp
HEADERS += ../gcs/src/libs/utils/qtlogging.h
