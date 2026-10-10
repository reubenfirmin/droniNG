#!/usr/bin/env python3
"""Offline, bounded survey of pinned configs; never emits installable firmware."""

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


ROOT = Path(__file__).resolve().parents[2]
SURVEY = ROOT / "flight/PiOS/Betaflight"
UPSTREAM = SURVEY / "upstream/betaflight"
PIN = re.compile(r"P[A-K]\d{1,2}$")


def verify_sources():
    lock = json.loads((SURVEY / "survey.lock.json").read_text())
    manifest = json.loads((SURVEY / "source-manifest.json").read_text())
    expected = {(k, p) for k, repo in lock["repositories"].items() for p in repo["files"]}
    actual = [(f["repository"], f["path"]) for f in manifest["files"]]
    if len(actual) != len(set(actual)) or set(actual) != expected:
        raise ValueError("Source manifest and lock inventories differ")
    for k, repo in lock["repositories"].items():
        if manifest["repositories"][k] != {n: repo[n] for n in ("repository", "commit")}:
            raise ValueError("Source manifest and lock pins differ")
    for f in manifest["files"]:
        data = (SURVEY / "upstream" / f["repository"] / f["path"]).read_bytes()
        blob = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        if (len(data) != f["bytes"] or hashlib.sha256(data).hexdigest() != f["sha256"]
                or blob != f["git_blob_sha1"]):
            raise ValueError(f"Source integrity failure: {f['path']}")
    return lock, manifest


def preprocess(cc, text, macros=False):
    result = subprocess.run(cc + ["-E", "-x", "c", "-dM" if macros else "-P", "-"],
                            input=text, text=True, capture_output=True, check=True)
    return result.stdout


def parse_defines(output):
    # Empty object macros must not consume the next line. Function-like macros
    # are outside this board-definition survey's object-macro inventory.
    result = {}
    for line in output.splitlines():
        match = re.fullmatch(r"#define ([A-Za-z_]\w*)(?:[ \t]+(.*))?", line)
        if match:
            result[match[1]] = match[2] or ""
    return result


def resolve(macros, value):
    seen = set()
    while value in macros:
        if value in seen:
            raise ValueError(f"Cyclic alias: {value}")
        seen.add(value)
        value = macros[value]
    return value


def f405_timers(cc):
    source = (UPSTREAM / "src/platform/STM32/timer_stm32f4xx.c").read_text()
    match = re.search(r"const timerHardware_t fullTimerHardware\[[^]]+\] = \{(.*?)\n\};",
                      source, re.S)
    if not match:
        raise ValueError("Upstream timer table shape changed")
    # Evaluate the actual table's conditionals, not a regex over excluded entries.
    body = preprocess(cc, "#define STM32F405xx 1\n" + match.group(1))
    table = defaultdict(list)
    for timer, channel, pin in re.findall(
            r"DEF_TIM\(\s*(TIM\d+),\s*(CH\d+N?),\s*(P[A-K]\d+),\s*0,\s*0\)", body):
        table[pin].append({"timer": timer, "channel": channel})
    if len(re.findall(r"DEF_TIM\(", body)) != sum(map(len, table.values())):
        raise ValueError("Unrecognized timer record; refuse partial translation")
    return table


def timer_record(pin, occurrence, dma, table):
    if not PIN.fullmatch(pin) or occurrence < 1:
        raise ValueError(f"Invalid timer pin/occurrence: {pin}/{occurrence}")
    result = {"pin": pin, "occurrence": occurrence, "betaflight_dma_option": dma}
    if table is None:
        result["resolution"] = "blocked: processor table/backend not supported by this survey"
    else:
        options = table.get(pin, [])
        if occurrence > len(options):
            raise ValueError(f"No timer occurrence {occurrence} for {pin}")
        result.update(options[occurrence - 1])
        result["resolution"] = "physical timer/channel only; PiOS AF/DMA allocation unvalidated"
    return result


def inspect_config(cc, path, table):
    source = path.read_text()
    # Include the board header alone deliberately: defaults are reported as missing.
    # Production import must supply the complete upstream build/default context.
    output = preprocess(cc, f'#include "{path}"\n', macros=True)
    macros = parse_defines(output)
    own_keys = set(re.findall(r"^\s*#define\s+(\w+)", source, re.M))
    own = {k: macros[k] for k in sorted(own_keys) if k in macros}
    gyrodev = (UPSTREAM / "src/main/pg/gyrodev.c").read_text()
    alignment_block = re.search(r"#ifndef GYRO_1_ALIGN\n.*?#endif // GYRO_1_ALIGN", gyrodev, re.S)
    if not alignment_block:
        raise ValueError("Upstream gyro alignment default shape changed")
    alignment_macros = parse_defines(preprocess(
        cc, f'#include "{path}"\n' + alignment_block.group(0), macros=True))
    alignment = alignment_macros["GYRO_1_ALIGN"]
    mcu = own["FC_TARGET_MCU"]
    native = mcu == "STM32F405"
    include = f'#include "{path}"\n'
    expanded = preprocess(cc, include + "TIMER_PIN_MAPPING\n")
    timers = []
    pattern = r"TIMER_PIN_MAP\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*\)"
    for index, pin, occurrence, dma in re.findall(pattern, expanded):
        record = timer_record(pin, int(occurrence), int(dma), table if native else None)
        record["index"] = int(index)
        timers.append(record)
    if len(re.findall(r"TIMER_PIN_MAP\(", expanded)) != len(timers):
        raise ValueError("Unrecognized timer mapping")
    pin_roles = defaultdict(list)
    for key, value in own.items():
        if "PIN" in key and PIN.fullmatch(resolve(macros, value)):
            pin_roles[resolve(macros, value)].append(key)
    spi = {}
    for key in own:
        match = re.fullmatch(r"SPI(\d+)_SCK_PIN", key)
        if match:
            instance = "SPI" + match[1]
            spi[instance] = {signal: own.get(instance + "_" + signal + "_PIN")
                             for signal in ("SCK", "SDI", "SDO")}
    imus = []
    for name, flag in (("MPU6000", "USE_GYRO_SPI_MPU6000"),
                       ("MPU6500", "USE_GYRO_SPI_MPU6500"),
                       ("ICM42688P", "USE_GYRO_SPI_ICM42688P"),
                       ("BMI270", "USE_ACCGYRO_BMI270"),
                       ("LSM6DSK320X", "USE_ACCGYRO_LSM6DSK320X")):
        if flag in own:
            imus.append(name)
    issues = []
    if not native:
        issues.append("required: new MCU/startup/RTOS/peripheral backend")
    for field in ("SYSTEM_HSE_MHZ",):
        if field not in own:
            issues.append(f"required: resolve {field} from full upstream defaults or reviewed hardware override")
    if len(imus) > 1:
        issues.append("required: exact fitted IMU/revision or implement and validate every allowed alternative")
    for name in imus:
        if name not in ("MPU6000", "MPU6500"):
            issues.append(f"required: native PiOS {name} driver port")
    if "USE_GYRO_CLKIN" in own:
        issues.append("required: explicit gyro CLKIN timer/clock port; outside initial MVP")
    if "USE_SDCARD_SPI" in own:
        issues.append("optional logging: SPI SD-card backend and storage integration")
    if "USE_FLASH_W25N01G" in own:
        issues.append("optional logging: SPI NAND backend, ECC/bad-block handling and storage integration")
    if "USE_BARO_DPS310" in own:
        issues.append("barometer: DPS310 port, async bus replacement and Pa-to-kPa conversion")
    if own.get("ADC_INSTANCE") == "ADC3":
        issues.append("ADC: backend currently hardcodes ADC1 clock enable; ADC3 needs review/fix")
    if any(k.startswith("SOFTSERIAL") for k in own):
        issues.append("optional softserial outside initial MVP")
    shared = [{"pin": pin, "roles": roles} for pin, roles in sorted(pin_roles.items())
              if len(roles) > 1]
    if shared:
        issues.append("required: mutually exclusive resources; do not enable every defined pin role")
    swd = {p: pin_roles[p] for p in ("PA13", "PA14") if p in pin_roles}
    if swd:
        issues.append("GPIO: declared functions occupy SWD debug pins")
    # Keep all board macros visible. None of these defaults is silently translated.
    return {
        "target": own["BOARD_NAME"], "source": str(path.relative_to(SURVEY)),
        "mcu": mcu, "processor_backend_available": native, "installable": False,
        "imu_alternatives": imus,
        "gyro": {"bus": own.get("GYRO_1_SPI_INSTANCE"), "cs": own.get("GYRO_1_CS_PIN"),
                 "drdy": own.get("GYRO_1_EXTI_PIN"), "alignment": alignment,
                 "alignment_source": "board definition" if "GYRO_1_ALIGN" in own else "pg/gyrodev.c default block",
                 "external_clock_pin": own.get("GYRO_1_CLKIN_PIN")},
        "spi_buses": spi, "hse_mhz_explicit": own.get("SYSTEM_HSE_MHZ"),
        "motor_pins": {k: v for k, v in own.items() if re.fullmatch(r"MOTOR\d+_PIN", k)},
        "timer_mappings": timers, "shared_pin_roles": shared, "swd_pin_roles": swd,
        "issues": issues, "board_macros": own,
    }


def dependency_inventory():
    files = []
    for path in sorted((UPSTREAM / "src/main/drivers").rglob("*.c")):
        if path.name.startswith(("accgyro_", "barometer_", "flash_")) or path.name in (
                "max7456.c", "sdcard_spi.c"):
            source = path.read_text()
            files.append({
                "source": str(path.relative_to(UPSTREAM)), "lines": len(source.splitlines()),
                "quoted_includes": re.findall(r'^#include "([^"]+)"', source, re.M),
                "boundary_symbols": sorted(set(re.findall(
                    r"\b(?:spi\w+|bus\w+|mpu\w+|EXTI\w+|IO\w+|gyroConfig|gyroDeviceConfig|delay\w*)\s*(?=\()",
                    source))),
                "scope": "lexical inventory; not complete preprocessed/link dependency closure",
            })
    bosch = (UPSTREAM / "lib/main/BoschSensortec/BMI270-Sensor-API/bmi270_maximum_fifo.c").read_text()
    array = re.search(r"bmi270_maximum_fifo_config_file\[\] = \{(.*?)\};", bosch, re.S)
    if not array:
        raise ValueError("BMI270 config blob missing")
    return {"drivers": files, "bmi270_config_blob_bytes": len(re.findall(r"0x[0-9a-fA-F]+", array.group(1)))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc", help="host C compiler; no ARM compiler required")
    parser.add_argument("--output", type=Path, default=SURVEY / "survey/desk-check.json")
    args = parser.parse_args()
    cc = shlex.split(args.cc)
    lock, manifest = verify_sources()
    table = f405_timers(cc)
    targets = [inspect_config(cc, SURVEY / "upstream/config" / p, table)
               for p in lock["repositories"]["config"]["files"] if p.endswith("config.h")]
    report = {"schema_version": 1, "scope": "offline survey; no generated firmware or hardware verification",
              "source_files_verified": len(manifest["files"]), "repositories": manifest["repositories"],
              "targets": targets, "dependencies": dependency_inventory()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for t in targets:
        resolved = sum("timer" in r for r in t["timer_mappings"])
        print(f"{t['target']}: {t['mcu']}; IMUs {', '.join(t['imu_alternatives'])}; "
              f"{resolved}/{len(t['timer_mappings'])} physical timers resolved; "
              f"{len(t['issues'])} review items; not installable")
    print(f"Verified {len(manifest['files'])} pinned files; wrote {args.output.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
