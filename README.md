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
required release SDK is Qt 6.12.0. The default
appearance is dark, with colours inspired by KDE Breeze Dark and a shared blue
highlight. Widgets use one application stylesheet; Welcome inherits the same palette.
See [the Qt modernization plan](QT_MODERNIZATION_PLAN.md) for the staged Qt 6
migration, prerequisites, and validation gates. Embedded firmware and Android
targets are retained; their builds also use a Linux host.

Use a separate build directory when changing Qt SDKs. With the local Qt 6.12.0
kit installed, build and run the release application with:

```bash
make -j4 BUILD_DIR="$PWD/build/qt612-release" GCS_BUILD_CONF=release gcs
build/qt612-release/ground/gcs/bin/droning-gcs
```

The Linux build bundles the STM32 DFU installer and Revolution installation
image. Install the pinned firmware compiler once with `make arm_sdk_install`.
Click **Connect controller** on Welcome to open Firmware. The page detects connected
USB controllers; select yours and connect. With DroniNG running, choose **Tune & configure**,
**Flight data** or **Live graphs**. Firmware installation is a separate maintenance action.
For HGLRC F4 V6PRO, click **Install DroniNG** after detection.
Follow the board-specific BOOT instructions in the tab;
installation starts automatically when DFU appears, verifies the firmware and
checks flight data. Catalog expansion is tracked in the [firmware plan](FIRMWARE_PLAN.md).
Controllers already in STM32 BOOT mode can enter the same installer directly.
**Maintenance & recovery** provides an explicit installation choice and recovery for controllers
with an existing dRonin bootloader and individual partition operations.

To reset the HGLRC F4 V6PRO for another setup/installation test, close DroniNG and run:

```bash
python3 make/scripts/restore-board.py --flash
```

The script waits while you disconnect USB and the flight battery, hold the BOOT
button beside the USB socket, reconnect USB to the same port, then release BOOT.
It restores the original **Betaflight 3.2.3 / OMNIBUSF4** release with clean
settings, verifies the full flash and checks the running firmware's identity.
The factory STM32 ROM bootloader is already present. Backups and diagnostics
go to `build/firmware-restore/`. Running without `--flash` only downloads and
validates the image. See [the test reset workflow](FIRMWARE_PLAN.md#resetting-the-reference-board-for-ui-tests)
for offline use and troubleshooting.

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
