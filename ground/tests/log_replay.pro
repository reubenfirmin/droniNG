TEMPLATE = app
TARGET = log-replay-tests
CONFIG += console testcase c++17
QT += widgets testlib
isEmpty(GCS_BUILD_TREE):error("Set GCS_BUILD_TREE to the selected build's ground/gcs directory")
include(../gcs/gcs.pri)
include(../gcs/src/plugins/coreplugin/coreplugin.pri)
LIBS += -L$$GCS_PLUGIN_PATH/droniNG
INCLUDEPATH += ../gcs/src/plugins
QMAKE_RPATHDIR += $$GCS_LIBRARY_PATH $$GCS_PLUGIN_PATH/droniNG
SOURCES += log_replay.cpp ../gcs/src/plugins/logging/logfile.cpp
HEADERS += ../gcs/src/plugins/logging/logfile.h
