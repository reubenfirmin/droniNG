TEMPLATE = app
TARGET = dark-widget-tests
CONFIG += console testcase c++17
QT += widgets testlib
isEmpty(GCS_BUILD_TREE):error("Set GCS_BUILD_TREE to the selected build's ground/gcs directory")
include(../gcs/gcs.pri)
include(../gcs/src/plugins/uavobjects/uavobjects.pri)
include(../gcs/src/libs/utils/utils.pri)
include(../gcs/src/libs/extensionsystem/extensionsystem.pri)
include(../gcs/src/libs/qwt/qwt.pri)
include(../gcs/src/libs/qscispinbox/qscispinbox.pri)
LIBS += -L$$GCS_PLUGIN_PATH/droniNG
QMAKE_RPATHDIR += $$GCS_LIBRARY_PATH $$GCS_PLUGIN_PATH/droniNG
INCLUDEPATH += ../gcs/src/plugins
SOURCES += dark_widgets.cpp \
    ../gcs/src/plugins/config/expocurve.cpp \
    ../gcs/src/plugins/uavobjectbrowser/uavobjecttreemodel.cpp \
    ../gcs/src/plugins/uavobjectbrowser/treeitem.cpp \
    ../gcs/src/plugins/uavobjectbrowser/fieldtreeitem.cpp
HEADERS += ../gcs/src/plugins/config/expocurve.h \
    ../gcs/src/plugins/uavobjectbrowser/uavobjecttreemodel.h \
    ../gcs/src/plugins/uavobjectbrowser/treeitem.h \
    ../gcs/src/plugins/uavobjectbrowser/fieldtreeitem.h
