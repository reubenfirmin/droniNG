TEMPLATE = app
TARGET = glc-prototype-tests
CONFIG += console testcase c++17
QT += widgets opengl openglwidgets testlib
isEmpty(GLC_SOURCE_DIRECTORY):error("Set GLC_SOURCE_DIRECTORY to the pinned upstream src/lib directory")
isEmpty(GLC_LIB_DIRECTORY):error("Set GLC_LIB_DIRECTORY to the prototype library build directory")
INCLUDEPATH += $$GLC_SOURCE_DIRECTORY
LIBS += -L$$GLC_LIB_DIRECTORY -lGLC_lib -lGL
QMAKE_RPATHDIR += $$GLC_LIB_DIRECTORY
SOURCES += glc_prototype.cpp

!isEmpty(GCS_LIB_DIRECTORY) {
    QT += core5compat
    DEFINES += GCS_ARCHIVE_ISOLATION_TEST
    INCLUDEPATH += ../gcs/src/libs/quazip
    LIBS += -L$$GCS_LIB_DIRECTORY -lquazip -lz
    QMAKE_RPATHDIR += $$GCS_LIB_DIRECTORY
}
