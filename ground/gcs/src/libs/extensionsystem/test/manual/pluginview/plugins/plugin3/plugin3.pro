TEMPLATE = lib
TARGET = plugin3

SOURCES += plugin3.cpp
HEADERS += plugin3.h

include(../../../../extensionsystem_test.pri)

LIBS += -L$${PWD}/../plugin2 -lplugin2

linux {
    QMAKE_RPATHDIR += $${PWD}/../plugin2
}
