TEMPLATE = lib
TARGET = Uploader
DEFINES += UPLOADER_LIBRARY
QT += svg widgets xml
QT += testlib
QT += network

include(../../gcsplugin.pri)

include(../../libs/utils/utils.pri)

include(../../plugins/coreplugin/coreplugin.pri)
include(../../plugins/rawhid/rawhid.pri)
include(../../plugins/uavobjects/uavobjects.pri)
include(../../plugins/uavobjectutil/uavobjectutil.pri)
include(../../plugins/uavsettingsimportexport/uavsettingsimportexport.pri)
include(../../plugins/uavtalk/uavtalk.pri)

HEADERS += uploadergadget.h \
    firmwarebootcontroller.h \
    firmwareupgradecontroller.h \
    firmwaresettingsbackup.h \
    uploadergadgetfactory.h \
    uploadergadgetwidget.h \
    uploaderplugin.h \
    uploader_global.h \
    bl_messages.h \
    tl_dfu.h \
    upgradeassistantdialog.h

SOURCES += uploadergadget.cpp \
    firmwarebootcontroller.cpp \
    firmwareupgradecontroller.cpp \
    firmwaresettingsbackup.cpp \
    uploadergadgetfactory.cpp \
    uploadergadgetwidget.cpp \
    uploaderplugin.cpp \
    upgradeassistantdialog.cpp \
    tl_dfu.cpp

OTHER_FILES += Uploader.pluginspec

FORMS += uploader.ui

RESOURCES += \
    uploader.qrc
