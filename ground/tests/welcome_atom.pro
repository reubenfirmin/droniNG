TEMPLATE = app
TARGET = welcome-atom-tests
CONFIG += console testcase c++17
QT += network qml quick widgets testlib
INCLUDEPATH += $$PWD/../gcs/src/plugins/welcome
INCLUDEPATH += $$PWD/../gcs/src/libs
DEFINES += QTCREATOR_UTILS_STATIC_LIB
DEFINES += WELCOME_PANEL_DIR=\\\"$$PWD/../gcs/share/welcome\\\"
DEFINES += NEWS_PANEL_FILE=\\\"$$PWD/../gcs/share/welcome/NewsPanel.qml\\\"
SOURCES += welcome_atom.cpp ../gcs/src/plugins/welcome/atomfeedmodel.cpp
SOURCES += ../gcs/src/libs/utils/darktheme.cpp
HEADERS += ../gcs/src/plugins/welcome/atomfeedmodel.h
