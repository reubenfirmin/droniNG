
INCLUDEPATH *= $$PWD/../..
!linux:error("droniNG desktop tests support Linux only")
LIBPATH*= $$PWD/../../../../lib

include(../extensionsystem.pri)

QT *= xml
