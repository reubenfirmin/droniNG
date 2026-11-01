#!/usr/bin/env python3
"""Build and verify the original F3 firmware without accessing a controller."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/qt612-release")
    build = parser.parse_args().build.resolve()
    evidence = build / "spracingf3-tests"
    evidence.mkdir(parents=True, exist_ok=True)
    with (evidence / "build.log").open("w") as log:
        subprocess.run(["make", "--jobserver-style=pipe", "-j2", "ef_spracingf3", f"BUILD_DIR={build}",
                        "CCACHE_BIN="], cwd=ROOT, check=True, stdout=log, stderr=subprocess.STDOUT)
    subprocess.run(["python3", "make/scripts/spracingf3-dry-run.py"], cwd=ROOT, check=True)
    packager = module("f3_package", ROOT / "make/scripts/package-imported-board.py")
    manifest, image = packager.manifest(build, "spracingf3")
    elf = (build / "fw_spracingf3/fw_spracingf3.elf").read_bytes()
    headers = struct.unpack_from("<I", elf, 28)[0]
    stride, count = struct.unpack_from("<HH", elf, 42)
    load = next(headers + i * stride for i in range(count)
                if struct.unpack_from("<I", elf, headers + i * stride)[0] == 1
                and struct.unpack_from("<I", elf, headers + i * stride + 16)[0])
    for address in (0x08001000, 0x08005000, 0x08040000):
        damaged = bytearray(elf)
        struct.pack_into("<I", damaged, load + 12, address)
        try:
            packager.validate_elf(damaged, "spracingf3")
        except ValueError:
            pass
        else:
            raise AssertionError(f"Reserved/out-of-bounds ELF load accepted: {address:#x}")
    upper, offsets = 0, set()
    for line in (build / "ef_spracingf3/ef_spracingf3.hex").read_text().splitlines():
        record = bytes.fromhex(line[1:])
        size, address, kind = record[0], int.from_bytes(record[1:3], "big"), record[3]
        assert len(record) == size + 5 and sum(record) % 256 == 0
        if kind == 4:
            upper = int.from_bytes(record[4:6], "big") << 16
        elif kind == 0:
            offset = upper + address - 0x08000000
            assert image[offset:offset + size] == record[4:4 + size]
            offsets.update(range(offset, offset + size))
    assert offsets == {i for start, size in packager.segments_for(packager.reviewed_profile("spracingf3"))
                       for i in range(start, start + size)}
    prefix = ROOT / "tools/gcc-arm-none-eabi-6-2017-q2-update/bin/arm-none-eabi-"
    disassembly = subprocess.check_output([str(prefix) + "objdump", "-d",
        str(build / "fw_spracingf3/fw_spracingf3.elf")], text=True)
    startup = module("startup_checks", ROOT / "flight/tests/run_omnibusf4sd.py")
    startup.validate_startup(disassembly)
    startup.validate_vectors(disassembly)
    epilogue = re.search(r"^[0-9a-f]+ <_port_irq_epilogue>:\n(.*?)(?=^[0-9a-f]+ <|\Z)", disassembly, re.M | re.S)
    assert epilogue and "vmrs" in epilogue[1]
    symbols = subprocess.check_output([str(prefix) + "nm", str(build / "fw_spracingf3/fw_spracingf3.elf")], text=True)
    addresses = {name: int(address, 16) for address, name in re.findall(r"^([0-9a-f]+) \w (\S+)$", symbols, re.M)}
    heap = addresses["_eheap"] - addresses["_sheap"]
    assert heap >= 24576
    (evidence / "report.json").write_text(json.dumps({"passed": True, "hardwareValidated": False,
        "imageSha256": hashlib.sha256(image).hexdigest(), "initialHeapBytes": heap,
        "fpuStartupAndRtos": True, "physicalFlashVectors": True, "manifest": manifest}, indent=2) + "\n")
    print(f"PASS: actual F303 image, reserved regions, FPU startup/RTOS, physical vectors; {heap} initial heap bytes")
    print(f"Evidence: {evidence / 'report.json'}. No controller accessed.")


if __name__ == "__main__":
    main()
