!linux:error("GLC integration supports Linux only")
!equals(QT_MAJOR_VERSION, 6):error("Qt 6 is required")

# Must match make/tools.mk. Keep upstream source unmodified and reproducible.
GLC_QT6_REV = b292ccf7851576d71ee9bed7ced4c340b910159a
GLC_QT6_CHECKOUT = $$clean_path($$PWD/../../../../../downloads/glc-qt6)
GLC_QT6_SOURCE = $$GLC_QT6_CHECKOUT/src/lib
!exists($$GLC_QT6_SOURCE/lib.pro):error("Run make glc6_download before configuring GLC")
GLC_QT6_ACTUAL_REV = $$system(git -C $$shell_quote($$GLC_QT6_CHECKOUT) rev-parse HEAD)
!equals(GLC_QT6_ACTUAL_REV, $$GLC_QT6_REV):error("GLC checkout does not match the pinned revision")

include($$GLC_QT6_SOURCE/lib.pro)

# Use one system zlib. The newer QuaZIP bundled by GLC stays private: it has
# a different ABI from the GCS archive library and must not interpose on it.
SOURCES -= $$find(SOURCES, ^3rdparty/zlib/)
HEADERS -= $$HEADERS_ZLIB
INCLUDEPATH -= ./3rdparty/zlib
LIBS += -lz
QMAKE_LFLAGS += -Wl,--version-script=$$shell_quote($$PWD/qt6.exports)
PRE_TARGETDEPS += $$PWD/qt6.exports

# Upstream paths are relative to its source directory, not this wrapper.
GLC_QT6_SOURCES =
for(file, SOURCES):GLC_QT6_SOURCES += $$absolute_path($$file, $$GLC_QT6_SOURCE)
SOURCES = $$GLC_QT6_SOURCES
GLC_QT6_HEADERS =
for(file, HEADERS):GLC_QT6_HEADERS += $$absolute_path($$file, $$GLC_QT6_SOURCE)
HEADERS = $$GLC_QT6_HEADERS
RESOURCES = $$GLC_QT6_SOURCE/glc_lib.qrc
INCLUDEPATH = $$GLC_QT6_SOURCE $$GLC_QT6_SOURCE/3rdparty/quazip
DEPENDPATH = $$GLC_QT6_SOURCE

# Replace upstream system installation and output locations with GCS rules.
INSTALLS =
VERSION =
# Use only the pinned upstream headers.
CONFIG += c++17 warn_off no_include_pwd
include(../../gcslibrary.pri)
