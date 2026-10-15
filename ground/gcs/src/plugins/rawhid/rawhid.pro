TEMPLATE = lib
TARGET = RawHID
DEFINES += RAWHID_LIBRARY

include(../../gcsplugin.pri)

include(../../plugins/coreplugin/coreplugin.pri)

HEADERS += rawhid_global.h \
    rawhidplugin.h \
    rawhid.h \
    hidapi/hidapi.h \
    rawhid_const.h \
    usbmonitor.h \
    usbsignalfilter.h \
    usbdevice.h

SOURCES += rawhidplugin.cpp \
    rawhid.cpp \
    usbsignalfilter.cpp \
    usbdevice.cpp \
    usbmonitor.cpp

OTHER_FILES += RawHID.pluginspec

# Linux USB HID backend
linux {
    SOURCES += hidapi/hidapi_linux.c
    LIBS += -ludev -lrt
}
