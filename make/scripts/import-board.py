#!/usr/bin/env python3
"""Generate native PiOS wiring and a bounded ROM-only layout from pinned board inputs."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def gpio(pin, mode="GPIO_Mode_AF", pull="GPIO_PuPd_NOPULL", output="GPIO_OType_PP"):
    match = re.fullmatch(r"P([ABC])(\d{1,2})", pin)
    if not match or int(match[2]) > 15:
        raise ValueError(f"Invalid/unsupported GPIO: {pin}")
    port, number = match.groups()
    return (f"{{ .gpio = GPIO{port}, .init = {{ .GPIO_Pin = GPIO_Pin_{number}, "
            f".GPIO_Mode = {mode}, .GPIO_Speed = GPIO_Speed_50MHz, "
            f".GPIO_OType = {output}, .GPIO_PuPd = {pull} }}, "
            f".pin_source = GPIO_PinSource{number} }}")


def generate(profile_id, output):
    if profile_id == "omnibusf4sd":
        importer = module("omnibus_import", ROOT / "make/scripts/omnibusf4sd-import.py")
        audit = module("desk_check", ROOT / "make/scripts/betaflight-desk-check.py")
        return importer.generate(ROOT, output, audit, gpio)
    if profile_id != "radix":
        raise ValueError("No reviewed importer/backend for this profile")
    raw = (ROOT / "shared/firmware/profiles/radix.json").read_bytes()
    profile = json.loads(raw)
    audit = module("desk_check", ROOT / "make/scripts/betaflight-desk-check.py")
    radix = module("radix_proof", ROOT / "make/scripts/radix-dry-run.py")
    lock, manifest = audit.verify_sources(ROOT / profile["sourceLock"], radix.SURVEY / "radix-source-manifest.json")
    candidate = radix.profile(["cc"], audit)
    for key, value in {"schemaVersion": 1, "id": "radix", "target": candidate["target"],
                       "mcu": candidate["mcu"], "hseHz": candidate["hse_hz"],
                       "flashBytes": candidate["flash_bytes"], "ramBytes": candidate["ram_bytes"],
                       "sysclkHz": 180000000, "motorCount": 4, "boardType": 224, "boardRevision": 1,
                       "imuOrientation": "PIOS_BMI160_TOP_0DEG", "layoutId": "radix-f446-rom-v1",
                       "supportLevel": "experimental"}.items():
        if profile.get(key) != value:
            raise ValueError(f"Profile requires explicit backend review: {key}")
    layout = {"vectors": [0, 16384], "settings": [16384, 32768], "identity": [49152, 16384],
              "autotune": [65536, 65536], "application": [131072, 393216], "descriptorOffset": 524188}
    if profile["layout"] != layout:
        raise ValueError("Layout requires explicit firmware/installer geometry review")
    info = (ROOT / "flight/targets/radix/board-info/board-info.mk").read_text()
    constants = {k: int(v, 0) for k, v in re.findall(r"^([A-Z_]+)\s*:=\s*(0x[0-9A-Fa-f]+|\d+)\b", info, re.M)}
    for key, value in {"BOARD_TYPE": 224, "BOARD_REVISION": 1, "BOOTLOADER_VERSION": 0,
                       "FW_BANK_BASE": 0x08000000, "FW_BANK_SIZE": 524288, "FW_DESC_SIZE": 100,
                       "EF_BANK_BASE": 0x08000000, "EF_BANK_SIZE": 524288,
                       "EE_BANK_BASE": 0x08004000, "EE_BANK_SIZE": 32768,
                       "OSCILLATOR_FREQ": 16000000, "SYSCLK_FREQ": 180000000}.items():
        if constants.get(key) != value:
            raise ValueError(f"Board registration/layout disagrees: {key}")
    digest = hashlib.sha256(raw + (radix.SURVEY / "radix-source-manifest.json").read_bytes()).digest()
    w = candidate["wiring"]
    macros = audit.parse_defines(audit.preprocess(["cc"], f'#include "{radix.TARGET / "target.h"}"\n', macros=True))
    adc = {key: int(macros[key]) for key in ("ADC_VOLTAGE_REFERENCE_MV", "DEFAULT_VOLTAGE_METER_SCALE", "DEFAULT_CURRENT_METER_SCALE")}
    if adc != {"ADC_VOLTAGE_REFERENCE_MV": 3245, "DEFAULT_VOLTAGE_METER_SCALE": 120, "DEFAULT_CURRENT_METER_SCALE": 200}:
        raise ValueError("ADC calibration requires explicit backend review")
    fixed = {"GYRO_1_EXTI_PIN": "PC13", "VIDEO_QSPI_CLOCK_PIN": "PB2", "VIDEO_QSPI_IO0_PIN": "PC9",
             "VIDEO_QSPI_IO1_PIN": "PC10", "VIDEO_VSYNC": "PB5", "VIDEO_HSYNC": "PC2",
             "ADC_VBAT_PIN": "PC1", "ADC_CURR_PIN": "PC0", "ADC_RSSI_PIN": "PC3"}
    if any(w[k] != v for k, v in fixed.items()) or candidate["serial"] != {
            "1": {"tx": "PB6", "rx": "PB7"}, "3": {"tx": "PB10", "rx": "PC5"},
            "4": {"tx": "PA0", "rx": "PA1"}, "6": {"tx": "PC6", "rx": "PC7"}}:
        raise ValueError("Shared peripheral/EXTI definitions require review")
    lines = ["/* Generated from pinned manufacturer wiring; SPDX-License-Identifier: GPL-3.0-or-later */", "#pragma once",
             '#define IMPORTED_PROFILE_ID "radix"', '#define IMPORTED_LAYOUT_ID "radix-f446-rom-v1"',
             "#define IMPORTED_PROFILE_HASH {" + ", ".join(f"0x{b:02x}" for b in digest) + "}"]
    for name, key, mode in (("SCK", "SPI3_SCK_PIN", "GPIO_Mode_AF"), ("MISO", "SPI3_SDI_PIN", "GPIO_Mode_AF"),
                            ("MOSI", "SPI3_SDO_PIN", "GPIO_Mode_AF"), ("IMU_CS", "GYRO_1_CS_PIN", "GPIO_Mode_OUT"),
                            ("BARO_CS", "BARO_CS_PIN", "GPIO_Mode_OUT"), ("FPGA_CS", "BRAINFPVFPGA_CS_PIN", "GPIO_Mode_OUT"),
                            ("FPGA_CLOCK", "BRAINFPVFPGA_CLOCK_PIN", "GPIO_Mode_AF"),
                            ("FPGA_RESET", "BRAINFPVFPGA_RESET_PIN", "GPIO_Mode_OUT"),
                            ("FPGA_CDONE", "BRAINFPVFPGA_CDONE_PIN", "GPIO_Mode_IN"),
                            ("FPGA_CRESET", "BRAINFPVFPGA_CRESET_PIN", "GPIO_Mode_OUT")):
        lines.append(f"#define IMPORTED_{name} {gpio(w[key], mode, 'GPIO_PuPd_UP' if name.endswith('CS') else 'GPIO_PuPd_NOPULL', 'GPIO_OType_OD' if name == 'FPGA_CRESET' else 'GPIO_OType_PP')}")
    motors = [f"{{ .timer = {m['timer']}, .timer_chan = TIM_Channel_{m['channel'][2:]}, "
              f".remap = GPIO_AF_{m['timer']}, .pin = {gpio(m['pin'], pull='GPIO_PuPd_DOWN')} }}"
              for m in candidate["timer_mappings"][:4]]
    lines += ["#define IMPORTED_MOTOR_CHANNELS { " + ", ".join(motors) + " }",
              "#define IMPORTED_IMU_ORIENTATION " + profile["imuOrientation"],
              f'#define IMPORTED_ADC_VREF ({adc["ADC_VOLTAGE_REFERENCE_MV"]} / 1000.0f)',
              f'#define IMPORTED_ADC_VOLTAGE_FACTOR (10000.0f / {adc["DEFAULT_VOLTAGE_METER_SCALE"]})',
              f'#define IMPORTED_ADC_CURRENT_FACTOR ({adc["DEFAULT_CURRENT_METER_SCALE"]} / 10.0f)']
    memory = """/* Generated original RADIX ROM-only layout. */
__main_stack_size__ = 0x0400;
__process_stack_size__ = 0x0800;
MEMORY {
    vectors (rx) : ORIGIN = 0x08000000, LENGTH = 16k
    bd_info (r) : ORIGIN = 0x0800c000, LENGTH = 16k
    flash (rx) : ORIGIN = 0x08020000, LENGTH = 384k - 100
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 128k
}
"""
    output.mkdir(parents=True, exist_ok=True)
    (output / "imported_profile.h").write_text("\n".join(lines) + "\n")
    (output / "imported_memory.ld").write_text(memory)
    report = {"schemaVersion": 1, "profile": profile, "profileSha256": digest.hex(),
              "sources": lock["repositories"], "wiring": candidate, "adcCalibration": adc, "installable": False,
              "scope": "generated native wiring; package validation still required"}
    (output / "import-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Generated radix: F446, four motor outputs, ROM layout; hardware validation pending")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.profile, args.output)
