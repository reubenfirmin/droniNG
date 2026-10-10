TEMPLATE = app
TARGET = log-replay-tests
CONFIG += console testcase c++17
QT += widgets testlib
isEmpty(GCS_BUILD_TREE):error("Set GCS_BUILD_TREE to the selected build's ground/gcs directory")
include(../gcs/gcs.pri)
include(../gcs/src/plugins/coreplugin/coreplugin.pri)
include(../gcs/src/libs/utils/utils.pri)
include(../gcs/src/libs/extensionsystem/extensionsystem.pri)
LIBS += -L$$GCS_PLUGIN_PATH/droniNG
INCLUDEPATH += ../gcs/src/plugins
DEFINES += CORE_PLUGIN_FILE=\\\"$$GCS_PLUGIN_PATH/droniNG/libCore.so\\\"
DEFINES += 'GCS_VERSION_INFO_FILE=\\\"$$GCS_BUILD_TREE/gcsversioninfo.h\\\"'
QMAKE_RPATHDIR += $$GCS_LIBRARY_PATH $$GCS_PLUGIN_PATH/droniNG
SOURCES += log_replay.cpp ../gcs/src/plugins/logging/logfile.cpp
HEADERS += ../gcs/src/plugins/logging/logfile.h
