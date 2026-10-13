#!/usr/bin/env python3
"""Reviewed OMNIBUSF4SD translation from the pinned Betaflight 3.4.0 target."""
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
import re


def generate(root, output, audit, gpio):
    source = root / "flight/Betaflight/upstream/betaflight_3_4"
    target = source / "src/main/target/OMNIBUSF4"
    raw = (root / "shared/firmware/profiles/omnibusf4sd.json").read_bytes()
    profile = json.loads(raw)
    manifest_path = root / "flight/Betaflight/omnibusf4sd-source-manifest.json"
    lock, _ = audit.verify_sources(root / profile["sourceLock"], manifest_path)
    expected = {"schemaVersion": 1, "id": "omnibusf4sd", "target": "OMNIBUSF4SD",
                "mcu": "STM32F405xx", "hseHz": 8000000, "sysclkHz": 168000000,
                "flashBytes": 1048576, "ramBytes": 131072, "boardType": 225, "boardRevision": 1,
                "motorCount": 4, "imuOrientation": "PIOS_MPU_TOP_180DEG",
                "layoutId": "omnibusf4sd-f405-rom-v1", "supportLevel": "experimental"}
    for key, value in expected.items():
        if profile.get(key) != value:
            raise ValueError(f"OMNIBUSF4SD profile requires review: {key}")
    layout = {"vectors": [0, 16384], "settings": [16384, 32768], "identity": [49152, 16384],
              "autotune": [65536, 65536], "application": [131072, 917504], "descriptorOffset": 1048476}
    if profile["layout"] != layout:
        raise ValueError("OMNIBUSF4SD layout requires review")
    prefix = f'#define OMNIBUSF4SD 1\n#include "{target / "target.h"}"\n'
    macros = audit.parse_defines(audit.preprocess(["cc"], prefix, macros=True))
    wiring = {"TARGET_BOARD_IDENTIFIER": '"OBSD"', "MPU6000_CS_PIN": "PA4", "MPU6000_SPI_INSTANCE": "SPI1",
              "MPU6500_CS_PIN": "PA4", "MPU_INT_EXTI": "PC4", "GYRO_MPU6000_ALIGN": "CW270_DEG",
              "ACC_MPU6000_ALIGN": "CW270_DEG", "GYRO_MPU6500_ALIGN": "CW270_DEG",
              "ACC_MPU6500_ALIGN": "CW270_DEG", "SPI3_SCK_PIN": "PC10", "SPI3_MISO_PIN": "PC11",
              "SPI3_MOSI_PIN": "PC12", "MAX7456_SPI_CS_PIN": "PA15", "MAX7456_SPI_INSTANCE": "SPI3",
              "BMP280_CS_PIN": "PB3", "BMP280_SPI_INSTANCE": "SPI3", "LED0_PIN": "PB5", "BEEPER_PIN": "PB4",
              "UART1_TX_PIN": "PA9", "UART1_RX_PIN": "PA10", "UART3_TX_PIN": "PB10", "UART3_RX_PIN": "PB11",
              "UART6_TX_PIN": "PC6", "UART6_RX_PIN": "PC7", "INVERTER_PIN_UART6": "PC8", "INVERTER_PIN_UART3": "PC9",
              "CURRENT_METER_ADC_PIN": "PC1", "VBAT_ADC_PIN": "PC2", "RSSI_ADC_PIN": "PA0", "USB_DETECT_PIN": "PC5",
              "SPI2_NSS_PIN": "PB12", "SPI2_SCK_PIN": "PB13", "SPI2_MISO_PIN": "PB14", "SPI2_MOSI_PIN": "PB15",
              "SDCARD_DETECT_PIN": "PB7"}
    for key, value in wiring.items():
        if audit.resolve(macros, key) != value:
            raise ValueError(f"OMNIBUSF4SD wiring requires review: {key}")
    for flag in ("USE_ACC_SPI_MPU6000", "USE_GYRO_SPI_MPU6500", "USE_BARO_SPI_BMP280", "BEEPER_INVERTED"):
        if flag not in macros:
            raise ValueError(f"Missing required target capability: {flag}")
    if "F405_TARGETS" not in (target / "target.mk").read_text() or "HSE_VALUE       ?= 8000000" not in (source / "Makefile").read_text():
        raise ValueError("Processor/clock build context changed")
    linker = (source / "src/main/target/link/stm32_flash_f405.ld").read_text()
    if "1024KByte FLASH, 128KByte RAM 64KByte CCM" not in linker:
        raise ValueError("Processor memory context changed")
    body = (target / "target.c").read_text()
    body = "\n".join(line for line in body.splitlines() if not line.startswith("#include"))
    expanded = audit.preprocess(["cc"], prefix + body)
    motors = re.findall(r"DEF_TIM\(\s*(TIM\d+),\s*(CH\d+),\s*(P[A-C]\d+),\s*TIM_USE_MOTOR\s*,\s*0,\s*(\d+)\)", expanded)
    if motors[:4] != [("TIM3", "CH3", "PB0", "0"), ("TIM3", "CH4", "PB1", "0"),
                      ("TIM2", "CH4", "PA3", "1"), ("TIM2", "CH3", "PA2", "0")]:
        raise ValueError("Motor timer mapping requires review")
    audit.verify_sources()  # Verify the shared F405 AF table before using it.
    timer_table = audit.f405_timers(["cc"])
    for timer, channel, pin, _ in motors[:4]:
        if {"timer": timer, "channel": channel} not in timer_table[pin]:
            raise ValueError(f"Invalid F405 timer alternate function: {pin}")
    info = (root / "flight/targets/omnibusf4sd/board-info/board-info.mk").read_text()
    constants = {k: int(v, 0) for k, v in re.findall(r"^([A-Z_]+)\s*:=\s*(0x[0-9A-Fa-f]+|\d+)\b", info, re.M)}
    for key, value in {"BOARD_TYPE": 225, "BOARD_REVISION": 1, "BOOTLOADER_VERSION": 0, "FW_BANK_BASE": 0x08000000,
                       "EF_BANK_BASE": 0x08000000, "FW_BANK_SIZE": 1048576, "EF_BANK_SIZE": 1048576,
                       "FW_DESC_SIZE": 100, "EE_BANK_BASE": 0x08004000, "EE_BANK_SIZE": 32768,
                       "OSCILLATOR_FREQ": 8000000, "SYSCLK_FREQ": 168000000}.items():
        if constants.get(key) != value:
            raise ValueError(f"Board registration disagrees: {key}")
    digest = hashlib.sha256(raw + manifest_path.read_bytes()).digest()
    for path, key, value in (("voltage.c", "VBAT_SCALE_DEFAULT", 110), ("current.c", "CURRENT_METER_SCALE_DEFAULT", 400)):
        text = (source / "src/main/sensors" / path).read_text()
        if not re.search(r"#define " + key + r"\s+" + str(value) + r"\b", text) or key in macros:
            raise ValueError("Battery calibration context requires review")
    lines = ["/* Generated from pinned Betaflight wiring; SPDX-License-Identifier: GPL-3.0-or-later */", "#pragma once",
             '#define IMPORTED_PROFILE_ID "omnibusf4sd"', '#define IMPORTED_LAYOUT_ID "omnibusf4sd-f405-rom-v1"',
             "#define IMPORTED_PROFILE_HASH {" + ", ".join(f"0x{b:02x}" for b in digest) + "}"]
    for name, pin, mode in (("IMU_SCK", "PA5", "AF"), ("IMU_MISO", "PA6", "AF"), ("IMU_MOSI", "PA7", "AF"),
                           ("IMU_CS", "PA4", "OUT"), ("OSD_SCK", "PC10", "AF"), ("OSD_MISO", "PC11", "AF"),
                           ("OSD_MOSI", "PC12", "AF"), ("OSD_CS", "PA15", "OUT"), ("BARO_CS", "PB3", "OUT")):
        lines.append(f"#define IMPORTED_{name} {gpio(pin, 'GPIO_Mode_' + mode, 'GPIO_PuPd_UP')}")
    channels = [f"{{ .timer = {timer}, .timer_chan = TIM_Channel_{channel[2:]}, .remap = GPIO_AF_{timer}, "
                f".pin = {gpio(pin, pull='GPIO_PuPd_DOWN')} }}" for timer, channel, pin, _ in motors[:4]]
    lines += ["#define IMPORTED_MOTOR_CHANNELS { " + ", ".join(channels) + " }",
              "#define IMPORTED_IMU_ORIENTATION PIOS_MPU_TOP_180DEG", "#define IMPORTED_ADC_VREF 3.3f",
              "#define IMPORTED_ADC_VOLTAGE_FACTOR (1000.0f / 11.0f)", "#define IMPORTED_ADC_CURRENT_FACTOR 40.0f"]
    output.mkdir(parents=True, exist_ok=True)
    (output / "imported_profile.h").write_text("\n".join(lines) + "\n")
    (output / "imported_memory.ld").write_text("""/* Generated OMNIBUSF4SD ROM-only layout; CCM deliberately unused. */
__main_stack_size__ = 0x0400;
__process_stack_size__ = 0x0800;
MEMORY {
    vectors (rx) : ORIGIN = 0x08000000, LENGTH = 16k
    bd_info (r) : ORIGIN = 0x0800c000, LENGTH = 16k
    flash (rx) : ORIGIN = 0x08020000, LENGTH = 896k - 100
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 128k
}
""")
    report = {"schemaVersion": 1, "profile": profile, "profileSha256": digest.hex(), "sources": lock["repositories"],
              "wiring": wiring, "motors": motors[:4], "installable": False, "scope": "generated native wiring; package validation required"}
    (output / "import-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Generated OMNIBUSF4SD: F405, four motor outputs, ROM layout")
