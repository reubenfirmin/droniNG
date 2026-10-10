![droniNG](artwork/Logo/droning_logo_full.svg)

droniNG is an autopilot/flight controller firmware for controllers in the OpenPilot/Tau Labs family.  It's aimed at a variety of use cases: acro/racing, autonomous flight, and vehicle research.

droniNG is a Linux-focused fork of [dRonin](https://github.com/d-ronin/dRonin),
released under the GPL.

See the [project repository](https://github.com/reubenfirmin/droniNG) for source,
documentation, and issue reports.

## Supported Targets

See the [firmware support plan](FIRMWARE_PLAN.md) for the current hardware
availability audit, proposed target retirements, and Betaflight board and driver
integration. Those proposals do not change the supported-target list yet.

### Desktop Ground Station

The desktop ground station targets Linux, using Qt 6 and system zlib. The
release SDK is Qt 6.12.0; Qt 5.15 remains a migration reference. The default
appearance is dark, with colours inspired by KDE Breeze Dark.
See [the Qt modernization plan](QT_MODERNIZATION_PLAN.md) for the staged Qt 6
migration, prerequisites, and validation gates. Embedded firmware and Android
targets are retained; their builds also use a Linux host.

Use a separate build directory when changing Qt SDKs. With the local Qt 6.12.0
kit installed, build and run the release application with:

```bash
make -j4 BUILD_DIR="$PWD/build/qt612-release" GCS_BUILD_CONF=release gcs
build/qt612-release/ground/gcs/bin/droning-gcs
```

The Python package
is named `droning`, and its command-line tools use the `droning-*` prefix.

### Full Support

- AeroQuad32
- [BrainFPV](http://brainfpv.com/)
- [BrainFPV RE1](http://brainfpv.com/) (no nav sensors on board)
- [DTFAir DTFc](http://www.dtfuhf.com/) (no nav sensors on board)  Also Colibri Race / TBS PowerCube.
- [DTFAir Seppuku](http://www.dtfuhf.com/)
- [Lumenier Lux](http://www.getfpv.com/lumenier-lux-flight-controller.html) (only V1, and no nav sensors are on board)
- OpenPilot Revolution (including many race-only variants)
- PikoBLX and clones
- Quantec Quanton
- Seriously Pro Racing F3 EVO (only the EVO variant) and clones
- Tau Labs Sparky
- [Tau Labs Sparky2](https://github.com/TauLabs/TauLabs/wiki/Sparky2)

### Limited Support

- [Team Black Sheep Colibri / Gemini hexcopter](http://team-blacksheep.com/products/prod:gemini)  (supported by Quanton target)
