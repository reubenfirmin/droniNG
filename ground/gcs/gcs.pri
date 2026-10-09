include (../tools.pri)

isEmpty(BRANDING_PATH) {
BRANDING_PATH = $$PWD/../../branding
DEFINES += BRANDING_PATH=\\\"$$BRANDING_PATH\\\"
include ($$BRANDING_PATH/gcs_definitions.pri)
}

defineReplace(cleanPath) {
    return($$clean_path($$1))
}

defineReplace(targetPath) {
    return($$shell_path($$1))
}

defineReplace(addNewline) { 
    return($$escape_expand(\\n\\t))
}

defineReplace(qtLibraryName) {
   return($$1)
}

# For use in custom compilers which just copy files
defineReplace(stripSrcDir) {
    return($$relative_path($$absolute_path($$1, $$OUT_PWD), $$_PRO_FILE_PWD_))
}

isEmpty(TEST):CONFIG(debug, debug|release) {
    !debug_and_release|build_pass {
        TEST = 1
    }
}

isEmpty(GCS_LIBRARY_BASENAME) {
    GCS_LIBRARY_BASENAME = lib
}

DEFINES += GCS_LIBRARY_BASENAME=\\\"$$GCS_LIBRARY_BASENAME\\\"

equals(TEST, 1) {
    QT +=testlib
    DEFINES += WITH_TESTS
}

GCS_SOURCE_TREE = $$PWD
isEmpty(GCS_BUILD_TREE) {
    sub_dir = $$_PRO_FILE_PWD_
    sub_dir ~= s,^$$re_escape($$PWD),,
    GCS_BUILD_TREE = $$cleanPath($$OUT_PWD)
    GCS_BUILD_TREE ~= s,$$re_escape($$sub_dir)$,,
}
GCS_APP_PATH = $$GCS_BUILD_TREE/bin
linux {
    GCS_APP_WRAPPER  = $${GCS_PROJECT_BRANDING}-gcs
    GCS_APP_TARGET   = $${GCS_PROJECT_BRANDING}-gcs.bin
    GCS_LIBRARY_PATH = $$GCS_BUILD_TREE/$$GCS_LIBRARY_BASENAME/$$GCS_PROJECT_BRANDING
    GCS_PLUGIN_PATH  = $$GCS_LIBRARY_PATH/plugins
    GCS_LIBEXEC_PATH = $$GCS_APP_PATH # FIXME
    GCS_DATA_PATH    = $$GCS_BUILD_TREE/share
    GCS_DATA_BASENAME = share
    GCS_DOC_PATH     = $$GCS_BUILD_TREE/share/doc
    !isEqual(GCS_SOURCE_TREE, $$GCS_BUILD_TREE):copydata = 1
}
DEFINES += 'GCS_DATA_BASENAME=\\\"$$GCS_DATA_BASENAME\\\"'
# Include path to shared API directory
INCLUDEPATH *= \
    $$GCS_SOURCE_TREE/../../shared/api

INCLUDEPATH *= \
    $$GCS_SOURCE_TREE/src/libs
INCLUDEPATH *= $$GCS_BUILD_TREE

DEPENDPATH += \
    $$GCS_SOURCE_TREE/src/libs

LIBS += -L$$GCS_LIBRARY_PATH

# DEFINES += QT_NO_CAST_FROM_ASCII
DEFINES += QT_NO_CAST_TO_ASCII
#DEFINES += QT_USE_FAST_OPERATOR_PLUS
#DEFINES += QT_USE_FAST_CONCATENATION
DEFINES += QT_DEPRECATED_WARNINGS

# include additional information in debug log (file/line/function)
DEFINES += QT_MESSAGELOGCONTEXT

unix {
    CONFIG(debug, debug|release):OBJECTS_DIR = $${OUT_PWD}/.obj/debug-shared
    CONFIG(release, debug|release):OBJECTS_DIR = $${OUT_PWD}/.obj/release-shared

    CONFIG(debug, debug|release):MOC_DIR = $${OUT_PWD}/.moc/debug-shared
    CONFIG(release, debug|release):MOC_DIR = $${OUT_PWD}/.moc/release-shared

    RCC_DIR = $${OUT_PWD}/.rcc
    UI_DIR = $${OUT_PWD}/.uic
}

linux {
    # Bail out on non-selfcontained libraries. Just a security measure
    # to catch unresolved dependencies during the build.
    QMAKE_LFLAGS += -Wl,--allow-shlib-undefined -Wl,--no-undefined
}

unix {
	GEN_GCOV {
		QMAKE_CXXFLAGS += -g -Wall -fprofile-arcs -ftest-coverage -O0
		QMAKE_LFLAGS += -g -Wall -fprofile-arcs -ftest-coverage  -O0
		LIBS += \
		    -lgcov
		unix:OBJECTS_DIR = ./Build
		unix:MOC_DIR = ./Build
		unix:UI_DIR = ./Build
	}
}

# use ISO C++ (no GNU extensions) to ensure maximum portability
CONFIG += c++17 strict_c++

CONFIG(release, debug|release): unix {
    # generate debug info for:
    # a) debugging release builds with a native debugger
    # b) generating breakpad symbols for use with crashreporter
    # packaging scripts should deal with this appropriately (e.g. split it off into a separate package)
    CONFIG += force_debug_info
    CONFIG += separate_debug_info
}
