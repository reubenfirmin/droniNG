# Controller restore scripts

[`restore-board.py`](restore-board.py) restores a reference controller to a
pinned firmware release with clean settings so connection, foreign-firmware
detection, and DroniNG installation can be tested repeatedly. Run commands from
the repository root and close DroniNG before flashing or checking startup.

## Requirements and shared usage

The script uses Python 3's standard library and dfu-util 0.11. Build the bundled
helper with `make firmware_installer_runtime` if it is missing. Controller USB
permissions must be configured; access failures provide the package's copyable
USB-rule installation command.

Select the board with `--board`. With no mode flag, the script downloads/caches
and validates the pinned image without performing DFU operations. `--flash`
backs up internal flash, restores the clean image, verifies every byte, then
checks the running firmware's identity. `--check` checks startup without flashing.

The [restore catalog](../../shared/firmware/test-restores.json) defines the
available profiles, release pins, flash geometry, BOOT instructions, and
expected firmware identity:

| Board | `--board` value | Restored firmware |
| --- | --- | --- |
| HGLRC F4 V6PRO | `hglrc-f4-v6pro` (default) | Betaflight 3.2.3 / OMNIBUSF4 |
| Omnibus F4 SD | `omnibusf4sd` | Betaflight 3.4.0 / OMNIBUSF4SD |

## HGLRC F4 V6PRO

```bash
# Download/cache and validate the pinned official image; no DFU operations.
python3 make/scripts/restore-board.py --board hglrc-f4-v6pro

# Close DroniNG, then start the reset and follow its BOOT instructions.
python3 make/scripts/restore-board.py --board hglrc-f4-v6pro --flash

# Confirm firmware identity after an ordinary USB reconnect; no flashing.
python3 make/scripts/restore-board.py --board hglrc-f4-v6pro --check
```

When prompted, disconnect USB and the flight battery, hold the BOOT button
beside the USB socket, reconnect USB to the same port, then release BOOT. The
script waits for STM32 DFU, restores the original Betaflight 3.2.3 / OMNIBUSF4
release with clean settings, verifies flash, and checks `BTFL`, version `3.2.3`,
board `OBF4`, and MSP API `1.36` on the same USB port.

After a successful reset, open DroniNG and exercise connection and installation
again. The original firmware identity is recorded in the saved read-only MSP
probe referenced by the [firmware plan](../../FIRMWARE_PLAN.md#resetting-the-reference-board-for-ui-tests).

## Omnibus F4 SD

[`restore-omnibusf4sd.py`](restore-omnibusf4sd.py) selects the Omnibus profile
and keeps its cache and logs under `build/firmware-restore/omnibusf4sd/`.
Unlike the shared script, this wrapper flashes by default:

```bash
# Validate/cache the pinned image without flashing.
python3 make/scripts/restore-omnibusf4sd.py --prepare

# Close DroniNG, then restore Betaflight 3.4.0 / OMNIBUSF4SD.
python3 make/scripts/restore-omnibusf4sd.py

# Check startup without flashing.
python3 make/scripts/restore-omnibusf4sd.py --check
```

When prompted, disconnect USB and the flight battery, hold BOOT while
reconnecting USB to the same port, then release BOOT.

For recovery testing, `--invalid` restores the pinned DroniNG IMU startup-failure
fixture and confirms its USB reset loop. This fixture requires the preserved
ELF specified in the restore catalog or a previously validated cached image.
After restoring it, open DroniNG to test recovery and reinstallation. See the
[Omnibus implementation notes](../../flight/Betaflight/OMNIBUSF4SD_IMPLEMENTATION.md)
for board validation details.

## Device selection, offline use, and recovery

The shared script selects the controller's USB topology before the BOOT
transition and waits up to 120 seconds for ROM DFU. Multiple controllers require
an explicit selection, such as `--usb-path 3-1.4.3.4`. Use `--wait` to change the
BOOT timeout. Every transfer selects DFU configuration, interface, alternate,
and serial number; an unknown or incompatible flash layout stops before erase.

Cached images are reused on subsequent runs. The shared script accepts
`--image /path/to/pinned-release.hex` for an offline copy with the same checksum,
`--dfu-util /path/to/package/bin/dfu-util` for another packaged helper, and
`--output /path/to/results` for a different cache and backup location.

If flash verification succeeds but startup needs a manual reconnect, release
BOOT, unplug/reconnect USB, and rerun the relevant command with `--check` and
the same board and USB path. The verified flash does not need another write.
If writing was interrupted, reconnect in BOOT mode and rerun the restore.

## Backups and reset scope

Each flash run gets a dated directory under `build/firmware-restore/` (or the
selected output directory), containing:

- `before.bin`: the complete internal-flash backup saved before erase.
- `restore.bin` and `readback.bin`: the expected image and verification readback.
- `restore.log` and `result.json`: command output, stages, USB selection, hashes,
  and post-boot identity.

The official HEX length and hash, decoded full-flash hash, record checksums,
bounds, overlaps, stack, and executable reset vectors are validated before DFU
operations. The controller is rechecked after backup and leaves DFU only after
every readback byte matches.

The reset replaces all internal flash, including settings and any dRonin flash
bootloader, with the pinned image and `0xff` in unused space. The factory STM32
ROM bootloader remains available. Option bytes and external SPI flash are
outside the reset; no unprotect command is sent.

## Adding boards and regression checks

Add reference boards to the restore catalog with their recorded original
firmware, official image pins, geometry, BOOT procedure, and post-boot identity.
Keep board-specific procedures here alongside the scripts.

Run the shared script's regression checks without real USB or flash access:

```bash
python3 ground/tests/restore_board.py
```

These checks use a simulated DFU helper and a pseudo-terminal serial protocol
to exercise reset, backup, readback, image validation, device selection, access
failures, interrupted writes, timeouts, and post-boot identity checks.
