TEMPLATE = app
TARGET = connection-diagnostics-tests
CONFIG += console testcase c++17
QT += widgets serialport network testlib svg
QT += svgwidgets
QT += sql xml
isEmpty(GCS_BUILD_TREE):error("Set GCS_BUILD_TREE to the selected build's ground/gcs directory")
include(../gcs/gcs.pri)
include(../gcs/src/plugins/coreplugin/coreplugin.pri)
include(../gcs/src/plugins/uavobjects/uavobjects.pri)
include(../gcs/src/plugins/uavtalk/uavtalk.pri)
include(../gcs/src/plugins/uavobjectutil/uavobjectutil.pri)
include(../gcs/src/plugins/uploader/uploader.pri)
include(../gcs/src/plugins/pathplanner/pathplanner.pri)
LIBS += -lOPMapGadget
include(../gcs/src/plugins/rawhid/rawhid.pri)
include(../gcs/src/libs/utils/utils.pri)
include(../gcs/src/libs/tlmapcontrol/tlmapcontrol.pri)
include(../gcs/src/libs/extensionsystem/extensionsystem.pri)
LIBS += -L$$GCS_PLUGIN_PATH/droniNG
QMAKE_RPATHDIR += $$GCS_LIBRARY_PATH $$GCS_PLUGIN_PATH/droniNG
INCLUDEPATH += ../gcs/src/plugins
DEFINES += CORE_PLUGIN_FILE=\\\"$$GCS_PLUGIN_PATH/droniNG/libCore.so\\\"
DEFINES += GCS_BUILD_TREE_PATH=\\\"$$GCS_BUILD_TREE\\\"
SOURCES += connection_diagnostics.cpp \
    ../gcs/src/plugins/uavtalk/telemetry.cpp \
    ../gcs/src/plugins/uavtalk/telemetrymonitor.cpp \
    ../gcs/src/plugins/serialconnection/serialplugin.cpp \
    ../gcs/src/plugins/serialconnection/serialpluginconfiguration.cpp \
    ../gcs/src/plugins/serialconnection/serialpluginoptionspage.cpp \
    ../gcs/src/plugins/serialconnection/serialdevice.cpp
HEADERS += ../gcs/src/plugins/uavtalk/telemetry.h \
    ../gcs/src/plugins/uavtalk/telemetrymonitor.h \
    ../gcs/src/plugins/serialconnection/serialplugin.h \
    ../gcs/src/plugins/serialconnection/serialpluginconfiguration.h \
    ../gcs/src/plugins/serialconnection/serialpluginoptionspage.h \
    ../gcs/src/plugins/serialconnection/serialdevice.h
FORMS += ../gcs/src/plugins/serialconnection/serialpluginoptions.ui
