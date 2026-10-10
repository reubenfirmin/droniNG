# Flight controller support and code size

Source inventory of the current checkout, taken on 2026-10-08.

The codebase has **14 embedded flight-controller targets: eight STM32F4 and six
STM32F3 ports**. There are also targets for Linux/FlyingPi flight control, a
FlyingPi I/O expander, an OSD board, and a radio modem.

Support here means an implemented firmware target and corresponding source code.
This analysis did not rebuild every target or test hardware.

## Embedded flight controllers

| Flight controller / target | MCU | Firmware LOC | GCS LOC |
|---|---|---:|---:|
| [AeroQuad32 — aq32](flight/targets/aq32) | STM32F407 | 2,025 | 259 |
| [BrainFPV — brain](flight/targets/brain) | STM32F405 | 2,233 | 319 |
| [BrainFPV RE1 — brainre1](flight/targets/brainre1) | STM32F446 | 2,552 | 377 |
| [DTFAir DTFc — dtfc](flight/targets/dtfc) | STM32F303 | 1,196 | 287 |
| [Lumenier Lux V1 — lux](flight/targets/lux) | STM32F303 | 1,229 | 286 |
| [Matek F405 — matek405](flight/targets/matek405) | STM32F405 | 1,991 | 183 |
| [Omnibus F3 — omnibusf3](flight/targets/omnibusf3) | STM32F303 | 1,273 | 242 |
| [PikoBLX — pikoblx](flight/targets/pikoblx) | STM32F303 | 1,207 | 243 |
| [Quantec Quanton — quanton](flight/targets/quanton) | STM32F405 | 3,001 | 334 |
| [OpenPilot Revolution / Mini — revolution](flight/targets/revolution) | STM32F405 | 2,614 | 285 |
| [DTFAir Seppuku — seppuku](flight/targets/seppuku) | STM32F405 | 2,053 | 400 |
| [Tau Labs Sparky — sparky](flight/targets/sparky) | STM32F303 | 1,497 | 244 |
| [Tau Labs Sparky2 — sparky2](flight/targets/sparky2) | STM32F405 | 2,218 | 262 |
| [SP Racing F3 EVO / FrSky XSRF3E — sprf3e](flight/targets/sprf3e) | STM32F303 | 1,403 | 283 |
| **Total** | | **26,492** | **4,004** |

The embedded flight-controller ports account for approximately **30,500 source
lines**, including their GCS board code, before shared firmware dependencies.

## Compatibility and variants

The [README](README.md#full-support) documents several variants that share targets:

- DTFc also supports Colibri Race / TBS PowerCube.
- Revolution supports Mini and additional race variants.
- PikoBLX and SP Racing F3 EVO support compatible clones.
- Lux support is specifically V1.
- SP Racing F3 support is specifically the EVO variant. The
  [GCS board description](ground/gcs/src/plugins/boards_dronin/sprf3e.cpp) also
  names FrSky XSRF3E.
- Older TBS Colibri / Gemini support is listed as limited and uses the Quanton
  target.

Matek405 and OmnibusF3 have registered firmware targets and GCS board classes,
although the README's controller list does not include them. The buildable
embedded targets are registered in each board's `target-defs.mk`; the root
[Makefile](Makefile) collects those definitions.

## Other targets

| Target | Purpose | Firmware LOC | GCS LOC |
|---|---|---:|---:|
| [flightd](flight/targets/flightd) | Host simulation and Linux/FlyingPi flight control | 188 | 134 |
| [flyingpio](flight/targets/flyingpio) | FlyingPi STM32F030 I/O expander | 705 | — |
| [playuavosd](flight/targets/playuavosd) | PlayUAV OSD | 1,208 | 72 |
| [pipxtreme](flight/targets/pipxtreme) | PipXtreme radio modem | 1,619 | 76 |

The `flightd` count covers its target directory. Its Linux platform and hardware
support extends into [PiOS/posix](flight/PiOS/posix), including the FlyingPi I/O
driver. FlyingPIO is the I/O expander firmware; flight control runs through the
Linux `flightd` path.

## Counting method and scope

These are approximate physical source-line counts, excluding comments and blank
lines. Preprocessor directives and static hardware configuration tables count as
code.

- **Firmware LOC:** `.c` and `.h` files under the corresponding
  `flight/targets/<target>` directory. This includes board initialization, pin and
  peripheral definitions, STM32 startup code, board bootloader code, and the RE1
  FPGA driver where present. Splash-image headers are excluded.
- **GCS LOC:** the corresponding board classes and configuration classes in
  `ground/gcs/src/plugins/boards_*`, including `.cpp` and `.h` files. The
  `pipxtreme` target maps to the `taulink` classes, and `flightd` maps to the
  `simulation` classes. Shared plugin registration code is excluded.
- Build files, linker scripts, XML settings definitions, Qt UI definitions,
  images, and generated outputs are excluded.
- Shared firmware and libraries are excluded from the per-board counts.
  Duplicated startup code inside different target directories is counted for
  each target.

## Shared firmware

Most of the firmware is shared. Board-specific code primarily defines pins,
peripherals, hardware configuration, and startup. Stabilization, attitude
estimation, navigation, autotune, sensor drivers, RTOS code, and common bootloader
logic live elsewhere, principally in:

- [flight/Modules](flight/Modules)
- [flight/Libraries](flight/Libraries)
- [flight/PiOS](flight/PiOS)
- [flight/targets/bl](flight/targets/bl)
- [flight/targets/bu](flight/targets/bu)

The table therefore measures the source volume local to each board port. The
complete firmware for a board additionally uses shared modules and libraries;
the counts are not compiled firmware sizes.
