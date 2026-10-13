#!/usr/bin/env python3
"""Build and verify the actual OmnibusF4SD firmware without accessing hardware."""
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/qt612-release"
ENV = dict(os.environ, CCACHE_DISABLE="1")


def run(command, **kwargs):
    subprocess.run([str(arg) for arg in command], cwd=ROOT, env=ENV, check=True, **kwargs)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


def function(path, name):
    source = path.read_text()
    match = re.search(r"^(?:static\s+)?(?:int32_t|int|float)\s+" + name + r"\([^;]+?\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"Production function not found: {name}")
    start, position, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[position] == "{") - (source[position] == "}"); position += 1
    return source[start:position]


def host_checks():
    flags = ["cc", "-std=gnu99", "-g", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error"]
    with tempfile.TemporaryDirectory(prefix="omnibus-tests-") as directory:
        temporary = Path(directory)
        osd = temporary / "osd"
        run(flags + ["-Iflight/tests/omnibus_mock", "-Iflight/PiOS/inc", "flight/tests/omnibus_osd.c",
                     "flight/PiOS/Common/pios_max7456.c", "-o", osd])
        run([osd])
        configs = []
        for target, name in [("omnibusf4sd", "omnibus"), ("radix", "radix")]:
            source = (ROOT / f"flight/targets/{target}/board-info/board_hw_defs.c").read_text()
            declaration = re.search(r"static const struct pios_bmp280_cfg baro_cfg\s*=\s*\{[^}]+\};", source).group()
            configs.append(declaration.replace("baro_cfg", name + "_baro_cfg"))
        (temporary / "imported_baro_configs.inc").write_text("\n".join(configs) + "\n")
        baro = temporary / "bmp280"
        run(flags + ["-Wno-unused-parameter", "-I", temporary,
                     "-Iflight/tests/omnibus_mock", "-Iflight/PiOS/inc",
                     "-DPIOS_INCLUDE_BMP280_SPI", "-DPIOS_EXCLUDE_BMP280_I2C",
                     "flight/tests/imported_bmp280.c", "flight/PiOS/Common/pios_bmp280.c", "-lm", "-o", baro])
        run([baro])
        mpu = ROOT / "flight/PiOS/Common/pios_mpu.c"
        names = ["PIOS_MPU_Validate", "PIOS_MPU_ClaimBus", "PIOS_MPU_ReleaseBus", "PIOS_MPU_SPI_Read",
                 "PIOS_MPU_SPI_Write", "PIOS_MPU_ReadReg", "PIOS_MPU_WriteReg", "PIOS_MPU_WriteRegVerify",
                 "PIOS_MPU_CheckWhoAmI", "PIOS_MPU_SetGyroRange", "PIOS_MPU_SetAccelRange",
                 "PIOS_MPU_GetGyroScale", "PIOS_MPU_GetAccelScale", "PIOS_MPU_SetGyroBandwidth",
                 "PIOS_MPU_SetAccelBandwidth", "PIOS_MPU_SetSampleRate", "PIOS_MPU_Config", "PIOS_MPU_parse_data"]
        ids = re.search(r"static const uint8_t pios_mpu_whoami\[PIOS_MPU_NUM\] = \{.*?\};", mpu.read_text(), re.S).group()
        # Keep the production copyright/license notice with the unchanged functions.
        (temporary / "omnibus_production.inc").write_text(mpu.read_text().split('#include')[0]
            + ids + "\n" + "\n\n".join(function(mpu, name) for name in names))
        imu = temporary / "mpu"
        run(flags + ["-Wno-unused-parameter", "-I", temporary, "-I", BUILD / "fw_omnibusf4sd",
                     "-Iflight/tests/omnibus_mock", "-Iflight/PiOS/inc", "-Ishared/api",
                     "flight/tests/omnibus_mpu.c", "-lm", "-o", imu])
        run([imu])
        # Compile the actual board call site with automatic stack variables
        # poisoned. This exposes an uninitialized in/out handle deterministically.
        board = ROOT / "flight/targets/imported/fw/pios_board.c"
        initializer = function(mpu, "PIOS_MPU_SPI_Init")
        def board_caller(source):
            match = re.search(r"(pios_mpu_dev_t imu\b.*?)\s+if \(PIOS_BMP280_SPI_Init", source, re.S)
            if not match:
                raise ValueError("Omnibus IMU board startup call site is missing")
            return "static void omnibus_imu_startup(pios_spi_t spi) {\n" + match[1] + "\n}\n"
        startup_flags = flags + ["-ftrivial-auto-var-init=pattern", "-I", temporary,
                                "-Iflight/tests/omnibus_mock", "-Iflight/PiOS/inc"]
        startup = temporary / "startup"
        production = temporary / "omnibus_startup_production.inc"
        production.write_text(initializer + "\n" + board_caller(board.read_text()))
        run(startup_flags + ["flight/tests/omnibus_startup.c", "-o", startup])
        run([startup])
        failed = BUILD / "omnibusf4sd-tests/failed-imu-startup.c"
        if failed.is_file():
            production.write_text(initializer + "\n" + board_caller(failed.read_text()))
            run(startup_flags + ["flight/tests/omnibus_startup.c", "-o", startup])
            result = subprocess.run([str(startup)], cwd=ROOT, env=ENV, capture_output=True,
                                    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
            if result.returncode == 0:
                raise AssertionError("The board startup regression accepted the failed IMU call site")
            print("PASS: preserved failed board initializer rejected with poisoned stack variables", flush=True)


def package_checks():
    packager = module("package_omnibus", ROOT / "make/scripts/package-imported-board.py")
    data, image = packager.manifest(BUILD, "omnibusf4sd")
    elf = bytearray((BUILD / "fw_omnibusf4sd/fw_omnibusf4sd.elf").read_bytes())
    start = struct.unpack_from("<I", elf, 28)[0]; stride, count = struct.unpack_from("<HH", elf, 42)
    loads = [start + i * stride for i in range(count) if struct.unpack_from("<I", elf, start + i * stride)[0] == 1
             and struct.unpack_from("<I", elf, start + i * stride + 16)[0]]
    for field, address in [(12, 0x08004000), (12, 0x08010000), (12, 0x08100000),
                           (8, 0x10000000), (8, 0x2001fff0)]:
        corrupt = elf.copy(); struct.pack_into("<I", corrupt, loads[0] + field, address)
        try: packager.validate_elf(corrupt, "omnibusf4sd")
        except ValueError: pass
        else: raise AssertionError(f"Reserved/out-of-bounds ELF load accepted: {address:#x}")
    upper, offsets = 0, set()
    for line in (BUILD / "ef_omnibusf4sd/ef_omnibusf4sd.hex").read_text().splitlines():
        record = bytes.fromhex(line[1:]); assert sum(record) % 256 == 0
        size, address, kind = record[0], int.from_bytes(record[1:3], "big"), record[3]
        assert len(record) == size + 5
        if kind == 4: upper = int.from_bytes(record[4:6], "big") << 16
        elif kind == 0:
            offset = upper + address - 0x08000000
            assert image[offset:offset + size] == record[4:4 + size]
            offsets.update(range(offset, offset + size))
    profile = packager.reviewed_profile("omnibusf4sd")
    expected = {offset for start, size in packager.segments_for(profile) for offset in range(start, start + size)}
    assert offsets == expected
    startup = startup_checks()
    print("PASS: actual F405 ELF; SRAM/flash bounds and HEX exactly cover the three writable regions", flush=True)
    evidence = BUILD / "omnibusf4sd-tests"; evidence.mkdir(exist_ok=True)
    (evidence / "report.json").write_text(json.dumps({"passed": True, "hardwareValidated": False,
        "imageSha256": hashlib.sha256(image).hexdigest(), "startup": startup, "manifest": data}, indent=2) + "\n")


def validate_startup(disassembly):
    """Check the executable startup path, rather than assuming compiler flags worked."""
    match = re.search(r"^[0-9a-f]+ <ResetHandler>:\n(.*?)(?=^[0-9a-f]+ <|\Z)", disassembly, re.M | re.S)
    if not match:
        raise ValueError("ARM reset handler is missing")
    reset = match[1]
    # CPACR is at 0xe000ed88. Its CP10/CP11 access mask is bits 20..23.
    # No FP register instruction may execute before that access and the barriers.
    first_fp = re.search(r"\b(?:vmsr|vmrs|vldr|vstr|vmov|vpush|vpop)\b", reset)
    if not first_fp or not re.search(r"\be000ed88\b", reset):
        raise ValueError("Reset handler does not enable the FPU before the RTOS uses it")
    prefix = reset[:first_fp.start()]
    access = re.search(r"\borr(?:\.w)?\s+[^\n]*#(?:15728640|0x[fF]00000)\b", prefix)
    if not access or not re.search(r"\bstr(?:\.w)?\b", prefix[access.end():]):
        raise ValueError("Reset handler does not grant full CP10/CP11 access")
    barrier = re.search(r"\bdsb\b[^\n]*\n[^\n]*\bisb\b", prefix)
    if not barrier or barrier.start() < access.end():
        raise ValueError("FPU access is not synchronized before its first instruction")
    if not re.search(r"\bmsr\s+CONTROL,\s*r[0-9]+", reset):
        raise ValueError("Reset handler does not initialize thread mode")
    if "<__early_init>" not in reset[first_fp.end():]:
        raise ValueError("FPU initialization is not before the early-init hook")


def validate_vectors(disassembly):
    match = re.search(r"^[0-9a-f]+ <_port_init>:\n(.*?)(?=^[0-9a-f]+ <|\Z)", disassembly, re.M | re.S)
    if not match or "e000ed00" not in match[1]:
        raise ValueError("ARM interrupt initialization is missing")
    # SCB->VTOR is offset 8. Check the register carrying physical flash base is
    # actually stored there, rather than accepting a stray address constant.
    address = re.search(r"\bmov(?:\.w)?\s+(r[0-9]+),\s*#134217728\b", match[1])
    if not address or not re.search(r"\bstr(?:\.w)?\s+" + address[1]
                                   + r",\s*\[r[0-9]+,\s*#8\]", match[1][address.end():]):
        raise ValueError("Interrupt vectors depend on the address-zero boot alias")


def startup_checks():
    objdump = ROOT / "tools/gcc-arm-none-eabi-6-2017-q2-update/bin/arm-none-eabi-objdump"
    elf = BUILD / "fw_omnibusf4sd/fw_omnibusf4sd.elf"
    disassembly = subprocess.check_output([str(objdump), "-d", str(elf)], cwd=ROOT, env=ENV, text=True)
    validate_startup(disassembly)
    validate_vectors(disassembly)
    epilogue = re.search(r"^[0-9a-f]+ <_port_irq_epilogue>:\n(.*?)(?=^[0-9a-f]+ <|\Z)",
                         disassembly, re.M | re.S)
    if not epilogue or "vmrs" not in epilogue[1]:
        raise ValueError("FPU startup check is not paired with the delivered FP-enabled RTOS")
    # Preserve and reject the actual image from the failed physical installation.
    failed = BUILD / "omnibusf4sd-tests/failed-startup.elf"
    if failed.is_file():
        old = subprocess.check_output([str(objdump), "-d", str(failed)], cwd=ROOT, env=ENV, text=True)
        try:
            validate_startup(old)
        except ValueError:
            pass
        else:
            raise AssertionError("The startup regression check accepted the failed installation image")
        try:
            validate_vectors(old)
        except ValueError:
            pass
        else:
            raise AssertionError("The vector regression check accepted the failed installation image")
    print("PASS: actual ARM reset enables/synchronizes FPU before early init and FP-enabled RTOS", flush=True)
    print("PASS: actual ARM interrupt setup uses physical flash vectors after ROM DFU", flush=True)
    return {"fpuEnabledBeforeEarlyInit": True, "fpuAccessSynchronized": True,
            "rtosUsesFpu": True, "physicalFlashVectors": True, "failedImageRejected": failed.is_file()}


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    with (BUILD / "omnibusf4sd-tests-build.log").open("w") as log:
        run(["make", "--jobserver-style=pipe", "-j2", "ef_omnibusf4sd", f"BUILD_DIR={BUILD}", "CCACHE_BIN="],
            stdout=log, stderr=subprocess.STDOUT)
    host_checks(); package_checks()
    print(f"PASS: firmware built and verified. Evidence: {BUILD}/omnibusf4sd-tests/report.json. No controller accessed.", flush=True)


if __name__ == "__main__": main()
