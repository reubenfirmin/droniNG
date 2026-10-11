QT += opengl openglwidgets
LIBS *= -l$$qtLibraryName(GLC_lib)
INCLUDEPATH += $$clean_path($$PWD/../../../../../downloads/glc-qt6/src/lib)
