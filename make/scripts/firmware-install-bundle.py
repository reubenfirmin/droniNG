#!/usr/bin/env python3
"""Bundle full-flash images and their build-derived identities for Linux installation."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct


def board_layout(root, target):
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", target):
        raise ValueError("Invalid firmware target")
    text = (root / "flight/targets" / target / "board-info/board-info.mk").read_text()
    return {key: int(value, 0) for key, value in re.findall(
        r"^([A-Z_]+)\s*:?=\s*(0x[0-9a-fA-F]+|[0-9]+)\b", text, re.M)}


def image_manifest(root, build, target):
    layout = board_layout(root, target)
    image = (build / f"ef_{target}/ef_{target}.bin").read_bytes()
    firmware = (build / f"fw_{target}/fw_{target}.bin").read_bytes()
    descriptor = (build / f"fw_{target}/fw_{target}.bin.firmwareinfo.bin").read_bytes()
    base, size = layout["EF_BANK_BASE"], layout["EF_BANK_SIZE"]
    offset = layout["FW_BANK_BASE"] - base
    desc_offset = offset + layout["FW_BANK_SIZE"] - layout["FW_DESC_SIZE"]
    spec = importlib.util.spec_from_file_location("version_info", root / "make/scripts/version-info.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    uavo = module.GetHashofDirs(str(root / "shared/uavobjectdefinition"), raw=1)
    if (len(image) != size or len(descriptor) != 100 or descriptor[:4] != b"TlFw"
            or descriptor[12:14] != bytes((layout["BOARD_TYPE"], layout["BOARD_REVISION"]))
            or descriptor[40:60] != hashlib.sha1(firmware).digest()
            or descriptor[60:80].hex() != uavo
            or image[offset:offset + len(firmware)] != firmware
            or image[desc_offset:desc_offset + 100] != descriptor):
        raise ValueError(f"{target}: full image, firmware, descriptor or UAVObjects do not match")
    # The bootloader's packed board-info blob is derived from the same target.
    info = struct.pack("<IBBBBIIIIII", 0xbdbdbdbd, layout["BOARD_TYPE"],
                       layout["BOARD_REVISION"], layout["BOOTLOADER_VERSION"], layout["HW_TYPE"],
                       layout["FW_BANK_BASE"], layout["FW_BANK_SIZE"] - 100,
                       base + desc_offset, 100, layout["EE_BANK_BASE"], layout["EE_BANK_SIZE"])
    info_offset = image.find(info, 0, layout["BL_BANK_SIZE"])
    if info_offset < 0 or image.find(info, info_offset + 1, layout["BL_BANK_SIZE"]) >= 0:
        raise ValueError(f"{target}: missing or ambiguous bootloader board info")
    return {
        "schemaVersion": 1, "target": target,
        "boardType": layout["BOARD_TYPE"], "boardRevision": layout["BOARD_REVISION"],
        "image": {"file": f"ef_{target}.bin", "base": base, "size": size,
                  "sha256": hashlib.sha256(image).hexdigest()},
        "bootloader": {"infoOffset": info_offset, "size": layout["BL_BANK_SIZE"]},
        "firmware": {"offset": offset, "size": len(firmware), "descriptorOffset": desc_offset,
                     "name": descriptor[14:40].split(b'\0', 1)[0].decode('ascii'),
                     "sha1": hashlib.sha1(firmware).hexdigest(), "uavoSha1": uavo},
        "buildCommit": f"{struct.unpack('<I', descriptor[4:8])[0]:08x}",
    }


def bundle(root, build, runtime, targets):
    destination = build / "ground/gcs/firmware"
    destination.mkdir(parents=True, exist_ok=True)
    for target in targets:
        manifest = image_manifest(root, build, target)
        for kind, extension in (("ef", "hex"), ("ef", "bin"), ("fw", "tlfw")):
            name = f"{kind}_{target}.{extension}"
            shutil.copy2(build / f"{kind}_{target}" / name, destination / name)
        (destination / f"ef_{target}.manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    binary = build / "ground/gcs/bin"
    libraries = build / "ground/gcs/lib/droning"
    binary.mkdir(parents=True, exist_ok=True)
    libraries.mkdir(parents=True, exist_ok=True)
    shutil.copy2(runtime / "bin/dfu-util", binary / "dfu-util")
    shutil.copy2((runtime / "lib/libusb-1.0.so.0").resolve(), libraries / "libusb-1.0.so.0")
    materials = build / "ground/gcs/share/firmware-installer"
    materials.mkdir(parents=True, exist_ok=True)
    sources = json.loads((root / "shared/firmware/installer-sources.json").read_text())
    pinned = {component["url"].rsplit("/", 1)[-1]: component["sha256"] for component in sources["components"]}
    # Always ship the small, complete upstream sources, their notices and pinned provenance.
    for archive, license_file in (("dfu-util-0.11.tar.gz", "dfu-util-0.11/COPYING"),
                                  ("libusb-1.0.29.tar.bz2", "libusb-1.0.29/COPYING")):
        if hashlib.sha256((root / "downloads" / archive).read_bytes()).hexdigest() != pinned[archive]:
            raise ValueError(f"Installer source checksum mismatch: {archive}")
        shutil.copy2(root / "downloads" / archive, materials / archive)
        shutil.copy2(root / "downloads" / license_file, materials / (archive + ".LICENSE"))
    shutil.copy2(root / "shared/firmware/installer-sources.json", materials / "sources.json")
    (materials / "README.txt").write_text(
        "The Linux firmware installer uses dfu-util 0.11 (GPL-2.0-or-later) and\n"
        "libusb 1.0.29 (LGPL-2.1-or-later). Complete upstream source archives and\n"
        "original copyright/license notices are supplied in this directory.\n"
        "libusb is dynamically linked and can be replaced independently.\n"
        "The build recipes and local image manifest generator are in the application\n"
        "source: make/firmware-installer.mk and make/scripts/firmware-install-bundle.py.\n"
        "sources.json records the upstream download URLs and SHA-256 checksums.\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("targets", nargs="+")
    args = parser.parse_args()
    bundle(args.root.resolve(), args.build.resolve(), args.runtime.resolve(), args.targets)


if __name__ == "__main__":
    main()
