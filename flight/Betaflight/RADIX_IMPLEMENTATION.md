# Original RADIX firmware

Implementation date: 2026-10-11. The original BrainFPV RADIX now has a real
native STM32F446 firmware target and a matching Linux GCS installation bundle.
This follows the [dry run](RADIX_DRY_RUN.md) and
[firmware plan](../../FIRMWARE_PLAN.md). The app offers "Install DroniNG"
with supported capabilities and configuration instructions. Hardware acceptance
results are tracked in this engineering record; no installation or bench run
has been performed on the connected controller.

The importer verifies the 35 pinned manufacturer inputs and emits reviewed
SPI, FPGA and motor wiring plus profile identity and a bounded linker layout.
The shared initializer uses native BMI160, SPI BMP280, BrainFPV FPGA/video,
GPS, USB UAVTalk and telemetry modules. No Betaflight flight loop or new FPGA
bitstream is embedded. FPGA startup uses the configuration already stored on
the board. The importer accepts only reviewed profiles; it is not a general
translator for arbitrary Betaflight targets.

Recorded automated evidence on 2026-10-11: production-driver and image checks
passed, along with 105 installer cases, 109 connection/configuration cases and
the packaged application UI checks. The tested RADIX image SHA-256 is
`fbf46959a65dfe2add2d11e7e2819de80beec5a6a252777d82e0989297fa4e5f`.
These checks cover device/bus failures, bounded waits and recovery, sensor
transforms, output-mode rejection, exact flash ranges, storage preservation,
write/readback failures and runtime identity. The hardware acceptance tasks
below cover physical timing, signal behavior and flight performance.

## Build and checks

From the repository root:

```sh
flight/tests/run_radix.py
```

This entrypoint takes no parameters, builds `ef_radix`, runs the actual native
FPGA driver under a hardware substitute with undefined-behavior traps, and
compiles unchanged production SPI and BMI160 functions for timeout, recovery,
axis and scale tests. It checks video sync storm recovery, reserved ELF ranges
and every HEX record, and verifies that PWM/OneShot settings recover after
DShot rejection. It never accesses USB or a flight controller. Evidence
is written to `build/qt612-release/radix-tests/report.json`.

The individual build is:

```sh
CCACHE_DISABLE=1 make --jobserver-style=pipe -j8 ef_radix BUILD_DIR="$PWD/build/qt612-release" CCACHE_BIN=
```

Artifacts are `build/qt612-release/ef_radix/ef_radix.{hex,bin,manifest.json}`.
The ELF/map and import report remain under `fw_radix`. The linked image uses
about 290 KB of flash. The linker reserves at least 48 KiB of runtime heap;
actual runtime heap, interrupt and stack margins still need measurement.

`make gcs` bundles Revolution, RADIX and Omnibus F4 SD images with their build-matched
UAVObjects. The GCS accepts the Radix profile/layout hash compiled into its
resources and rejects stale images, unsafe regions, vectors, board IDs or
protocol hashes. The native installer tests in
`ground/tests/firmware_installation.cpp` simulate three separate writes and
readbacks, verify preservation of settings/autotune, reject different erase
geometry and prevent completion for a different runtime profile. Connection
tests exercise the RADIX offer, ambiguous identifiers and native port UI.
Radix setup and output configuration exclude DShot; firmware rejects those
settings and holds outputs in failsafe until a supported mode is selected.
The native ADC order is current/voltage/RSSI, matching its GCS labels and
settings defaults. Manufacturer calibration uses a 3.245 V reference,
83.333 mV/V voltage factor and 20 mV/A current factor; saved calibration is
preserved. Battery voltage/current still require measurement on the bench.
Conversions follow the pinned manufacturer's
[voltage](https://github.com/BrainFPV/betaflight/blob/d966f1f3a12df8446a623110d753b8dd72ebeaa4/src/main/sensors/voltage.c)
and [current](https://github.com/BrainFPV/betaflight/blob/d966f1f3a12df8446a623110d753b8dd72ebeaa4/src/main/sensors/current.c)
meter equations and its target constants.

## Flash layout and installation

| Region | Address | Size | Installer writes |
| --- | --- | --- | --- |
| Direct-boot vectors | `0x08000000` | 16 KiB | Yes |
| Settings banks | `0x08004000` | 32 KiB | No |
| Board/profile identity | `0x0800c000` | 16 KiB | Yes |
| Autotune storage | `0x08010000` | 64 KiB | No |
| Application and 100-byte descriptor | `0x08020000` | 384 KiB | Yes |

Board type/revision are `0xe0/1`, layout is `radix-f446-rom-v1`, profile is
`radix`. Sector 0 contains vectors that enter application code in sector 5.
There is no custom bootloader. The generated HEX contains only the three
writable ranges. **The BIN and TLFW files are build containers containing
reserved gaps; do not write either as a whole-flash raw download.** Use the
GCS installer, which validates the manifest and writes/verifies the three
regions separately, or a tool that respects the generated HEX addresses.

The GCS retains the STM32 ROM bootloader and requires its exact 512 KiB F446
erase geometry. It verifies the flashed bytes, reconnects on the same USB
port, checks the firmware descriptor and receives the runtime `BoardProfile`
schema/ID/layout/hash before reporting success. Native port settings use
`HwImported`; the native HID telemetry interface remains available. Updating
Radix firmware from the firmware page follows the same ROM DFU workflow.

The manufacturer's [manual recovery procedure](https://www.brainfpv.com/knowledgebase/betaflight-quick-start-guide/)
for original RADIX uses a temporary connection between the BOOT pad on the
bottom side and the 3.3 V pad directly opposite it on the top side, held while
connecting USB. Disconnect
power and remove that connection before normal startup. RADIX 2 uses a
different bootloader and is excluded. A four-character `RADI` target only
auto-selects original RADIX when the USB product also identifies it exactly;
otherwise the GCS requires a model selection. Recovery to Betaflight uses
the same ROM DFU route with the manufacturer's original-RADIX HEX and a
separate saved Betaflight configuration. Existing Betaflight configuration
does not become native droniNG settings.

Installation UX follow-up: request DFU automatically through the identified
Betaflight connection when the user selects Install DroniNG. Confirm the ROM
DFU device on the same USB port before any write. Retain the pad procedure as
manual recovery when the firmware cannot accept a reboot request; expose a
clear retry if the software transition fails. The current first-install path
requires manual BOOT entry.

## Required bench work

Record these results against the exact image, controller and configuration:

1. Confirm original RADIX PCB/processor, cold and warm boot, native USB
   descriptor/profile identity, watchdog recovery and BOOT-pad ROM recovery.
2. Check BMI160 identification, sample timing and signs with known board
   rotations and gravity; check BMP280 altitude and disconnect/failure behavior.
3. Check M1–M4 numbering, inactive startup, arming/disarming and PWM/OneShot
   waveforms with propellers removed. Native motor order must be verified in
   the setup wizard; it must not inherit Betaflight ordering silently.
4. Test RX3 SBUS inversion and the selected receiver protocol alongside GPS
   on UART4, external telemetry on UART1 and optional SPort on TX6. Check
   failsafe, GPS loss/staleness and receiver-telemetry duplex operation.
5. Test PAL/NTSC video, fonts and live flight/GPS data, no camera, noisy sync
   suppression/recovery and shared SPI behavior during sensor acquisition.
6. Save/reboot/load settings, capture and persist autotune data, update via
   segmented ROM DFU, verify storage preservation and exercise return to the
   manufacturer's firmware. Record runtime RAM/heap/stack/CPU/timing margins.
7. After those checks pass, perform controlled flight and native autotune
   validation before changing the support level.

DShot DMA, SD flight logging, additional motor outputs, RGB LEDs and camera
control are not implemented in this increment. UART4 GPS excludes ESC
telemetry on RX4. Physical behavior and return-to-Betaflight recovery have
not been tested on the connected board.
