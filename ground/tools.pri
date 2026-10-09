TOOLS_DIR = $$PWD/../tools

!linux:error("droniNG desktop builds support Linux only")

# if ccache is in CONFIG, use it
ccache {
    *-g++*|*-clang* {
        QMAKE_CC=$$(CCACHE_BIN) $$QMAKE_CC
        QMAKE_CXX=$$(CCACHE_BIN) $$QMAKE_CXX
    }
}

equals(QT_MAJOR_VERSION, 5) {
    !equals(QT_MINOR_VERSION, 15)|lessThan(QT_PATCH_VERSION, 2) {
        error("Qt 5.15.2+ required for the Qt 5 baseline")
    }
} else:equals(QT_MAJOR_VERSION, 6) {
    lessThan(QT_MINOR_VERSION, 10):error("Qt 6.10+ required; release target is Qt 6.12.0")
} else {
    error("Select Qt 5.15.2+ or Qt 6.10+ qmake; release target is Qt 6.12.0")
}

# Must match make/tools.mk
BREAKPAD = $$TOOLS_DIR/breakpad/6598c9c33fc02da7805401f3b0f1a733e6a24071
