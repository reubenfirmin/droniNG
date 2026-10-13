# Original BrainFPV RADIX implementation dry run

Investigation date: 2026-10-11. The result supports implementing Radix through
the imported-board path in [FIRMWARE_PLAN.md](../../FIRMWARE_PLAN.md). It
also narrows the work: the original Radix is **STM32F446**, and native RE1
support already supplies much of its processor, sensor and video machinery.
A new FPGA design is not currently an identified requirement.

This is an offline source/adapter proof. It does not access the controller,
produce an installable Radix image, or establish hardware compatibility.
The subsequent [experimental implementation](RADIX_IMPLEMENTATION.md) produces
a real native image and GCS package; this document preserves the dry-run evidence.

## Reproduce

Run from the repository root, using the existing ARM toolchain in `tools/`
and host GCC with undefined-behavior sanitizer support:

```sh
python3 make/scripts/radix-dry-run.py
```

The entrypoint verifies all pinned sources, extracts the candidate wiring and
timer map, compiles/runs the unchanged FPGA driver against simulated PiOS
SPI/GPIO, and compiles a Cortex-M4 object. It needs no network or USB access.
Results are written to `build/radix-dry-run/report.json` with `installable: false`.
The mocks and prototype bridge under `survey/` are excluded from flight builds.

The separately tested native F446 comparison build is:

```sh
make -j8 fw_brainre1 BUILD_DIR="$PWD/build/qt612-release" CCACHE_BIN=
```

The dry-run report includes its sizes when that ELF is present. This remains
an RE1 firmware image and cannot be installed on Radix as a substitute.
Disabling ccache avoids its cache write outside the workspace.

The 35 unchanged manufacturer files, including their license, are recorded in
[radix.lock.json](radix.lock.json) and
[radix-source-manifest.json](radix-source-manifest.json). They come from
`BrainFPV/betaflight` commit `d966f1f3a12df8446a623110d753b8dd72ebeaa4`.
This is a selected investigation snapshot, not a complete upstream build or
an established production release pairing. If the snapshot needs restoring,
the existing fetcher supports the independent Radix lock/manifest:

```sh
python3 make/scripts/fetch-betaflight-survey.py --lock flight/Betaflight/radix.lock.json --manifest flight/Betaflight/radix-source-manifest.json
```

That restoration command requires network access. The offline check verifies
the exact file set, immutable repository pin, Git blob IDs and SHA-256 hashes.

## Results

| Check | Result | Scope |
| --- | --- | --- |
| Manufacturer source verification | 35 files verified | Target, FPGA/video hooks, BMI160, F446 timer/startup/linker inputs and license; selected dependencies only. |
| Processor/clock | STM32F446, 16 MHz HSE | `RADIX/target.mk` corrects the earlier plan's F405 assumption. Manufacturer linker declares 512 KiB flash and 128 KiB RAM; confirm the physical chip/revision. |
| Timer extraction | Eight mappings resolved | First four motors: PA2/TIM5 CH3, PA3/TIM5 CH4, PA10/TIM1 CH3, PA15/TIM2 CH1. Native AF/DMA allocation and waveforms remain unverified. |
| FPGA host proof | Passed with undefined-behavior sanitizer in trap mode | Actual PiOS SPI function declarations, simulated transactions/GPIO/time, register masks, initialization, serial inversion, buzzer/LED, OSD controls and bulk limits. |
| Startup/failure proof | Passed | Missing bus, CDONE timeout, failed claims/clocks/transfers, invalid revision, false initialization success and cached failed writes. |
| Cortex-M4 FPGA object | 956 bytes text, 0 data, 20 BSS | Unlinked unchanged manufacturer source using mock service declarations. Excludes bridge, video and runtime allocation. |
| Native RE1 baseline | Build passed | 324,572 bytes flash content, 69,760 bytes static RAM and 61,312-byte heap reservation. Includes native OSD/GPS/telemetry/autotune; runtime heap/stack margins were not measured. |

The host build preserves an upstream `uint8_t`/enum declaration mismatch,
using `-fshort-enums` and allowing only `enum-int-mismatch` under otherwise
fatal warnings. The compiler diagnostic is retained in the report. The host
compiler was GCC 15.3.1; the existing ARM compiler was GCC 6.3.1. This does not
replace correcting the declaration in a production adaptation.

## What can be reused

**FPGA configuration and OSD.** The pinned Radix FPGA registers match the native
[RE1 FPGA driver](../../targets/brainre1/re1fpga/fpga_drv.c). Its normal
manufacturer initialization does not define `BRAINFPV_FPGA_INCLUDE_BITSTREAM`:
it pulses CRESET, waits for CDONE, supplies the 16 MHz clock and resets the
running FPGA. This indicates reuse of its stored configuration rather than
generating or embedding a new FPGA bitstream. The optional embedded-bitstream
path contains a developer-specific file path and is outside this proof.
Actual configuration/revision and video output still need bench confirmation.

The native PiOS QuadSPI video backend already uses the same clock/data/sync
pins, DMA stream and pixel clock. The native OnScreenDisplay module supplies
the renderer and live flight/GPS content, so importing the complete Betaflight
OSD application is unnecessary. Adapt native initialization and Radix defaults:
the existing RE1 initializer writes the black register twice instead of
initializing white; Radix initializes black/white to 35/110 and sync threshold
to 30. Review Radix's spurious-VSYNC suppression/recovery for missing or noisy
video input. No PAL/NTSC output or interrupt timing was exercised here.

**Sensors.** Native PiOS BMI160 and BMP280 implementations exist. Enable the
BMP280 SPI path; the RE1 baseline currently uses its I2C path. Generate Radix's
SPI3 chip selects and BMI160 DRDY wiring. Verify alignment independently:
Betaflight `CW0_DEG` does not establish the equivalent PiOS orientation because
the native driver applies its own axis/sign transformation.

**Serial roles.** The proposed simultaneous configuration is receiver UART3,
GPS UART4, external telemetry UART1 and USB GCS. Reserve UART6 for receiver
telemetry when needed. The manufacturer
[hardware manual](https://www.brainfpv.com/knowledgebase/radix-hardware-manual/)
documents FPGA-controlled inversion on UART3 RX and UART6 TX. GPS on UART4
excludes ESC telemetry on RX4. GCS configuration must expose those choices and
connector names explicitly.

## Concrete implementation work

1. **Generate native board initialization.** Introduce the shared imported
   F405/F446 target and profile/schema. Generate Radix clock, GPIO, buses,
   interrupts, timers and serial/ADC tables. Confirm the physical PCB/MCU and
   establish production source/driver pins. Resolve AF/DMA conflicts before
   enabling four motor outputs; the extra PC8/PB0 outputs remain separate work.
2. **Handle the shared SPI3 bus correctly.** Radix puts BMI160, BMP280 and FPGA
   on one bus, unlike RE1's separate IMU bus. The native BMP280 changes the bus
   clock after asserting CS; BMI160 and the RE1 FPGA driver do not restore their
   clock on each claim. Set the device's clock/mode while owning the bus and
   before selecting it. Propagate claim/transfer failures, bound waits, and
   release CS/bus on every failure. The BMI160 register-read path also ignores
   a failed bus claim. Verify coherent samples while video controls and
   barometer reads run concurrently.
3. **Adapt FPGA/video services with explicit success checks.** Upstream FPGA
   writes return no transfer status, initialization can report success after
   failed writes, and the shadow cache can suppress a subsequent retry. Its
   revision getter never reads the chip. The host prototype exposes each case
   and rejects failed initialization using an error latch and explicit revision
   read; production must additionally update cached registers only after
   confirmed writes. Configure stored FPGA startup in the direct-boot firmware,
   since the RE1 path relies on its custom loader doing that first. Implement
   Radix video defaults, missing/noisy-input recovery and actual OSD validation.
4. **Define internal-flash storage and direct boot together.** Radix has an SD
   card, not RE1's external NOR. Use native internal flash/logfs for settings,
   with separate autotune storage; SD flight logging can follow independently.
   A candidate 512 KiB allocation is sector 0 for vectors/startup (16 KiB),
   sectors 1–2 for settings (32 KiB), sector 3 reserved for identity/recovery
   (16 KiB), sector 4 for autotune (64 KiB), and sectors 5–7 for application code
   (384 KiB). This is a layout proposal, not implemented allocation: generate
   exact sector descriptors, split linker regions and vectors at `0x08000000`,
   relocate legacy board metadata, bound autotune payload/headers and verify
   settings/update/recovery behavior. The RE1 linker currently declares 512 KiB
   starting at offset 128 KiB, beyond the physical end; its actual built image
   fits, but that region declaration must not be copied into the new layout.
5. **Complete image identity and the installation journey.** Generate the
   profile/layout descriptor and ROM-DFU package from the same authoritative
   layout; implement the common GCS board settings and exact catalog identity.
   Verify vectors, ranges, persistence, return-to-Betaflight recovery and
   postboot identity before offering an experimental image. Then test cold
   start, sensor axes/timing, receiver, four motor outputs, OSD, GPS, telemetry
   and autotune on the owned Radix before accepted support.

The host bulk-write prototype deliberately uses a 3,073-byte stack buffer; it
must not be copied into a flight thread with a 1 KiB stack. Production should
send header/data under one bus claim using bounded transfers and a measured
buffer budget. The object-size and RE1 comparisons are feasibility evidence,
not a Radix firmware-fit or real-time guarantee. No catalog support claim or
hardware installation changed during this investigation.
