TEMPLATE = app
TARGET = plugin-spec-tests
CONFIG += console testcase c++17
QT += widgets xml testlib
INCLUDEPATH += ../gcs/src/libs
isEmpty(GCS_LIB_DIRECTORY):error("Set GCS_LIB_DIRECTORY to the selected build's lib/droning directory")
LIBS += -L$$GCS_LIB_DIRECTORY -lExtensionSystem -lAggregation
QMAKE_RPATHDIR += $$GCS_LIB_DIRECTORY
SOURCES += plugin_specs.cpp
