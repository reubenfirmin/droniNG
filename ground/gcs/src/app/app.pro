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

macx {
    LIBS += -framework CoreFoundation
    ICON = $$BRANDING_PATH/gcs_app.icns
    QMAKE_INFO_PLIST = $$BRANDING_PATH/Info.plist
    FILETYPES.files = profile.icns prifile.icns
    FILETYPES.path = Contents/Resources
    QMAKE_BUNDLE_DATA += FILETYPES
} else {
    target.path  = /bin
    INSTALLS    += target
}

RESOURCES += \
    app_resource.qrc

DISTFILES += \
    $$BRANDING_PATH/gcs_app.icns

