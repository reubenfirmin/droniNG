cache()

include(gcs.pri)
!build_pass:message("QMAKE_CXX = $$QMAKE_CXX")
!build_pass:message("QMAKE_CC = $$QMAKE_CC")

TEMPLATE  = subdirs
CONFIG   += ordered

SUBDIRS = src share copydata
linux:!isEmpty(copydata) {
	bin_install.target = FORCE
	bin_install.commands = cp $$GCS_SOURCE_TREE/bin/gcs $$GCS_APP_PATH/$$GCS_APP_WRAPPER
	QMAKE_EXTRA_TARGETS += bin_install
}

copydata.file = copydata.pro
