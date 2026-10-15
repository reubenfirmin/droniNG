#!/usr/bin/env python3
"""Offline original-RADIX wiring and FPGA adapter proof; never accesses a controller."""
import argparse
from collections import defaultdict
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
UPSTREAM = SURVEY / "upstream/brainfpv"
TARGET = UPSTREAM / "src/main/target/RADIX"
OUTPUT = ROOT / "build/radix-dry-run"


def run(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"Command failed: {shlex.join(map(str, command))}\n{result.stderr or result.stdout}")
    return result


def timer_mappings(cc, audit, macros):
    source = (UPSTREAM / "src/main/drivers/stm32/timer_stm32f4xx.c").read_text()
    match = re.search(r"const timerHardware_t fullTimerHardware\[[^]]+\] = \{(.*?)\n\};", source, re.S)
    if not match:
        raise ValueError("Missing upstream timer table")
    body = audit.preprocess(cc, "#define STM32F446xx 1\n" + match[1])
    table = defaultdict(list)
    for timer, channel, pin in re.findall(r"DEF_TIM\(\s*(TIM\d+),\s*(CH\d+N?),\s*(P[A-K]\d+),\s*0,\s*0\)", body):
        table[pin].append({"timer": timer, "channel": channel})
    if body.count("DEF_TIM(") != sum(map(len, table.values())):
        raise ValueError("Unrecognized timer table entry")
    expanded = audit.preprocess(cc, f'#include "{TARGET / "target.h"}"\nTIMER_PIN_MAPPING\n')
    records = re.findall(r"TIMER_PIN_MAP\(\s*(\d+),\s*(\w+),\s*(\d+),\s*(-?\d+)\)", expanded)
    if len(records) != expanded.count("TIMER_PIN_MAP(") or len(records) != 8:
        raise ValueError("Incomplete RADIX timer mapping")
    mappings = []
    for index, pin, occurrence, dma in records:
        record = audit.timer_record(pin, int(occurrence), int(dma), table)
        record["index"] = int(index)
        record["resolution"] = "F446 physical mapping; native DMA/waveforms remain unvalidated"
        mappings.append(record)
    # Independent reviewed fixture for the initial four outputs.
    expected = [("PA2", "TIM5", "CH3"), ("PA3", "TIM5", "CH4"),
                ("PA10", "TIM1", "CH3"), ("PA15", "TIM2", "CH1")]
    if [(m["pin"], m["timer"], m["channel"]) for m in mappings[:4]] != expected:
        raise ValueError("Motor map differs from the reviewed RADIX fixture")
    for i, record in enumerate(mappings[:4], 1):
        if record["pin"] != macros[f"MOTOR{i}_PIN"]:
            raise ValueError("Motor pin and timer map disagree")
    return mappings


def profile(cc, audit):
    macros = audit.parse_defines(audit.preprocess(cc, f'#include "{TARGET / "target.h"}"\n', macros=True))
    target_make = (TARGET / "target.mk").read_text()
    mcu = re.search(r"^TARGET_MCU\s*:=\s*(\w+)$", target_make, re.M)
    if not mcu or mcu[1] != "STM32F446xx" or macros["TARGET_BOARD_IDENTIFIER"] != '"RADIX"':
        raise ValueError("This proof only covers the original STM32F446 RADIX")
    if macros["SYSTEM_HSE_MHZ"] != "16" or "BRAINFPV_FPGA_INCLUDE_BITSTREAM" in macros:
        raise ValueError("Clock/FPGA configuration requires a new audit")
    for flag in ("USE_ACCGYRO_BMI160", "USE_BARO_SPI_BMP280", "USE_BRAINFPV_FPGA", "INCLUDE_VIDEO_QUADSPI"):
        if flag not in macros:
            raise ValueError(f"Required RADIX input missing: {flag}")
    wiring = {key: macros[key] for key in (
        "GYRO_1_SPI_INSTANCE", "GYRO_1_CS_PIN", "GYRO_1_EXTI_PIN", "GYRO_1_ALIGN",
        "BARO_SPI_INSTANCE", "BARO_CS_PIN", "SPI3_SCK_PIN", "SPI3_SDI_PIN", "SPI3_SDO_PIN",
        "BRAINFPVFPGA_SPI_INSTANCE", "BRAINFPVFPGA_SPI_DIVISOR", "BRAINFPVFPGA_CS_PIN",
        "BRAINFPVFPGA_CDONE_PIN", "BRAINFPVFPGA_CRESET_PIN", "BRAINFPVFPGA_RESET_PIN", "BRAINFPVFPGA_CLOCK_PIN",
        "VIDEO_QSPI_CLOCK_PIN", "VIDEO_QSPI_IO0_PIN", "VIDEO_QSPI_IO1_PIN", "VIDEO_VSYNC", "VIDEO_HSYNC",
        "ADC_VBAT_PIN", "ADC_CURR_PIN", "ADC_RSSI_PIN", "SDCARD_SPI_INSTANCE", "SDCARD_SPI_CS_PIN", "SDCARD_DETECT_PIN")}
    serial = {str(port): {"tx": macros[f"UART{port}_TX_PIN"], "rx": macros[f"UART{port}_RX_PIN"]}
              for port in (1, 3, 4, 6)}
    return {"target": "RADIX", "mcu": mcu[1], "hse_hz": int(macros["SYSTEM_HSE_MHZ"]) * 1000000,
            "flash_bytes": 512 * 1024, "ram_bytes": 128 * 1024,
            "memory_evidence": "manufacturer F446 linker script; physical MCU/PCB still needs confirmation",
            "installable": False, "wiring": wiring, "timer_mappings": timer_mappings(cc, audit, macros),
            "serial": serial, "proposed_roles": {"receiver": "UART3", "gps": "UART4",
                                                   "external_telemetry": "UART1", "gcs": "USB",
                                                   "receiver_telemetry_if_needed": "UART6"},
            "serial_limit": "UART4 GPS excludes ESC telemetry on RX4; UART6 needs explicit FPGA inversion policy",
            "fpga_configuration": "normal upstream init resets CRESET and waits for CDONE; embedded bitstream disabled"}


def object_size(tool, path):
    fields = run([str(tool), str(path)]).stdout.splitlines()[1].split()
    return dict(zip(("text", "data", "bss"), map(int, fields[:3])))


def baseline(elf, size):
    sections = {}
    for line in run([str(size), "-A", str(elf)]).stdout.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1].isdigit() and fields[2].isdigit():
            sections[fields[0]] = {"bytes": int(fields[1]), "address": int(fields[2])}
    flash = sum(s["bytes"] for s in sections.values() if 0x08000000 <= s["address"] < 0x08080000)
    flash += sections.get(".data", {"bytes": 0})["bytes"]
    static_ram = sum(sections.get(n, {"bytes": 0})["bytes"] for n in (".stacks", ".data", ".bss"))
    return {"target": "brainre1", "elf": str(elf.relative_to(ROOT)),
            "sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
            "flash_bytes": flash, "static_ram_bytes": static_ram,
            "remaining_heap_bytes": sections[".heap"]["bytes"],
            "scope": "existing RE1 baseline only; no Radix wiring, direct boot or runtime resource proof"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--arm-prefix", default=str(ROOT / "tools/gcc-arm-none-eabi-6-2017-q2-update/bin/arm-none-eabi-"))
    args = parser.parse_args()
    cc = shlex.split(args.cc)
    spec = importlib.util.spec_from_file_location("desk_check", Path(__file__).with_name("betaflight-desk-check.py"))
    audit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(audit)
    _, manifest = audit.verify_sources(SURVEY / "radix.lock.json", SURVEY / "radix-source-manifest.json")
    candidate = profile(cc, audit)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    source = TARGET / "fpga_drv.c"
    host = SURVEY / "survey/radix_fpga_host.c"
    includes = ["-I" + str(SURVEY / "survey/radix_mock"), "-I" + str(TARGET), "-I" + str(ROOT / "flight/PiOS/inc")]
    flags = ["-std=gnu99", "-Os", "-Wall", "-Wextra", "-Werror", "-fshort-enums"] + includes
    # Recent GCC diagnoses this pre-existing uint8_t/enum declaration mismatch.
    # Preserve the pinned source and allow only that diagnostic in the host proof.
    host_flags = flags + ["-Wno-error=enum-int-mismatch", "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error"]
    executable = OUTPUT / "radix-fpga-host"
    compile_result = run(cc + host_flags + [str(source), str(host), "-o", str(executable)])
    result = run([str(executable)])
    print(result.stdout.strip())
    arm_cc, arm_size = args.arm_prefix + "gcc", args.arm_prefix + "size"
    arm_obj = OUTPUT / "radix-fpga-arm.o"
    run([arm_cc] + flags + ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
                            "-c", str(source), "-o", str(arm_obj)])
    report = {"schema_version": 1, "status": "passed", "installable": False,
              "scope": "source audit, host simulated SPI/GPIO and Cortex-M4 compile boundary only; no hardware access",
              "sources": manifest["repositories"], "verified_files": len(manifest["files"]),
              "profile": candidate,
              "host_probe": {"compiler": run(cc + ["--version"]).stdout.splitlines()[0],
                             "undefined_behavior_sanitizer": "trap mode", "stdout": result.stdout.strip(),
                             "known_upstream_diagnostic": compile_result.stderr.strip()},
              "arm_probe": {"compiler": run([arm_cc, "--version"]).stdout.splitlines()[0],
                            "object_bytes": object_size(arm_size, arm_obj),
                            "scope": "upstream FPGA source with mock service declarations, unlinked; excludes bridge and runtime"}}
    elf = ROOT / "build/qt612-release/fw_brainre1/fw_brainre1.elf"
    if elf.exists():
        report["existing_f446_baseline"] = baseline(elf, arm_size)
    (OUTPUT / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {len(manifest['files'])} pinned files; F446 wiring and eight timer mappings")
    print(f"PASS: Cortex-M4 FPGA object compiled: {report['arm_probe']['object_bytes']}")
    print(f"Wrote {OUTPUT.relative_to(ROOT)}/report.json; no installable image generated")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f"RADIX dry run failed: {error}", file=sys.stderr)
        sys.exit(1)
