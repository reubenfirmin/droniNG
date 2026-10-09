include(../../gcs.pri)
include(../../gcsversioninfo.pri)

TEMPLATE = app
TARGET = $$GCS_APP_TARGET
DESTDIR = $$GCS_APP_PATH
QT += xml
QT += widgets
SOURCES += \
    main.cpp \
    customsplash.cpp


include(../rpath.pri)
include(../libs/utils/utils.pri)
include(../libs/libcrashreporter-qt/libcrashreporter-qt.pri)
include(../libs/runguard/runguard.pri)

HEADERS += \
    customsplash.h

LIBS *= -l$$qtLibraryName(ExtensionSystem) -l$$qtLibraryName(Aggregation)

target.path  = /bin
INSTALLS    += target

RESOURCES += \
    app_resource.qrc

