#!/usr/bin/env python3
"""Audit original SP Racing F3 wiring and exercise MPU6050 code offline; never flashes."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SURVEY = ROOT / "flight/Betaflight"
UPSTREAM = SURVEY / "upstream/betaflight_3_1_5"
TARGET = UPSTREAM / "src/main/target/SPRACINGF3"
OUTPUT = ROOT / "build/spracingf3-dry-run"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def run(command):
    result = subprocess.run([str(arg) for arg in command], text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"Command failed: {shlex.join(map(str, command))}\n{result.stdout}{result.stderr}")
    return result.stdout.strip()


def profile(cc, audit):
    macros = audit.parse_defines(audit.preprocess(cc, f'#include "{TARGET / "target.h"}"\n', macros=True))
    expected = {"TARGET_BOARD_IDENTIFIER": '"SRF3"', "GYRO_MPU6050_ALIGN": "CW270_DEG",
                "ACC_MPU6050_ALIGN": "CW270_DEG", "MPU_INT_EXTI": "PC13", "LED0": "PB3",
                "BEEPER": "PC15", "I2C_DEVICE": "(I2CDEV_1)", "M25P16_CS_PIN": "PB12",
                "M25P16_SPI_INSTANCE": "SPI2", "ADC_INSTANCE": "ADC2", "VBAT_ADC_PIN": "PA4",
                "CURRENT_METER_ADC_PIN": "PA5", "RSSI_ADC_PIN": "PB2"}
    if any(macros.get(key) != value for key, value in expected.items()):
        raise ValueError("Original SPRACINGF3 wiring changed; review required")
    if (not {"USE_GYRO_MPU6050", "USE_ACC_MPU6050", "BEEPER_INVERTED"}.issubset(macros)
            or "USE_VCP" in macros or "USE_GYRO_SPI_MPU6500" in macros):
        raise ValueError("This audit requires the original I2C MPU6050/CP2102 board, not the Evo or a variant")
    # Evaluate the F3 alternate-function branch and its DMA-remap conditions.
    timer_def = (UPSTREAM / "src/main/drivers/timer_def.h").read_text()
    timer_def = re.sub(r"^\s*#include[^\n]*", "", timer_def, flags=re.M)
    af = audit.parse_defines(audit.preprocess(cc, "#define STM32F3\n#define REMAP_TIM17_DMA\n"
                                            + timer_def, macros=True))
    source = (TARGET / "target.c").read_text()
    body = re.search(r"timerHardware\[USABLE_TIMER_CHANNEL_COUNT\]\s*=\s*\{(.*?)\n\};", source, re.S)
    if not body:
        raise ValueError("Missing original F3 timer table")
    body = audit.preprocess(cc, body[1])
    records = re.findall(r"DEF_TIM\(\s*(TIM\d+),\s*(CH\d+),\s*(P[A-F]\d+),\s*([^,]+),\s*([01])\s*\)", body)
    if len(records) != int(macros["USABLE_TIMER_CHANNEL_COUNT"]) or len(records) != body.count("DEF_TIM("):
        raise ValueError("Refusing incomplete F3 timer translation")
    timers = []
    for timer, channel, pin, use, output in records:
        alternate = af.get(f"GPIO_AF__{pin}_{timer}_{channel}", "")
        if not re.fullmatch(r"GPIO_AF_\d+", alternate):
            raise ValueError(f"Missing F3 alternate function for {pin}/{timer}/{channel}")
        timers.append({"pin": pin, "timer": timer, "channel": channel, "alternateFunction": alternate,
                       "roles": use.strip().split(" | "), "outputEnabled": bool(int(output))})
    motors = [entry for entry in timers if "TIM_USE_MOTOR" in entry["roles"]]
    fixture = [("PA6", "TIM16", "CH1", "GPIO_AF_1"), ("PA7", "TIM17", "CH1", "GPIO_AF_1"),
               ("PA11", "TIM4", "CH1", "GPIO_AF_10"), ("PA12", "TIM4", "CH2", "GPIO_AF_10"),
               ("PB8", "TIM4", "CH3", "GPIO_AF_2"), ("PB9", "TIM4", "CH4", "GPIO_AF_2"),
               ("PA2", "TIM15", "CH1", "GPIO_AF_9"), ("PA3", "TIM15", "CH2", "GPIO_AF_9")]
    if [tuple(m[k] for k in ("pin", "timer", "channel", "alternateFunction")) for m in motors] != fixture:
        raise ValueError("Original SP Racing F3 motor map differs from the reviewed fixture")
    serial = {f"UART{port}": {"tx": macros[f"UART{port}_TX_PIN"], "rx": macros[f"UART{port}_RX_PIN"]}
              for port in (1, 2, 3)}
    if serial != {"UART1": {"tx": "PA9", "rx": "PA10"}, "UART2": {"tx": "PA14", "rx": "PA15"},
                  "UART3": {"tx": "PB10", "rx": "PB11"}}:
        raise ValueError("Serial allocation requires review")
    bus = (UPSTREAM / "src/main/drivers/bus_i2c_stm32f30x.c").read_text()
    for name, pin in (("I2C1_SCL", "PB6"), ("I2C1_SDA", "PB7")):
        if not re.search(rf"#ifndef {name}\s+#define {name} {pin}\s+#endif", bus):
            raise ValueError("Upstream I2C defaults changed")
    memory = (UPSTREAM / "src/main/target/link/stm32_flash_f303_256k.ld").read_text()
    for region, address, size in (("FLASH", "0x08000000", "252K"), ("FLASH_CONFIG", "0x0803F000", "4K"),
                                  ("RAM", "0x20000000", "40K"), ("CCM", "0x10000000", "8K")):
        if not re.search(rf"\b{region}\s*\([^)]*\)\s*:\s*ORIGIN = {address}, LENGTH = {size}\b", memory):
            raise ValueError("F303 memory assumptions require review")
    return {"target": "SPRACINGF3", "boardIdentifier": "SRF3", "mcu": "STM32F303xC",
            "flashBytes": 262144, "sramBytes": 40960, "ccmBytes": 8192,
            "memoryScope": "pinned upstream target; physical flash size has not been read",
            "imu": {"chip": "MPU6050", "bus": "I2C1", "scl": "PB6", "sda": "PB7", "drdy": "PC13",
                    "betaflightAlignment": "CW270_DEG", "nativeOrientation": "PIOS_MPU_TOP_180DEG",
                    "driver": "flight/PiOS/Common/pios_mpu.c", "action": "reuse native I2C driver"},
            "serial": serial, "timers": timers, "motorOutputs": motors,
            "transport": {"gcs": "CP2102 / UART1", "recovery": "STM32 system bootloader over UART1",
                          "usbDfu": False},
            "proposedSerialRoles": {"gcs": "UART1", "gps": "UART2", "receiver": "UART3"},
            "constraints": ["UART1 headers share the USB bridge; keep GCS telemetry there",
                            "UART2 TX is SWCLK; UART2 and SWD cannot operate together",
                            "UART3 shares PB10/PB11 with parallel PWM inputs",
                            "PA11/PA12 are motor outputs, so the Evo native-USB path cannot be reused",
                            "No onboard MAX7456; external OSD needs a reviewed transport and resource allocation",
                            "MPU6050 silicon revision must be read successfully before selecting the acceleration scale"],
            "installable": False}


def native_functions():
    helper = module("omnibus_test_helpers", ROOT / "flight/tests/run_omnibusf4sd.py")
    path = ROOT / "flight/PiOS/Common/pios_mpu.c"
    source = path.read_text()
    declarations = [re.search(pattern, source, re.S)[0] for pattern in (
        r"static const uint8_t pios_mpu_whoami\[PIOS_MPU_NUM\] = \{.*?\};",
        r"static const uint8_t pios_mpu_i2c_addr\[\] = \{.*?\};")]
    names = ["PIOS_MPU_Validate", "PIOS_MPU_I2C_Read", "PIOS_MPU_I2C_Write", "PIOS_MPU_ReadReg",
             "PIOS_MPU_WriteReg", "PIOS_MPU_WriteRegVerify", "PIOS_MPU_CheckWhoAmI", "PIOS_MPU_I2C_Probe",
             "PIOS_MPU_SetGyroRange", "PIOS_MPU_SetAccelRange", "PIOS_MPU_GetGyroScale", "PIOS_MPU_GetAccelScale",
             "PIOS_MPU_SetGyroBandwidth", "PIOS_MPU_SetAccelBandwidth", "PIOS_MPU_SetSampleRate",
             "PIOS_MPU_Config", "PIOS_MPU_parse_data", "PIOS_MPU_Check6050Revision"]
    return source.split("#include")[0] + "\n".join(declarations + [helper.function(path, name) for name in names]
        + [helper.function(ROOT / "flight/PiOS/Common/pios_ms5611.c", "PIOS_MS5611_ValidatePROM")])


def probe(cc, candidate, output):
    output.mkdir(parents=True, exist_ok=True)
    mock = output / "mock"
    # Explicit service boundary; the original MPU register/type declarations stay real.
    headers = ("platform.h", "build/debug.h", "common/maths.h", "common/utils.h", "nvic.h", "system.h",
               "gpio.h", "exti.h", "io_types.h", "rcc_types.h", "gyro_sync.h", "sensor.h", "accgyro.h")
    for name in headers:
        path = mock / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("/* Host probe service stub. */\n")
    (mock / "platform.h").write_text('#include "spracingf3_mock.h"\n')
    (mock / "pios.h").write_text('#include "spracingf3_mock.h"\n#include <pios_spi.h>\n#include <pios_i2c.h>\n')
    (output / "spracingf3_production.inc").write_text(native_functions())
    (output / "spracingf3_profile.h").write_text("#define SPRACINGF3_IMU_ORIENTATION "
                                               + candidate["imu"]["nativeOrientation"] + "\n")
    executable = output / "mpu6050-host"
    upstream = UPSTREAM / "src/main/drivers"
    # Preserve the native int/size_t loop and the parameter used only with a magnetometer.
    run(cc + ["-std=gnu99", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-Wno-unused-parameter",
              "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error",
              "-I" + str(mock), "-I" + str(SURVEY / "survey"), "-I" + str(upstream), "-I" + str(output),
              "-I" + str(ROOT / "flight/PiOS/inc"), "-I" + str(ROOT / "shared/api"),
              str(upstream / "accgyro_mpu6050.c"), str(SURVEY / "survey/spracingf3_mpu_host.c"),
              "-lm", "-o", str(executable)])
    return {"compiler": run(cc + ["--version"]).splitlines()[0], "stdout": run([executable]),
            "scope": "untouched upstream chip source and extracted unchanged native I2C functions; simulated bus; no RTOS/IRQ/physical timing proof",
            "nativeDriverSha256": hashlib.sha256((ROOT / "flight/PiOS/Common/pios_mpu.c").read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    cc = shlex.split(args.cc)
    audit = module("desk_check", Path(__file__).with_name("betaflight-desk-check.py"))
    _, manifest = audit.verify_sources(SURVEY / "spracingf3.lock.json", SURVEY / "spracingf3-source-manifest.json")
    candidate = profile(cc, audit)
    host = probe(cc, candidate, args.output)
    report = {"schemaVersion": 1, "status": "passed", "installable": False, "hardwareValidated": False,
              "sources": manifest["repositories"], "verifiedFiles": len(manifest["files"]),
              "profile": candidate, "hostProbe": host,
              "remainingWork": ["Bench-validate native startup/FPU and runtime heap with required modules",
                                "Confirm fitted barometer/magnetometer and MPU6050 silicon revision on hardware",
                                "Bench-validate UART backup, selective writes, readback, recovery and telemetry reconnection",
                                "Bench-validate receiver, GPS, external OSD and telemetry together",
                                "Bench-check DRDY, I2C timing, axes, outputs, saved settings and hardware-BOOT recovery"]}
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(host["stdout"])
    print(f"PASS: {len(manifest['files'])} pinned files; 17 F3 timer/AF mappings, eight motor outputs")
    print(f"Wrote {args.output / 'report.json'}; no installable image generated or controller accessed")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.CalledProcessError) as error:
        sys.exit(f"SP Racing F3 dry run failed: {error}")
