TEMPLATE = lib
TARGET = Utils

QMAKE_CXXFLAGS += -Wno-sign-compare

QT += gui \
    network \
    xml \
    svg \
    opengl \
    qml \
    quick \
    widgets

DEFINES += QTCREATOR_UTILS_LIB

include(../../gcslibrary.pri)

SOURCES += \
    pathchooser.cpp \
    hostosinfo.cpp \
    basevalidatinglineedit.cpp \
    qtcolorbutton.cpp \
    treewidgetcolumnstretcher.cpp \
    styledbar.cpp \
    coordinateconversions.cpp \
    pathutils.cpp \
    worldmagmodel.cpp \
    homelocationutil.cpp \
    mytabbedstackwidget.cpp \
    mytabwidget.cpp \
    svgimageprovider.cpp \
    scaledpixmaplabel.cpp \
    longlongspinbox.cpp

SOURCES += xmlconfig.cpp settingsxml.cpp darktheme.cpp

HEADERS += utils_global.h \
    pathchooser.h \
    hostosinfo.h \
    basevalidatinglineedit.h \
    qtcolorbutton.h \
    treewidgetcolumnstretcher.h \
    qtcassert.h \
    styledbar.h \
    coordinateconversions.h \
    pathutils.h \
    worldmagmodel.h \
    homelocationutil.h \
    mytabbedstackwidget.h \
    mytabwidget.h \
    svgimageprovider.h \
    scaledpixmaplabel.h \
    longlongspinbox.h


HEADERS += xmlconfig.h settingsxml.h darktheme.h

FORMS +=

RESOURCES += utils.qrc
