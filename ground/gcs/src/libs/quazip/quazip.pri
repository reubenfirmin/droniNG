LIBS *= -l$$qtLibraryName(quazip)
INCLUDEPATH += $$PWD
greaterThan(QT_MAJOR_VERSION, 5):QT += core5compat
