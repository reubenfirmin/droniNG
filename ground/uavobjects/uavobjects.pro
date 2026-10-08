#
# Qmake project for UAVObjects generation.
#
# TODO: provide some dependencies (now it builds every time)
#

TEMPLATE  = subdirs

# Some handy defines
defineReplace(addNewline) {
    return($$escape_expand(\\n\\t))
}

# QMAKESPEC should be defined by qmake but sometimes it is not
isEmpty(QMAKESPEC) {
    macx-g++:SPEC = macx-g++
    linux-g++:SPEC = linux-g++
    linux-g++-32:SPEC = linux-g++
    linux-g++-64:SPEC = linux-g++-64
} else {
    SPEC = $$QMAKESPEC
}

# Some platform-dependent options
unix {
    CONFIG(release, debug|release) {
        BUILD_CONFIG = release
    } else {
        BUILD_CONFIG = debug
    }
}

unix {
    uavobjects.commands += $(MKDIR) -p ../../uavobject-synthetics $$addNewline()

    uavobjects.commands += cd ../../uavobject-synthetics &&
    uavobjects.commands += ../ground/uavobjgenerator/uavobjgenerator
    uavobjects.commands += ../../shared/uavobjectdefinition ../.. &&

    uavobjects.commands += cd ../ground/gcs &&
    uavobjects.commands += $(QMAKE) ../../../ground/gcs/gcs.pro
    uavobjects.commands += -spec $$SPEC CONFIG+=$${BUILD_CONFIG} -r $$addNewline()
}

uavobjects.target = FORCE
QMAKE_EXTRA_TARGETS += uavobjects
