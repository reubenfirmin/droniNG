include(../gcs.pri)

DESTDIR = $$GCS_LIBRARY_PATH

include(rpath.pri)

TARGET = $$qtLibraryName($$TARGET)

contains(QT_CONFIG, reduce_exports):CONFIG += hGCS_symbols

!macx {
    target.path = /$$GCS_LIBRARY_BASENAME/dronin
    INSTALLS += target
}
