TEMPLATE  = subdirs
CONFIG   += ordered
QT += widgets
SUBDIRS   = \
    qscispinbox\
    aggregation \
    extensionsystem \
    utils \
    tlmapcontrol \
    qwt \
    libcrashreporter-qt \
    runguard

SUBDIRS   += \
    quazip
SDL {
SUBDIRS += sdlgamepad
}

!LIGHTWEIGHT_GCS {
    SUBDIRS += glc_lib
    greaterThan(QT_MAJOR_VERSION, 5):glc_lib.file = glc_lib/qt6.pro
}

SUBDIRS +=
