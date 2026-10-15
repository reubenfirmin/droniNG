# Omnibus F4 SD firmware

Implementation date: 2026-10-11. The connected controller reports USB product
`OmnibusF4`, Betaflight 3.4.0, MSP API 1.39 and target `OMNIBUSF4SD` (`OBSD`).
The firmware catalog previously covered only `OMNIBUSF4`/`OBF4` through the
HGLRC compatibility profile. The SD target has different mounting, inverter
controls and storage, so it now has its own native F405 firmware offer.

The first physical installation failed its startup acceptance check. All three
firmware regions were written and read back successfully, but the controller
did not return with native USB or live telemetry. Inspection of that exact
executable found that reset skipped FPU initialization while the RTOS used FP
instructions. The corrected build enables and synchronizes FPU access before
early initialization, and uses physical flash interrupt vectors after ROM DFU.
The compiled startup checks pass and reject the preserved failed executable.
The second physical installation returned with native USB but repeatedly reset
with the two-blink IMU startup error. Its board initializer passed an
uninitialized in/out sensor handle; that caller now initializes it to null.
A production-caller regression catches the old code with poisoned stack
variables and passes the correction. The user subsequently confirmed live
attitude telemetry. The recovery-cycle checks below cover the remaining
connection and peripheral acceptance work.

[omnibusf4sd.lock.json](omnibusf4sd.lock.json) pins Betaflight commit
`cce8a59be4b85f0ac327b75b11556b3e488805f1` (3.4.0). The
[source manifest](omnibusf4sd-source-manifest.json) verifies the 25 reviewed
board, driver, clock, calibration, installation and linker inputs. The complete
revision is also cached under `downloads/betaflight`; new source review does
not require another network download. Upstream sources and licenses are
retained unchanged. Native scheduling, acquisition and flight control remain
in droniNG. The existing PiOS MPU6000/6500, BMP280 and MAX7456 drivers supply
the chip backends.

## Recovery cycle and sensor alarms, 2026-10-12

The user reproduced the saved IMU reset loop with the restore command, recovered
through the main card, installed and verified all three regions, then obtained
live telemetry by clicking Start controller. The first automatic USB open failed
before that successful connection. HID now records the node and OS errno;
installation retries up to six failed opens on its verified USB path while the
node and access rules settle. Losing an opened transport before live data still
stops immediately with the retained recovery diagnosis. The card's actions and
guidance remain stable as the failed controller enumerates and disappears.
Explicit Retry connection waits up to ten seconds for the same controller;
another identified board on that port does not satisfy the request. View log now
shares the install action row, with its expanded log inside the card. Failure
headings describe an unconfirmed connection rather than claiming firmware did
not start.

The live screenshot shows SENSOR critical and BARO error. Both imported boards
configured BMP280 oversampling as `3`, which sets continuous mode but leaves the
temperature and pressure acquisition fields zero. [Bosch's BMP280 datasheet,
section 4.3.4](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf)
defines those zero fields as skipped conversions, including pressure output
`0x80000`. The native driver rejects that pressure sample; after 100 ms without
barometer data, the sensor loop raises BARO error and SENSOR critical even while
gyro/attitude updates work. The board configurations now use
`BMP280_HIGH_RESOLUTION` (temperature x1, pressure x8). The driver rejects
disabled acquisition configurations and propagates conversion-write and
semaphore failures. Its pressure compensation avoids shifting negative signed
integers, which the new undefined-behavior checks exposed.

The host regression runs the complete SPI BMP280 driver with the actual Omnibus
and RADIX configuration declarations. It checks repeated valid measurements,
Bosch reference pressure/temperature compensation, skipped ADC configurations
and bus/write/read/semaphore failures. Telemetry logs alarm changes with receiver
and state-estimation reasons, plus received barometer/GPS readings. INPUT critical
matches unconfigured receiver settings; GPS error indicates no GPS messages on
the configured GPS port. The screenshot alone cannot establish their detailed
reason codes.

Verification of the rebuilt delivery passed: all 133 connection/controller
checks, the MPU/OSD/BMP280 host regressions and firmware startup/layout checks,
and the packaged application's navigation, inline installation, setup and
responsive layout checks. Recovery and install captures were inspected at wide
and narrow sizes. Evidence is in
`build/qt612-release/ui-checks/native-tests.log`,
`build/qt612-release/ui-checks/ui-check-w89mai_1/report.json` and
`build/qt612-release/omnibusf4sd-tests/report.json`. The bundled Omnibus image
SHA-256 is `db510eaef632965cf6413c25b7a65dc637c93c8f2bbe57a0c7d5ad7575198e5c`;
the RADIX image is
`246d59e57466204f6528ccb57cf85dd692423be3547ea65f36198dedd77a82be`.
Both packaged images match the rebuilt firmware. These checks were isolated
from physical USB devices; this correction has not yet been installed on the
user's board.

Bench acceptance for this correction: install the rebuilt image, confirm it
connects without Start controller, confirm BARO and SENSOR clear with ongoing
pressure/temperature/attitude updates, then unplug/replug USB and repeat the
saved-failure recovery cycle. Receiver/GPS alarms must be checked again after
their corresponding setup; they are not suppressed by this change.

## Live health and USB interruption, 2026-10-13

After another installation the user supplied live logs with no BARO or SENSOR
alarm, `ManualControl=Critical` with the explicit `Settings` reason, GPS error,
and an intermittent EventSystem warning. Receiver settings still require setup;
the default GPS UART has not supplied GPS messages. EventSystem means a firmware
event queue or callback failed. Its cause cannot be identified from that alarm
alone. The health log now resolves the three SystemStats warning object IDs to
names while the warning is active; retained historical IDs do not make a cleared
alarm appear active.

The old sample diagnostics were incorrect: incoming UAVTalk objects marked
hardware presence but never set the received-data flag. Validated object frames
now set that flag, ACK/presence updates preserve it, and disconnect clears it.
Local settings edits and rejected wire frames do not count as telemetry. The
startup retriever also uses this flag to avoid requesting already received
single-instance objects and completes when its final entries can be skipped.
The absence of BARO/SENSOR alarms is consistent with restored acquisition, but
pressure and temperature still need to be read from this physical controller.

The live connection subsequently lost all telemetry and reported a HID write
failure. With the GCS closed, a correct-CRC native diagnostic probe independently
failed with `ETIMEDOUT` (errno 110). Resetting the host USB connection did not
restore communication. Neither probe changed firmware or saved settings.
Evidence is in `build/qt612-release/controller-inspection/20261013T123531.653397Z`
and `20261013T123717.115991Z`. The earlier `20261013T123140.845137Z` run used an
incorrect checksum and is not evidence of a controller failure.

HID worker failures previously left the QIODevice open after reading stopped or
the writer stalled. Both workers now report the operation and OS errno, stop,
and close the transport. Write statistics count complete reports actually sent;
short reports fail, transient retries reset after success and remain bounded.
Input lengths are validated before accessing their payloads. The main recovery
card retains failed communication and offers Retry connection; automatic retries
stop. An ordinary removed-device notification follows normal disconnection.
These behaviors are shared by all native HID controllers.

`python3 make/scripts/inspect-uavtalk.py` reads health, barometer and queue
diagnostics for 60 seconds, saves JSONL samples and a JSON report automatically,
and requires the GCS to be closed. `--reset-usb` resets only the host connection.
The remaining physical step is to remove all controller power, reconnect USB
with BOOT released, and run this probe before reopening the app. The EventSystem
warning and reason for the USB stall remain unresolved until those diagnostics
can be read; no firmware workaround has been applied without that evidence.

The GCS and Linux package were rebuilt. All 142 connection/controller checks,
three native inspection framing/schema checks and packaged application UI
checks passed. The USB failure card was inspected in the delivered theme.
Evidence: `build/qt612-release/ui-checks/native-tests.log`,
`build/qt612-release/ui-checks/uavtalk-inspection-tests.log` and
`build/qt612-release/ui-checks/ui-check-67dv2_i8/report.json`.
The no-argument `ground/tests/run_ui.py` entrypoint includes the new diagnostic
checks alongside its existing connection and packaged UI checks.

## Wiring and supported functions

| Function | Native assignment |
| --- | --- |
| Processor | STM32F405, 8 MHz HSE, 168 MHz system clock, 1 MiB flash, 128 KiB SRAM; CCM unused |
| Motion sensors | MPU6000, MPU6500 or ICM20608 on SPI1; PA4 CS, PC4 DRDY; 1 kHz samples |
| Mounting | Betaflight CW270; native `PIOS_MPU_TOP_180DEG` gives forward/right/down `(-rawY, -rawX, -rawZ)` |
| Barometer | SPI3 BMP280, PB3 CS |
| Analog OSD | SPI3 MAX7456, PA15 CS; shared SPI3 uses mode 0 and per-device clocks before CS |
| Motors 1–4 | PB0/TIM3 CH3, PB1/TIM3 CH4, PA3/TIM2 CH4, PA2/TIM2 CH3 |
| Receiver | UART6 by default, UART3 selectable; external inverter PC8/PC9 follows the selected port |
| GPS | UART3 by default; moves to UART6 when the receiver selects UART3 |
| External telemetry | UART1; USB HID telemetry remains available |
| ADC | Current PC1/channel 11, voltage PC2/channel 12, RSSI PA0/channel 0; native ADC1 DMA backend |
| Battery calibration | 3.3 V reference, 11:1 voltage divider (90.909 mV/V), 40 mV/A current factor; saved calibration preserved |
| Indicators | PB5 active-low LED; PB4 active-low buzzer |

Four PWM/OneShot outputs, GPS, telemetry, analog OSD and the native autotune
modules are built in. DShot, SD logging, extra motor outputs and software
serial are not implemented. The SD SPI2 interface remains disabled.

## Installation and settings

The GCS offers **Install DroniNG** for either reported target identifier. It
validates the build-matched image before touching the connection. For the
detected Betaflight 3.4.0 firmware, the installer reopens that serial connection,
verifies fresh API/firmware/board identity, discovers the ARM flag through
MSP_BOXIDS, checks fresh MSP_STATUS, enters CLI and waits for its prompt before
sending `bl`. The pinned CLI stops motor PWM and enters the STM32 bootloader.
MSP_REBOOT in this version is a normal restart and is not used for DFU entry.
Serial errors, an armed controller, invalid identity or a missing prompt stop
the automatic request and produce inline recovery instructions. Other firmware
versions use the board's BOOT procedure until their command behavior is reviewed.

| Region | Address | Size | Installer writes |
| --- | --- | --- | --- |
| Direct-boot vectors | `0x08000000` | 16 KiB | Yes |
| Settings | `0x08004000` | 32 KiB | No |
| Immutable board/profile identity | `0x0800c000` | 16 KiB | Yes |
| Autotune storage | `0x08010000` | 64 KiB | No |
| Application and descriptor | `0x08020000` | 896 KiB | Yes |

Board type/revision is `0xe1/1`; profile `omnibusf4sd`; layout
`omnibusf4sd-f405-rom-v1`. The installer requires the exact F405 erase geometry,
writes and reads back each of the three permitted regions, then reconnects on
the same USB path. Completion requires a fresh matching firmware descriptor,
live telemetry and the runtime BoardProfile schema/ID/layout/hash. The STM32
ROM bootloader is retained; no custom loader is installed. HEX records cover
only writable regions. BIN/TLFW build containers contain reserved gaps and
must not be used as whole-flash raw downloads.

The common `HwImported` settings object and profile-driven configuration page
provide receiver/GPS port selection and native apply/save/reboot controls.
The wizard and output configuration advertise four outputs and PWM/OneShot;
firmware rejects DShot settings and holds outputs in failsafe until corrected.

## Verification

Run from the repository root:

```sh
python3 flight/tests/run_omnibusf4sd.py
python3 flight/tests/run_radix.py
ground/tests/run_ui.py
```

The Omnibus entrypoint builds the actual ARM image, verifies reviewed upstream
inputs, exercises the complete production MAX7456 driver and unchanged MPU
functions under hardware substitutes, and checks actual ELF/HEX bounds and
compiled ARM startup instructions. Tests
cover mounting/scales, coherent sample delivery, shared-bus clock ordering,
missing devices, allocation/claim/clock/transfer errors, failed or discarded
register writes, bounded reset/clear/font/vsync waits and recovery. Evidence
is written to `build/qt612-release/omnibusf4sd-tests/report.json`.

The original production-driver and image-bound checks passed on 2026-10-11,
but did not check FPU initialization. The image that failed physical startup is
`cacfd4cff457ce2f4b51c4992bdec2c0b063471d825b851d74c6bb261f1b8177`,
with build-matched UAVObject SHA-1
`b5b028a7deefd880b55e6565ae75956df16b12b4`. All 105 installer cases and 114
connection/configuration cases passed. The rebuilt Linux package also passed
the no-argument UI entrypoint, including wide/narrow firmware layouts,
maintenance/log cards, sidebar controls, USB access, setup and replay checks.
Its map view became usable in 1.29 seconds in the isolated test environment;
this is not a measurement of live map-tile download performance.

GCS tests cover both firmware offers, native configuration and receiver/GPS
label changes, wizard output modes, software DFU entry and its rejection paths,
exact flash ranges, saved storage, stale/unsafe images, mismatched erase
geometry, write/readback failures and incorrect runtime profiles. The packaged
application checks include hardware isolation and rendered UI captures.
The no-argument UI entrypoint writes its own complete log to
`build/qt612-release/ui-checks/run-ui.log`; no shell redirection is required.

Physical acceptance remains a concrete bench task: cold startup and software
DFU on this controller; motion axes/sample timing; battery measurements;
receiver, four outputs, GPS, telemetry and video together; persistence and
interrupted-install recovery. Autotune and flight behavior then require flight
acceptance.

## Physical installation failure and recovery

The 2026-10-11 installation used Betaflight's software BOOT command; no physical
BOOT connection was required. Linux recorded `0483:df11`, product
`STM32 BOOTLOADER`, on USB path `3-1.4.3.4` at 19:55:19. It disconnected at
19:57:24 and did not enumerate again. Subsequent sysfs and a host-side
`dfu-util -d ,0483:df11 -l` probe found no controller. The original firmware was
Betaflight 3.4.0, target `OMNIBUSF4SD`.

The [Airbot Pro schematic](https://www.scribd.com/document/443405674/OmnibusF4-Pro-Sch)
labels D6 as a green supply LED and D5 as a blue STAT LED. Solid green on that
revision indicates supply power, not a firmware error code. Other Omnibus
revisions need their own schematic or physical identification.

ST's [factory bootloader](https://community.st.com/stm32-mcus-60/faq-stm32-boot-process-37)
is in ROM, separate from the application's writable flash. The documented
[Omnibus recovery procedure](https://docs.px4.io/v1.14/en/advanced_config/bootloader_update_from_betaflight#dfu-mode)
is to disconnect USB and the flight battery, hold the board's BOOT button while
connecting USB, then release BOOT after power-up. A successful recovery entry
enumerates as `0483:df11`; it does not create a serial port. Betaflight can then
be restored with its firmware flasher using the original `OMNIBUSF4SD` target.
Holding BOOT during USB power-up restored factory DFU on this controller at
20:32 on 2026-10-11: serial `208333984E48`, USB path `3-1.4.3.4`, and the expected
1 MiB F405 flash geometry. A read-only upload preserved all 1,048,576 bytes of
installed flash for diagnosis. Factory recovery entry is confirmed. The user
then authorized restoration of the original Betaflight firmware, which returned
successfully. The subsequent DroniNG attempt and IMU correction are recorded
below.

Readback evidence is retained in `build/qt612-release/omnibusf4sd-tests/`:

- `omnibusf4sd-installed-flash.bin`: SHA-256
  `2345e123caa93393a698638038fa1f06940150cadf31249ab44c6c25fdf60309`.
  Each writable region matches the shipped image byte for byte; the full-dump
  hash differs because it also includes the reserved settings/autotune regions.
- `omnibusf4sd-option-bytes.bin`: `f3 aa 0c 55 f3 aa 0c 55 ff ff 00 00 ff ff 00 00`.
  Read protection is level 0 (`0xaa`), and sector write protection is disabled.
- Official [Betaflight 3.4.0 release](https://github.com/betaflight/betaflight/releases/tag/3.4.0)
  asset `betaflight_3.4.0_OMNIBUSF4SD.hex`: 996,191 bytes, SHA-256
  `bb0d1b4e68c5dcee623aed0fbec64396edb387d612a97bc9843221ba7b3fbd93`.
  HEX checksums, target/version strings, nonoverlapping F405 flash bounds and
  Cortex-M4 vectors were checked. The decoded BIN was restored after explicit
  user approval. Its flash span is `0x08000000`–`0x0805b664`.

Restoration succeeded on 2026-10-11 (2026-10-12 UTC). All 374,372 restored bytes
were uploaded again and compared byte for byte with the official image before
leaving DFU. Readback is `betaflight-restored-readback.bin`, SHA-256
`a3e612d190c1fc3240742c3f75b66ca5095ab4222ff8339ef65b892f5b29e779`.
The controller returned on the same USB path as `0483:5740`, product
`OmnibusF4`, serial interface `/dev/ttyACM0`. Fresh read-only MSP responses
confirmed variant `BTFL`, version `3.4.0`, API `1.39`, board identifier `OBSD`
and target `OMNIBUSF4SD`. ROM and option bytes were not written. Betaflight's
configuration sector was already erased in the preserved failed-install dump;
the restoration uses default settings.

For repeatable recovery after another installation attempt, close DroniNG and
run this command from the repository root:

```sh
python3 make/scripts/restore-omnibusf4sd.py
```

The command waits for hardware BOOT mode using the procedure above, reuses the
same pinned official Betaflight 3.4.0 image and saves a full flash backup before
writing. It restores the complete 1 MiB flash with unused space erased, including
default settings, verifies every byte, then checks fresh MSP version/API/board
and the full `OMNIBUSF4SD` target name on the same USB port. This remains the
default restore mode. ROM and option bytes remain untouched. Concurrent recovery
commands are rejected, and ambiguous controllers require `--usb-path`.

`--prepare` validates and caches the image without flashing. `--check` only checks
the running firmware; use it after releasing BOOT and reconnecting if flash
verification succeeded but the startup check timed out. Console history is
appended to `build/firmware-restore/omnibusf4sd/run-restore.log`; each flash attempt
also retains `before.bin`, `restore.bin`, `readback.bin`, helper output and
`result.json` in a separate timestamped directory there. A successful write is
not reported as a completed restore until startup identity matches.

To exercise the full startup-failure recovery journey, close DroniNG and run:

```sh
python3 make/scripts/restore-omnibusf4sd.py --invalid
```

Use the same hardware BOOT procedure. This selects the preserved second failed
DroniNG image: two blue blinks, pause, then a reset before telemetry starts.
It reconstructs the image from the checksum-pinned `failed-imu-startup.elf` and
saved descriptor, matching the installed image checksum
`7007b1fb1cb974d7d574121d41c3983337aac460172e7e9ef267afae5ec7be63`.
The validated 1 MiB image is cached separately from Betaflight, so later runs
need neither the executable nor a download. Preparation fails if neither that
cache nor the preserved executable exists; it never substitutes a current build.
The command backs up the current board, verifies every written byte and confirms
two native `OMNIBUSF4SD` USB enumerations on the same port before reporting that
the reset loop was reproduced. A startup-check failure retains the verified-flash
result and gives a check-only command. Reports record the selected test state.

`--invalid --prepare` validates/caches this fixture without flashing;
`--invalid --check` checks its USB reset loop without flashing. After restoring
the invalid state, open DroniNG, use the main card's recovery action and reinstall
the bundled corrected firmware. Confirm that live panels populate, setup appears
once and the main card no longer offers an update to the just-installed image.
Repeat `--invalid` for another cycle; omit it to restore Betaflight instead.

The GCS now polls serial discovery while connected, clears firmware identity
when a transport disappears, reconnects using fresh USB-path observations,
and separates verified writes from successful startup. A startup timeout
produces an actionable inline state and retains the verified-install record
across app restarts. BOOT release instructions are conditional on using BOOT;
closing the view after verified writes no longer reports installation cancelled.

## Startup correction, 2026-10-12

The exact failed executable is preserved as `failed-startup.elf` beside the
flash readback. Its reset handler never enables CP10/CP11 in SCB CPACR, while
the compiled RTOS interrupt epilogue executes `vmrs`. `crt0.c` does not include
`chconf.h`, so its `#if CORTEX_USE_FPU` evaluated false despite the kernel's
FPU-enabled configuration. This is a concrete early-startup fault: an FP
instruction without coprocessor access causes a fault before USB can report it.
The [ARM Cortex-M4 manual](https://www.keil.com/dd/docs/datashts/arm/cortex_m4/r0p1/ddi0439c_cortex_m4_r0p1_trm.pdf)
documents the CP10/CP11 access controls required to use the FPU.
The board's fault PC was not captured, so hardware acceptance still determines
whether this accounts for the entire observed failure.

The F4 build now supplies the same FPU setting to reset and kernel code. Reset
sets CPACR and executes DSB/ISB before its first FP instruction and before
`__early_init`. Imported boards also set SCB VTOR to physical flash
`0x08000000`; the original value was zero, which depends on the mapping left
by the boot path. The revised interrupt setup works independently of ROM DFU's
address-zero mapping. Tests inspect the actual ARM executable for both fixes
and reject the preserved failed executable for both defects.

The FPU/vector-corrected candidate SHA-256 was
`7007b1fb1cb974d7d574121d41c3983337aac460172e7e9ef267afae5ec7be63`.
The production-driver, compiled startup and ELF/HEX checks passed. Its second
bench installation confirmed native USB enumeration but exposed the IMU startup
failure described below. The restore-only command above is prepared with the
verified official image if recovery is needed.

The Linux application delivered for that attempt contained that checksum. Its
114 connection/controller checks and packaged UI checks passed on 2026-10-12.
The shared startup changes also passed the Radix production-driver/image tests
and compiled FPU/vector checks. These checks did not access physical USB.

## IMU startup correction, 2026-10-12

The second installation verified all three regions and enumerated on the same
USB path `3-1.4.3.4` as `20a0:4250`, product `OMNIBUSF4SD`, with CDC and HID
interfaces. Linux reported serial `46004C000C50484E52333720+FW`. Repeated sysfs
observations showed disappearance and fresh enumeration roughly every four
seconds. The user reported two blue flashes followed by a pause. That is
`PIOS_HAL_PANIC_IMU` (code 2): the firmware repeats the blink pattern three
times and resets. USB activates before IMU startup; telemetry tasks cannot
start until board initialization returns, explaining the missing handshake.

The shared Omnibus board caller declared `pios_mpu_dev_t imu` without an
initializer. `PIOS_MPU_SPI_Init` allocates only when its incoming handle is null;
otherwise it treats the value as an existing device. The compiled failed caller
passed stack storage without initializing it. The caller now uses `imu = NULL`,
matching the other native targets. The failed source and executable are preserved
as `failed-imu-startup.c` and `failed-imu-startup.elf` beside the first failure.

The regression compiles the actual board caller and actual MPU SPI initializer
with stack variables initialized to a poison pattern. The preserved failed
caller is rejected; the corrected caller passes for MPU6000, MPU6500 and
ICM20608G, allocation/probe/IRQ errors, unsupported chips and configuration
failures. Existing MPU acquisition, MAX7456, compiled FPU/vector and ELF/HEX
checks also pass. These establish the caller fix; the subsequent live-startup
observation below supplies physical evidence for the corrected image.

The corrected candidate SHA-256 is
`4a9538934535a02fe656ce6aefbfd75d6fadfd3177be4a5df62cd6011b010e6a`.
The app now retains automatic connection history and stops retries after the
first transport loss before live telemetry. It shows an inline
startup error; closing the installation keeps the diagnosis. Connect or Start
controller begins a new attempt. The installer also stops after its startup
timeout and avoids switching to stale serial discovery after observing native
HID on that USB path. These paths are covered by native connection tests.
If USB remains open without telemetry, the startup timeout also clears detection
on the main page and preserves an explicit unconfirmed-startup status after
closing the installation view.

The delivered Linux package contains the corrected firmware checksum above and
the rebuilt app library. All 119 connection/controller checks and packaged UI
checks passed with USB hardware isolated. The rendered startup-reset state
shows the diagnosis above the retry controls. Evidence is in
`build/qt612-release/ui-checks/native-tests.log` and
`build/qt612-release/ui-checks/ui-check-swh1nn34/report.json`.

### Live startup observed on 2026-10-12

The user completed the corrected installation and reported responsive gyro and
attitude data. `screenshot-20261012-085826.png` shows DroniNG 1.0, populated
attitude/aircraft panels and live system health. The earlier reset loop no longer
describes the installed controller. Cold USB reconnect, sample timing and the
peripheral acceptance tasks above still require bench verification.

The screenshots also exposed dashboard issues: live panels were gated by Done,
setup appeared twice inside nested cards and every imported controller offered
an update. Installation now uses the main card directly and hands off to one
setup action as soon as matching firmware/profile and live data are confirmed.
Update offers compare the running descriptor with the validated bundled image.
The same workflow is exercised for RADIX, Omnibus and HGLRC; it is not specific
to this controller.

The recovery-test entrypoint's invalid fixture was prepared on 2026-10-12 with
the exact second-installation image checksum. Its regression checks use fake
DFU transfers, including backups, readback failures and post-flash reset-loop
verification. Adding this mode did not flash the physical controller; a bench
cycle remains the next check of the complete recovery journey.
