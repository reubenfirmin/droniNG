#!/usr/bin/env python3
"""Generate the reviewed original F303 board's native wiring and direct-boot layout."""
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
import re


def generate(root, output, gpio, module):
    survey = root / "flight/Betaflight"
    audit = module("f3_audit", root / "make/scripts/betaflight-desk-check.py")
    dry = module("f3_dry", root / "make/scripts/spracingf3-dry-run.py")
    audit.verify_sources(survey / "spracingf3.lock.json", survey / "spracingf3-source-manifest.json")
    wiring = dry.profile(["cc"], audit)
    raw = (root / "shared/firmware/profiles/spracingf3.json").read_bytes()
    profile = json.loads(raw)
    layout = {"vectors": [0, 2048], "identity": [2048, 2048], "settings": [4096, 16384],
              "autotune": [20480, 16384], "application": [36864, 225280], "descriptorOffset": 262044}
    expected = {"id": "spracingf3", "target": "SPRACINGF3", "mcu": "STM32F303xC", "boardType": 226,
                "boardRevision": 1, "hseHz": 8000000, "sysclkHz": 72000000, "flashBytes": 262144,
                "ramBytes": 40960, "ccmBytes": 8192, "motorCount": 8,
                "imuOrientation": wiring["imu"]["nativeOrientation"], "layout": layout,
                "layoutId": "spracingf3-f303-uart-v1"}
    if any(profile.get(k) != v for k, v in expected.items()):
        raise ValueError("Original F3 profile requires explicit backend/layout review")
    board = (root / "flight/targets/spracingf3/board-info/board-info.mk").read_text()
    constants = {k: int(v, 0) for k, v in re.findall(r"^([A-Z_]+)\s*:=\s*(0x[\da-fA-F]+|\d+)\b", board, re.M)}
    for k, v in {"BOARD_TYPE": 226, "BOARD_REVISION": 1, "FW_BANK_BASE": 0x08000000,
                 "FW_BANK_SIZE": 262144, "FW_DESC_SIZE": 100, "EE_BANK_BASE": 0x08001000,
                 "EE_BANK_SIZE": 16384, "OSCILLATOR_FREQ": 8000000, "SYSCLK_FREQ": 72000000}.items():
        if constants.get(k) != v:
            raise ValueError(f"Original F3 board registration disagrees: {k}")
    digest = hashlib.sha256(raw + (survey / "spracingf3-source-manifest.json").read_bytes()).digest()
    lines = ["/* Generated pinned SPRACINGF3 wiring; SPDX-License-Identifier: GPL-3.0-or-later */", "#pragma once",
             '#define IMPORTED_PROFILE_ID "spracingf3"', '#define IMPORTED_LAYOUT_ID "spracingf3-f303-uart-v1"',
             "#define IMPORTED_PROFILE_HASH {" + ", ".join(f"0x{b:02x}" for b in digest) + "}",
             "#define IMPORTED_IMU_ORIENTATION " + wiring["imu"]["nativeOrientation"]]
    motors = [f"{{ .timer = {m['timer']}, .timer_chan = TIM_Channel_{m['channel'][2:]}, "
              f".remap = {m['alternateFunction']}, .pin = {gpio(m['pin'], pull='GPIO_PuPd_DOWN')} }}"
              for m in wiring["motorOutputs"]]
    lines.append("#define IMPORTED_MOTOR_CHANNELS {" + ", ".join(motors) + "}")
    for uart, pins in wiring["serial"].items():
        for role, pin in pins.items():
            lines.append(f"#define IMPORTED_{uart}_{role.upper()} " + gpio(pin, pull="GPIO_PuPd_UP"))
    for role in ("scl", "sda"):
        lines.append(f"#define IMPORTED_I2C_{role.upper()} " + gpio(wiring["imu"][role], output="GPIO_OType_OD"))
    memory = """/* Original F303: separate vectors/identity, settings and tune pages. */
__main_stack_size__ = 0x400;
__process_stack_size__ = 0x400;
MEMORY {
    vectors (rx) : ORIGIN = 0x08000000, LENGTH = 2k
    bd_info (r) : ORIGIN = 0x08000800, LENGTH = 2k
    flash (rx) : ORIGIN = 0x08009000, LENGTH = 220k - 100
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 40k
    ccmram (rwx) : ORIGIN = 0x10000000, LENGTH = 8k
}
"""
    sections = (root / "flight/PiOS/STM32F30x/sections_chibios.ld").read_text()
    sections = sections.replace("} > flash", "} > vectors", 1)
    sections = sections.replace("KEEP(*(.boardinfo))", "KEEP(*(.boardinfo))\n        KEEP(*(.boardprofile))")
    sections += '\nASSERT(_eheap - _sheap >= 24k, "F3 runtime heap below 24 KiB")\n'
    output.mkdir(parents=True, exist_ok=True)
    (output / "imported_profile.h").write_text("\n".join(lines) + "\n")
    (output / "memory.ld").write_text(memory)
    (output / "sections.ld").write_text(sections)
    (output / "import-report.json").write_text(json.dumps({"schemaVersion": 1, "profile": profile,
        "profileSha256": digest.hex(), "wiring": wiring, "installable": False}, indent=2) + "\n")
    print("Generated original SP Racing F3: eight outputs, I2C MPU6050, UART1 telemetry, direct boot")
