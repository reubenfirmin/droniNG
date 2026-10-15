#!/usr/bin/env python3
"""Build and check the actual experimental RADIX image. Never accesses hardware."""
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/qt612-release"
ENV = dict(os.environ, CCACHE_DISABLE="1")


def run(command, **kwargs):
    subprocess.run([str(arg) for arg in command], cwd=ROOT, env=ENV, check=True, **kwargs)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def function(path, name):
    source = path.read_text()
    match = re.search(r"^static\s+[^\n]+\b" + name + r"\([^;]+?\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"Production function not found: {name}")
    start, position, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[position] == "{") - (source[position] == "}")
        position += 1
    return source[start:position]


def host_checks():
    flags = ["cc", "-std=gnu99", "-g", "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error"]
    with tempfile.TemporaryDirectory(prefix="radix-tests-") as directory:
        temporary = Path(directory)
        fpga = temporary / "fpga"
        run(flags + ["-Iflight/tests/imported_mock", "-Iflight/PiOS/inc", "-Iflight/targets/brainre1/re1fpga",
                     "flight/tests/imported_fpga.c", "flight/targets/brainre1/re1fpga/fpga_drv.c", "-o", fpga])
        run([fpga])
        spi = ROOT / "flight/PiOS/STM32/pios_spi.c"
        bmi = ROOT / "flight/PiOS/Common/pios_bmi160.c"
        board = ROOT / "flight/targets/imported/fw/pios_board.c"
        functions = [function(spi, name) for name in ("SPI_WaitFlag", "SPI_PIO_TransferBlock")]
        functions.append(function(bmi, "PIOS_BMI160_parse_data"))
        functions.append(function(board, "battery_settings_init"))
        # Preserve source license notices with the extracted production functions.
        (temporary / "imported_production.inc").write_text(
            spi.read_text().split('#include <pios.h>')[0] +
            bmi.read_text().split('/* Project Includes */')[0] +
            board.read_text().split('#include "board_hw_defs.c"')[0] + "\n\n".join(functions))
        runtime = temporary / "runtime"
        run(flags + ["-Wall", "-Wextra", "-Werror", "-I", temporary, "-I", BUILD / "fw_radix", "-Iflight/PiOS/inc",
                     "-Iflight/PiOS/STM32F4xx/inc", "flight/tests/imported_runtime.c", "-lm", "-o", runtime])
        run([runtime])


def package_checks():
    packager = module("package_radix", ROOT / "make/scripts/package-imported-board.py")
    data, image = packager.manifest(BUILD)
    elf = bytearray((BUILD / "fw_radix/fw_radix.elf").read_bytes())
    import struct
    start = struct.unpack_from("<I", elf, 28)[0]
    stride, count = struct.unpack_from("<HH", elf, 42)
    loads = [start + i * stride for i in range(count) if struct.unpack_from("<I", elf, start + i * stride)[0] == 1
             and struct.unpack_from("<I", elf, start + i * stride + 16)[0]]
    for address in (0x08004000, 0x08010000, 0x08080000):
        corrupt = elf.copy(); struct.pack_into("<I", corrupt, loads[0] + 12, address)
        try:
            packager.validate_elf(corrupt)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Reserved/out-of-bounds ELF load accepted: {address:#x}")
    upper, offsets = 0, set()
    for line in (BUILD / "ef_radix/ef_radix.hex").read_text().splitlines():
        record = bytes.fromhex(line[1:]); assert sum(record) % 256 == 0
        size, address, kind = record[0], int.from_bytes(record[1:3], "big"), record[3]
        assert len(record) == size + 5
        if kind == 4:
            upper = int.from_bytes(record[4:6], "big") << 16
        elif kind == 0:
            offset = upper + address - 0x08000000
            assert image[offset:offset + size] == record[4:4 + size]
            offsets.update(range(offset, offset + size))
    expected = {offset for start, size in packager.SEGMENTS for offset in range(start, start + size)}
    assert offsets == expected
    assert data["hardwareValidated"] is False and data["supportLevel"] == "experimental"
    print("PASS: real ELF rejects reserved/out-of-bounds loads; HEX exactly covers the three reviewed regions", flush=True)
    (BUILD / "radix-tests").mkdir(exist_ok=True)
    (BUILD / "radix-tests/report.json").write_text(json.dumps({
        "passed": True, "hardwareValidated": False, "imageSha256": hashlib.sha256(image).hexdigest(),
        "manifest": data,
    }, indent=2) + "\n")


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    with (BUILD / "radix-tests-build.log").open("w") as log:
        run(["make", "--jobserver-style=pipe", "-j2", "ef_radix", f"BUILD_DIR={BUILD}", "CCACHE_BIN="],
            stdout=log, stderr=subprocess.STDOUT)
    host_checks()
    package_checks()
    print(f"PASS: actual firmware built. Evidence: {BUILD}/radix-tests/report.json. No controller accessed.", flush=True)


if __name__ == "__main__":
    main()
