TEMPLATE = app
TARGET = sound-sequence-tests
CONFIG += console testcase c++17
QT += multimedia testlib
SOURCES += sound_sequence.cpp ../gcs/src/plugins/notify/soundsequenceplayer.cpp
HEADERS += ../gcs/src/plugins/notify/soundsequenceplayer.h
INCLUDEPATH += ../gcs/src/plugins/notify
