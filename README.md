![droniNG](artwork/Logo/droning_logo_full.svg)

droniNG is a revival of the older [dRonin](https://github.com/d-ronin/dRonin)
project, continuing its OpenPilot/Tau Labs lineage. It provides autopilot/flight
controller firmware and a desktop ground station for acro/racing, autonomous
flight, and vehicle research, and is released under the GPL.

Development focuses on Linux, with modernization in progress:

- **Linux:** Linux is the primary desktop platform and the build host for the
  ground station and firmware. Installation, USB permissions, controller
  discovery, and recovery are part of that focus.
- **UI modernization:** The ground station uses Qt 6, and work continues on
  clearer setup and configuration workflows, useful progress and recovery
  feedback, and a consistent dark appearance.
- **Broader flight controller coverage:** The project supports importing
  Betaflight board definitions and porting Betaflight chip drivers into the
  native PiOS hardware layer to bring droniNG to more controllers. Integration
  and hardware validation are ongoing; see the
  [Betaflight adaptation work](flight/Betaflight/README.md) and
  [firmware support plan](FIRMWARE_PLAN.md) for current progress.

See the [project repository](https://github.com/reubenfirmin/droniNG) for source,
documentation, and issue reports.

## Supported Targets

### Flight Controllers

The Linux ground station bundles installation images for the following boards.
Support and validation are specific to the listed hardware:

| Controller | Firmware target | Current status |
| --- | --- | --- |
| OpenPilot Revolution | `revolution` | Legacy native target; installation image bundled. |
| HGLRC F4 V6PRO | `revolution` | [Reviewed compatibility profile](shared/firmware/compatibility/hglrc-f4-v6pro.json); installation applies sensor orientation and OSD settings. |
| Omnibus F4 SD | `omnibusf4sd` | [Experimental](flight/Betaflight/OMNIBUSF4SD_IMPLEMENTATION.md); installation and live attitude telemetry confirmed. USB stability, peripheral acceptance, and flight validation remain in progress. |
| BrainFPV RADIX (original) | `radix` | [Experimental](flight/Betaflight/RADIX_IMPLEMENTATION.md); native image and installer available. Bench and flight validation pending. RADIX LI and RADIX 2 are excluded. |

RADIX and Omnibus F4 SD currently include four PWM/OneShot motor outputs,
OSD, GPS, telemetry, and autotune modules. DShot and SD logging are not implemented.
The linked board records describe capabilities, limitations, and remaining checks.
Further Betaflight-based board coverage is tracked in the
[firmware support plan](FIRMWARE_PLAN.md).

### Desktop Ground Station

The desktop ground station targets Linux, using Qt 6 and system zlib. The
required release SDK is Qt 6.12.0. The default
appearance is dark, with colours inspired by KDE Breeze Dark and a shared blue
highlight. Widgets use one application stylesheet; Welcome inherits the same palette.
UI modernization is ongoing. Embedded firmware and Android targets are retained;
their builds also use a Linux host.

Use a separate build directory when changing Qt SDKs. With the local Qt 6.12.0
kit installed, build and run the release application with:

```bash
make -j4 BUILD_DIR="$PWD/build/qt612-release" GCS_BUILD_CONF=release gcs
build/qt612-release/ground/gcs/bin/droning-gcs
```

The Linux build bundles the STM32 DFU installer and firmware installation
catalog. Install the pinned firmware compiler once with `make arm_sdk_install`.
Click **Connect controller** on Welcome to open Firmware. The page detects connected
USB controllers; select yours and connect. With DroniNG running, choose **Tune & configure**,
**Flight data** or **Live graphs**. Firmware installation is a separate maintenance action.
For supported controllers, click **Install DroniNG** and follow the board-specific
BOOT instructions in the tab;
installation starts automatically when DFU appears, verifies the firmware and
checks flight data. See the [firmware plan](FIRMWARE_PLAN.md) for board support and
installation details.
Controllers already in STM32 BOOT mode can enter the same installer directly.
**Maintenance & recovery** provides an explicit installation choice and recovery for controllers
with an existing dRonin bootloader and individual partition operations.

For development test resets, see the
[controller restore script documentation](make/scripts/restore-board.md).

Portable Linux packages include `rules.udev` for controller and STM32 DFU access.
From the extracted package directory, install these rules once:

```bash
sudo install -m 0644 rules.udev /etc/udev/rules.d/45-droning.rules && sudo udevadm control --reload-rules
```

At startup, DroniNG checks that its USB rules are installed and current. When
setup is needed, the Welcome page presents a controller-access setup card with
**Copy setup command** and an expandable **View command** section. The command
includes the package's full path. The card updates automatically after installation
and reconnecting the controller. Setup stays in Welcome without interrupting other
workspaces or opening a dialog.

The Python package
is named `droning`, and its command-line tools use the `droning-*` prefix.

### Legacy Firmware Targets

The inherited board targets below remain in the source tree alongside
Revolution. Their current hardware validation and maintenance are under review;
see the [target inventory and proposed retirements](FIRMWARE_PLAN.md#current-targets-and-hardware-availability).

| Controller family | Firmware target |
| --- | --- |
| AeroQuad32 | `aq32` |
| Original BrainFPV | `brain` |
| BrainFPV RE1 | `brainre1` |
| DTFAir DTFc / Colibri Race / TBS PowerCube | `dtfc` |
| Lumenier Lux V1 | `lux` |
| Matek F405-CTR | `matek405` |
| Omnibus F3 | `omnibusf3` |
| PikoBLX | `pikoblx` |
| Quantec Quanton / original TBS Colibri / Gemini | `quanton` |
| DTFAir Seppuku | `seppuku` |
| Tau Labs Sparky | `sparky` |
| Tau Labs Sparky2 / Brushed Sparky | `sparky2` |
| SP Racing F3 EVO / FrSky XSRF3E | `sprf3e` |

Compatibility depends on the exact board revision; later boards or clones need
their own wiring review. The original Colibri / Gemini coverage through Quanton
is limited. Target retirements remain proposals; these entries describe existing
firmware targets.

Auxiliary targets are separate: PipXtreme / OPLink radio (`pipxtreme`), PlayUAV
OSD (`playuavosd`), and FlyingPi I/O (`flyingpio`). Linux-hosted flight software
uses the separate `flightd` target.
