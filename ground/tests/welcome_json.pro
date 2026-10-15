TEMPLATE = app
TARGET = welcome-json-tests
CONFIG += console testcase c++17
QT += qml quick widgets testlib
DEFINES += JSON_MODEL_FILE=\\\"$$PWD/../gcs/share/welcome/JSONListModel/JSONListModel.qml\\\"
SOURCES += welcome_json.cpp
