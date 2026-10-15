TEMPLATE = app
TARGET = firmware-installation-tests
CONFIG += console testcase c++17
QT += widgets testlib
isEmpty(GCS_BUILD_TREE):error("Set GCS_BUILD_TREE to the selected build's ground/gcs directory")
include(../gcs/gcs.pri)
include(../gcs/src/plugins/coreplugin/coreplugin.pri)
include(../gcs/src/plugins/uploader/uploader.pri)
include(../gcs/src/libs/utils/utils.pri)
include(../gcs/src/libs/extensionsystem/extensionsystem.pri)
LIBS += -L$$GCS_PLUGIN_PATH/droniNG
QMAKE_RPATHDIR += $$GCS_LIBRARY_PATH $$GCS_PLUGIN_PATH/droniNG
INCLUDEPATH += ../gcs/src/plugins
DEFINES += INSTALL_IMAGE_ROOT=\\\"$$GCS_BUILD_TREE/firmware\\\"
SOURCES += firmware_installation.cpp
