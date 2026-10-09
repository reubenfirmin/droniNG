TEMPLATE = lib
TARGET = Droning
include(../../gcsplugin.pri)
include(../../plugins/uavobjects/uavobjects.pri)
include(../../plugins/coreplugin/coreplugin.pri)
include(../../plugins/uavobjectutil/uavobjectutil.pri)
include(../../plugins/uavobjectwidgetutils/uavobjectwidgetutils.pri)
include(../../../usbids.pri)

QT += svg

OTHER_FILES += Droning.pluginspec

HEADERS += \
    droningplugin.h \
    simulation.h \
    simulationconfiguration.h \
    omnibusf3.h \
    pikoblx.h \
    playuavosd.h \
    seppuku.h \
    seppukuconfiguration.h \
    sprf3e.h \
    sprf3econfiguration.h

SOURCES += \
    droningplugin.cpp \
    simulation.cpp \
    simulationconfiguration.cpp \
    omnibusf3.cpp \
    pikoblx.cpp \
    playuavosd.cpp \
    seppuku.cpp \
    seppukuconfiguration.cpp \
    sprf3e.cpp \
    sprf3econfiguration.cpp

RESOURCES += \
    droning.qrc

FORMS += \
    simulationconfiguration.ui \
    seppuku.ui \
    sprf3econfiguration.ui
