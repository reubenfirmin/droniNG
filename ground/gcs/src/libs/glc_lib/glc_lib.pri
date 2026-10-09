QT += opengl
LIBS *= -l$$qtLibraryName(GLC_lib)
greaterThan(QT_MAJOR_VERSION, 5) {
    QT += openglwidgets
    INCLUDEPATH += $$clean_path($$PWD/../../../../../downloads/glc-qt6/src/lib)
} else {
    INCLUDEPATH += $$PWD
}
