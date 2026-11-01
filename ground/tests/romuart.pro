TEMPLATE = app
TARGET = romuart-tests
QT += core testlib
QT -= gui
CONFIG += console testcase c++17
DEFINES += CORE_LIBRARY
INCLUDEPATH += ../gcs/src/plugins
SOURCES += romuart_tests.cpp ../gcs/src/plugins/coreplugin/romuart.cpp
