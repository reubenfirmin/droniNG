TEMPLATE = app
TARGET = plot-compat-tests
CONFIG += console testcase c++17
QT += widgets svg xml printsupport testlib
isEmpty(GCS_LIB_DIRECTORY):error("Set GCS_LIB_DIRECTORY to the selected build's lib/droning directory")
INCLUDEPATH += ../gcs/src/libs/qwt/src
DEFINES += QWT_DLL
LIBS += -L$$GCS_LIB_DIRECTORY -lQwt
QMAKE_RPATHDIR += $$GCS_LIB_DIRECTORY
SOURCES += plot_compat.cpp
