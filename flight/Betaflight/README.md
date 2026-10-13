# Betaflight hardware adaptation survey

Offline feasibility work performed on 2026-10-09. **The proposed boundary is
workable:** import board wiring at build time, port each chip family once, and
keep acquisition, scheduling and flight control in PiOS/droniNG. The register
logic of the untouched upstream ICM426xx driver compiled and passed a host
probe through the real PiOS SPI API declarations.

The survey and probe sources are outside firmware source selection. They do
not establish production board support. The separate
[Radix implementation](RADIX_IMPLEMENTATION.md) and
[Omnibus F4 SD implementation](OMNIBUSF4SD_IMPLEMENTATION.md) import their
pinned wiring and build native firmware images. No controller was
flashed or reconfigured. The C probe simulates hardware; it does not validate
STM32 SPI, interrupts, sample deadlines or flight behavior.

## Sources and reproducibility

The six definitions are representative familiar board families, selected to
exercise different integration cases, not a measured popularity ranking. A
target name can cover multiple PCB revisions and fitted parts. In particular,
`MAMBAF405_2022A` is a family fixture, not a claim that it is the newest Mamba.

Complete source snapshots are cached under `downloads/betaflight/`, outside the
tracked firmware tree. One archive is downloaded per pinned repository revision;
later imports and checks use those local snapshots without network access. Only
the reviewed inputs listed in the locks are copied into `upstream/` and selected
for builds. Downloading a repository does not enable its boards or drivers.

The survey source pins are:

| Repository | Commit | Scope |
| --- | --- | --- |
| [betaflight/config](https://github.com/betaflight/config/tree/e1d87e7ecf6717bd3806a08400aeb892cd1d1fc3) | `e1d87e7ecf6717bd3806a08400aeb892cd1d1fc3` | Six board definitions and license |
| [betaflight/betaflight](https://github.com/betaflight/betaflight/tree/4fc1520c5a5decddc8ef07ad57c0e766ea8747ba) | `4fc1520c5a5decddc8ef07ad57c0e766ea8747ba` | Chip drivers, dependency context, F4 timer/DMA tables, startup context and licenses |

These are survey snapshots, not a selected production release pair. The 84
downloaded files occupy 828,242 bytes. This is a selected source set rather than
a complete Betaflight build tree. MPU6000/6500, ICM426xx, BMI270, LSM6DSO/DSV,
BMP280/DPS310, NOR/NAND, MAX7456 and SD-card drivers are included for inspection.

[survey.lock.json](survey.lock.json) lists every selected remote file.
[source-manifest.json](source-manifest.json) records each file's Git blob hash,
SHA-256 and byte count. The downloader checks previously verified hashes against
the complete pinned snapshot before writing. It refuses to overwrite a locally
changed upstream file. Upstream code and copyright headers remain unmodified; both repository
GPL licenses and the Bosch BSD license are retained.

Run from the repository root:

```sh
# Fetch all existing pins once; requires network access and Python 3.
# Subsequent runs and additional source imports use the complete local cache.
python3 make/scripts/fetch-betaflight-survey.py --all

# Offline; requires Python 3 and a host C compiler.
python3 make/scripts/betaflight-desk-check.py
python3 flight/Betaflight/survey/test_desk_check.py
python3 make/scripts/betaflight-driver-probe.py
```

The probe also requires the host `size` utility. Compiler flags include
`-Wall -Wextra -Werror` and undefined-behavior checks that trap on failure;
the trap mode avoids requiring a separate sanitizer runtime. `--cc` selects a
different host compiler for either analysis script. Compiler and object-size
results describe this host, not ARM firmware.

## Board-definition dry run

The [audit script](../../make/scripts/betaflight-desk-check.py) used the host
C preprocessor on all six untouched definitions. It expanded aliases such as
`TIMER_PIN_MAP(..., MOTOR1_PIN, ...)`, evaluated the F405 timer table's
conditionals, and resolved all **40 physical timer/channel mappings** for the
four F405 targets. Upstream's
[timer IO documentation](upstream/betaflight/src/main/pg/timerio.c) and
[resolver](upstream/betaflight/src/main/drivers/timer_common.c) establish that
the occurrence number is one based, not a timer ID.

The [generated report](survey/desk-check.json) preserves every board macro,
buses, gyro CS/DRDY/alignment, motor pins, timer mappings, shared pin roles,
review items and a lexical driver dependency inventory. It marks every target
as non-installable. It deliberately does not claim to resolve native DMA
allocation, alternate-function programming, enabled-resource policy, memory
layout or the complete upstream build/default context.

| Definition | MCU and permitted IMUs | Gyro wiring | Result and additional work |
| --- | --- | --- | --- |
| [SpeedyBee F405 V4](upstream/config/configs/SPBE/SPEEDYBEEF405V4/config.h) | F405; ICM42688P | SPI1, PA4 CS, PC4 DRDY, CW90 | Existing processor family; new ICM port. DPS310 needs a port. Logging uses SPI SD, with no current PiOS storage integration. OSD SPI2 uses PC2/PC3 data pins. 12 timer mappings resolved. |
| [SpeedyBee F405 V5](upstream/config/configs/SPBE/SPEEDYBEEF405V5/config.h) | F405; ICM42688P | SPI1, PA4 CS, PC4 DRDY, CW270 | New ICM port plus explicit external gyro clock on PB15. That maps to TIM12 CH2 and needs a native clock implementation. DPS310 needs a port; storage is NOR. 11 mappings resolved. |
| [GEPRC F405](upstream/config/configs/GEPR/GEPRCF405/config.h) | F405; MPU6000, ICM42688P, BMI270 or LSM6DSK320X | SPI3, PA15 CS, PC3 DRDY, CW180 | Good first profile after selecting an exact fitted IMU/barometer. BMP280 and MAX7456 have native drivers; W25Q NOR needs exact identification/geometry review. Broad substitution support requires three new IMU families. 8 mappings resolved. |
| [Mamba F405 2022A](upstream/config/configs/DIAT/MAMBAF405_2022A/config.h) | F405; MPU6000, MPU6500 or BMI270 | SPI1, PA4 CS, PC4 DRDY, CW180 | Existing MPU support may be reused for an exact MPU revision. BMI270 and DPS310 need ports. ADC3 needs backend review; HSE is not explicit in this definition. 9 mappings resolved. |
| [iFlight Blitz F722](upstream/config/configs/IFRC/IFLIGHT_BLITZ_F722/config.h) | F722; MPU6000, ICM42688P or BMI270 | SPI1, PA4 CS, PC4 DRDY | Processor port required before board support. NOR/NAND alternatives must be distinguished. HSE not explicit; upstream gyro default is CW0. F4 tables were not applied to its 10 timer mappings. |
| [Kakute H7 V2](upstream/config/configs/HBRO/KAKUTEH7V2/config.h) | H743; BMI270 | SPI4, PE4 CS, PE1 DRDY | Processor port, BMI270 and optionally NAND logging. HSE not explicit; upstream gyro default is CW0. F4 tables were not applied to its 10 timer mappings. |

Important consequences for the importer:

- A definition listing four IMUs is a set of allowed substitutions, not a
  statement that all four are fitted. Select the exact hardware or implement
  every advertised alternative; the first profile can be deliberately narrower.
- Definitions contain alternatives that cannot be active together: GEPRC has
  motor 7 and LED strip on PB1; several targets share PPM and UART RX. GEPRC
  also assigns PINIO to PA13/PA14, which are SWD debug pins. Availability of a
  pin macro must not automatically enable that function.
- Timer occurrence and DMA-option integers need an MCU-specific translation.
  For example, PB1 occurrence 2 selects TIM3 CH4, while PB15 occurrence 3 selects
  TIM12 CH2. A valid timer assignment still does not establish conflict-free
  DShot DMA. The audit retains the requested DMA option without pretending it
  is a PiOS stream/channel identifier.
- Missing board macros do not mean Betaflight lacks defaults. The audit now
  evaluates the actual `GYRO_1_ALIGN` default block in
  [gyrodev.c](upstream/betaflight/src/main/pg/gyrodev.c), giving CW0 for Blitz and
  Kakute. Clock setup requires more context: the pinned
  [F4 startup](upstream/betaflight/src/platform/STM32/startup/system_stm32f4xx.c)
  reads a persistent HSE value and has an HSI path when it is zero. This survey
  does not infer the physical crystal from that behavior. Missing HSE fields
  remain explicit review items rather than guessing 8 MHz for every board.
- Mamba requests ADC3, but
  [the current ADC backend](../PiOS/STM32F4xx/pios_internal_adc.c) unconditionally
  enables the ADC1 peripheral clock. This is a specific backend concern, even
  though the processor family already exists.
- Betaflight settings such as current-meter scales, DShot mode, PID-loop
  denominator, UART function defaults and PINIO box IDs are not native hardware
  descriptors. Preserve them as source evidence and translate only deliberate
  droniNG equivalents with unit/resource review.

## Driver boundary and C probe

The [host probe](survey/icm42688_host.c) compiles the original
[ICM426xx source](upstream/betaflight/src/main/drivers/accgyro/accgyro_spi_icm426xx.c)
with its actual MPU enums/WHO_AM_I constants. The
[bounded mock header](survey/mock/survey_mock.h) supplies only the device fields
and services this chip source needs. The bridge calls the real declarations in
[pios_spi.h](../PiOS/inc/pios_spi.h); mock implementations emulate a banked register
device. It claims/releases the bus, sets clock rate and asserts/deasserts CS for
each transaction. A simple synchronous acquisition replacement decodes samples.

Both internal-clock and external-clock builds passed. Assertions checked:

- ICM42688 identification and rejection of an unknown identity;
- gyro/accelerometer callback selection and scale values;
- 8/4/2/1 kHz register settings and invalid-decimation fallback;
- banked anti-alias filter settings, DRDY register configuration and AFSR bits;
- modeled SPI divider selection of 21 MHz from an 84 MHz bus under a 24 MHz limit;
- positive/negative big-endian samples, gyro scaling and temperature conversion;
- bus-claim failure, transfer failure, unchanged samples after failed reads,
  and releasing CS/bus ownership on transfer failure;
- external-clock register settings and failed-clock fallback.

The test found two integration requirements rather than proving they already
work in firmware. `spiWriteReg()` and upstream initialization return `void`;
the probe latches bus errors, and a production PiOS initializer must reject a
failed initialization instead of declaring success. Clock setup failure still
returns a detected ICM42688 with internal clock selected. A profile requiring
CLKIN must decide explicitly whether that is acceptable, with verified hardware
timing; it must not silently claim the external-clock capability.

The compiled chip source still calls `mpuGyroInit`, `mpuGyroReadSPI` and
`mpuAccReadSPI`. The probe replaces these with a counter and synchronous reads.
It **does not compile the upstream shared MPU acquisition code**. That code
brings `spiSequence`, EXTI callbacks, DMA buffers/completion, cycle timing and
sampling-mode discovery. Importing it unchanged would substantially enlarge
the compatibility surface. This is the strongest reason to port register
initialization/detection and provide native PiOS acquisition.

The [probe result](survey/driver-probe.json) records compiler and host object
sizes: 1,974 text bytes for internal clock; 2,182 text bytes plus 8 BSS bytes
with external clock. These exclude the bridge and are not estimates of ARM
flash/RAM use. The board's real clock tree, transaction duration, ISR/RTOS
interaction, coherent frame delivery, timeouts, mounting rotation and startup
failure behavior still require a production port and bench tests.

The [seven offline regression checks](survey/test_desk_check.py) passed. They use
reviewed wiring/timer fixtures, check MCU rejection and specific substitutions
and resource conflicts, and ensure invalid occurrences, cyclic aliases and
modified source bytes fail rather than producing plausible-looking results.
An additional check verifies that empty feature definitions do not consume the
following macro; the survey was corrected and regenerated when this was found.

## Other downloaded drivers

| Chip/service | Local comparison and port implication |
| --- | --- |
| MPU6000/6500 | Existing [PiOS MPU driver](../PiOS/Common/pios_mpu.c) already covers these families. Reuse it with reviewed wiring/orientation; importing Betaflight's duplicate driver is unnecessary for initial support. |
| ICM42688P | The host probe demonstrates a small register-service boundary. Build a native PiOS driver around the reviewed detection/configuration logic, with explicit initialization errors and native sample delivery. |
| BMI270 | A dedicated port. This pin uses Bosch's **328-byte maximum-FIFO configuration blob**, not an assumed 8 KiB standard image. SPI reads include a dummy byte, samples are little endian, and startup toggles CS to enter SPI. Its own EXTI/DMA path and optional 6.4 kHz FIFO mode need replacement/review. The existing BMI160 driver is not a BMI270 implementation. |
| LSM6DSK320X | GEPRC's newest permitted alternative is implemented in `accgyro_spi_lsm6dsv16x.c`, not the older LSM6DSO file. It has its own sample rates/scales and segmented reads. Treat it as a separate reviewed chip-family port. |
| BMP280 | Existing native PiOS support; verify bus, probing and units for the exact profile. |
| DPS310 | New PiOS barometer driver. Preserve calibration/compensation; replace asynchronous generic-bus callbacks and singleton state with PiOS lifecycle/state. Betaflight returns Pa and centidegrees; native PiOS barometer data uses kPa and degrees C. |
| MAX7456 | Reuse existing native driver with imported SPI/CS wiring and reviewed power/control pins. |
| SPI NOR | Existing JEDEC driver is a useful backend, but exact part identification, capacity, erase geometry and filesystem allocation still need review. The name `USE_FLASH_M25P16` does not establish a particular fitted capacity. |
| W25N NAND | A storage project beyond a bus wrapper: page/block operations, ECC and bad-block management. The downloaded upstream driver implements those concerns. Existing PiOS JEDEC NOR is not a substitute. Optional logging can wait behind basic flight-controller support. |
| SPI SD | ChibiOS has bundled MMC infrastructure, but this fork has no PiOS SD backend/storage integration corresponding to SpeedyBee V4. Reuse would still need integration and validation. |
| F722/H743 MCU | New startup, clock, interrupt, RTOS, DMA/memory, USB and peripheral backends. H7 also needs its own cache/memory/DMA policy. Shared chip drivers alone cannot create processor support. |

## Changes to the implementation plan

The user's required capabilities are **OSD, telemetry and GPS** in addition to
basic flight-controller support. **Autotune is strongly preferred**, with its
buffer, CPU and tune-data storage requirements planned into reference builds.
Accepted imported profiles must include the OSD backend/modules, native USB
telemetry plus a documented external telemetry path, and a supported external
GPS UART/module. Validate receiver, GPS, telemetry and OSD together. The initial
OSD path is MAX7456-compatible analog video; digital OSD needs its own backend.
Autotune limitations must be explicit. GPS-dependent navigation also needs its
estimator/sensor prerequisites and flight validation.

All six surveyed definitions declare MAX7456 support and hardware UARTs, making
them candidates for these requirements. Exact fitted OSD hardware, simultaneous
resource allocation, build fit and runtime behavior remain unverified. The
generated desk check is a wiring survey, not a required-feature acceptance test.

Keep the existing HGLRC/Omnibus F405 baseline, and use the owner's BrainFPV
RADIX as the first imported-board implementation, per the 2026-10-11 decision
in [FIRMWARE_PLAN.md](../../FIRMWARE_PLAN.md). Radix is outside this six-board
survey: the [Radix dry run](RADIX_DRY_RUN.md) separately pins its manufacturer
sources, confirms F446 and exercises its unchanged FPGA driver through a host
PiOS bridge and Cortex-M4 compilation. Reuse native BMI160/BMP280 and adapt
the existing RE1 FPGA/video services before a
production firmware offer. An exact GEPRC F405 revision with ICM42688P follows
as the new IMU port and expansion proof. Verify the fitted parts before
declaring the broad `GEPRCF405` profile supported. SpeedyBee V4 is a useful
second ICM profile because it tests different mounting and optional SD logging.
SpeedyBee V5 should follow an explicit CLKIN port or a separately documented,
bench-tested internal-clock profile. An exact MPU-equipped Mamba is another
reuse candidate after clock and ADC review.

The [Radix implementation](RADIX_IMPLEMENTATION.md) now builds
profile-driven tables, shared F4 initialization and native BrainFPV FPGA/video
services into a real F446 image, with a segmented ROM-DFU GCS install path.
Bench/flight validation and broader importer support remain pending. The following ICM PiOS
port must deliver coherent samples into `PIOS_SENSORS` and measure behavior on
the GEPRC hardware.
Keep F7/H7 processor work, BMI270/LSM ports and optional NAND/SD logging as
separate increments. No general Betaflight scheduler/configuration emulator is
needed by the demonstrated register boundary.

The result supports the architecture in [FIRMWARE_PLAN.md](../../FIRMWARE_PLAN.md)
while narrowing the first implementation to evidence-backed chip/profile ports.
It does not establish that downloaded targets are ready to install.
