TEMPLATE = app
TARGET = map-startup-tests
CONFIG += console testcase c++17
QT += network testlib
QT -= gui
INCLUDEPATH += ../gcs/src/plugins/opmap
SOURCES += map_startup.cpp ../gcs/src/plugins/opmap/mapstartup.cpp
HEADERS += ../gcs/src/plugins/opmap/mapstartup.h
