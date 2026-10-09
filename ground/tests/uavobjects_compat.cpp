#include <QTest>
#include <uavobjects/uavobjectsplugin.h>

// Run the debug plugin's tests directly, without constructing the GCS or
// enabling transport/upload plugins. Tests exercise the actual shared library.
QTEST_GUILESS_MAIN(UAVObjectsPlugin)
