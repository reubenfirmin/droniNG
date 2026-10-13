#!/usr/bin/env python3
"""Validate and package a native imported target; never access a controller."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
SEGMENTS = ((0, 16384), (49152, 16384), (131072, 393216))


def reviewed_profile(profile_id):
    accepted = {"radix": (224, 524288, "radix-f446-rom-v1"),
                "omnibusf4sd": (225, 1048576, "omnibusf4sd-f405-rom-v1")}
    if profile_id not in accepted:
        raise ValueError("No reviewed package layout for this profile")
    board, size, layout_id = accepted[profile_id]
    profile = json.loads((ROOT / f"shared/firmware/profiles/{profile_id}.json").read_bytes())
    layout = {"vectors": [0, 16384], "settings": [16384, 32768], "identity": [49152, 16384],
              "autotune": [65536, 65536], "application": [131072, size - 131072], "descriptorOffset": size - 100}
    if (profile["id"] != profile_id or profile["boardType"] != board or profile["boardRevision"] != 1
            or profile["flashBytes"] != size or profile["ramBytes"] != 131072
            or profile["layoutId"] != layout_id or profile["layout"] != layout):
        raise ValueError("Profile memory/identity requires explicit package review")
    return profile


def segments_for(profile):
    return ((0, 16384), (49152, 16384), tuple(profile["layout"]["application"]))


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_elf(elf, profile_id="radix"):
    profile = reviewed_profile(profile_id)
    segments = segments_for(profile)
    descriptor_address = 0x08000000 + profile["layout"]["descriptorOffset"]
    if elf[:7] != b"\x7fELF\x01\x01\x01" or struct.unpack_from("<H", elf, 18)[0] != 40:
        raise ValueError("Expected a little-endian ARM ELF32 executable")
    entry, start = struct.unpack_from("<II", elf, 24)
    stride, count = struct.unpack_from("<HH", elf, 42)
    if not (entry & 1 and 0x08020000 <= entry < descriptor_address):
        raise ValueError("ELF entry is outside the application")
    used = 0
    for index in range(count):
        kind, offset, virtual, physical, size, memory, flags, alignment = struct.unpack_from("<8I", elf, start + index * stride)
        if kind != 1:
            continue
        in_ram = 0x20000000 <= virtual and virtual + memory <= 0x20000000 + profile["ramBytes"]
        in_flash = any(0x08000000 + a <= virtual and virtual + memory <= 0x08000000 + a + b for a, b in segments)
        if memory < size or not (in_ram or in_flash):
            raise ValueError("ELF memory region is outside the reviewed SRAM/flash")
        if not size:
            continue
        relative = physical - 0x08000000
        if offset + size > len(elf) or not any(a <= relative and relative + size <= a + b for a, b in segments):
            raise ValueError("ELF load region crosses reserved settings/autotune or flash bounds")
        if relative >= 131072 and relative + size > profile["layout"]["descriptorOffset"]:
            raise ValueError("ELF overlaps the descriptor")
        used += size
    return used


def manifest(build, profile_id="radix"):
    profile = reviewed_profile(profile_id)
    fw = build / f"fw_{profile_id}"
    size, descriptor_offset = profile["flashBytes"], profile["layout"]["descriptorOffset"]
    segments = segments_for(profile)
    report = json.loads((fw / "import-report.json").read_bytes())
    digest = hashlib.sha256((ROOT / f"shared/firmware/profiles/{profile_id}.json").read_bytes() +
                            (ROOT / f"flight/Betaflight/{profile_id}-source-manifest.json").read_bytes()).digest()
    if report["profile"] != profile or report["profileSha256"] != digest.hex():
        raise ValueError("Generated profile is stale; rebuild firmware")
    image = (fw / f"fw_{profile_id}.tlfw").read_bytes()
    raw = (fw / f"fw_{profile_id}.bin").read_bytes()
    descriptor = (fw / f"fw_{profile_id}.bin.firmwareinfo.bin").read_bytes()
    version = load_module("version_info", ROOT / "make/scripts/version-info.py")
    uavo = version.GetHashofDirs(str(ROOT / "shared/uavobjectdefinition"), raw=1)
    if (len(image) != size or not 131080 <= len(raw) <= descriptor_offset or len(descriptor) != 100
            or image[:len(raw)] != raw or image[descriptor_offset:] != descriptor
            or descriptor[:4] != b"TlFw" or descriptor[12:14] != bytes((profile["boardType"], 1))
            or descriptor[40:60] != hashlib.sha1(raw).digest() or descriptor[60:80].hex() != uavo):
        raise ValueError("Firmware bounds, descriptor, board identity or UAVObjects mismatch")
    info = struct.pack("<IBBBBIIIIII", 0xbdbdbdbd, profile["boardType"], 1, 0, 1,
                       0x08000000, descriptor_offset, 0x08000000 + descriptor_offset, 100, 0x08004000, 32768)
    identity = struct.pack("<4sI32s32s32s", b"BPrf", 1, profile_id.encode(), profile["layoutId"].encode(), digest)
    if image[49152:49184] != info or image[49184:49288] != identity:
        raise ValueError("Immutable board/profile identity mismatch")
    stack, reset = struct.unpack_from("<II", image)
    if not (0x20000000 < stack <= 0x20020000 and not stack & 7
            and reset & 1 and 0x08020000 <= reset < 0x08000000 + len(raw)):
        raise ValueError("Invalid direct-boot vector")
    used = validate_elf((fw / f"fw_{profile_id}.elf").read_bytes(), profile_id)
    return {
        "schemaVersion": 1, "target": profile_id, "boardType": profile["boardType"], "boardRevision": 1,
        "layoutId": profile["layoutId"], "profileId": profile_id, "profileSha256": digest.hex(),
        "supportLevel": "experimental", "installable": True, "hardwareValidated": False,
        "image": {"file": f"ef_{profile_id}.bin", "base": 0x08000000, "size": len(image),
                  "sha256": hashlib.sha256(image).hexdigest()},
        "firmware": {"offset": 0, "size": len(raw), "descriptorOffset": descriptor_offset,
                     "sha1": hashlib.sha1(raw).hexdigest(), "uavoSha1": uavo},
        "identityOffset": 49152,
        "programSegments": [{"offset": offset, "size": size} for offset, size in segments],
        "flashLoadBytes": used, "limitations": profile["limitations"],
    }, image


def intel_hex(image, segments=SEGMENTS):
    def record(address, kind, data):
        payload = bytes((len(data),)) + struct.pack(">H", address) + bytes((kind,)) + data
        return ":" + (payload + bytes((-sum(payload) & 255,))).hex().upper()
    lines, upper = [], None
    for offset, size in segments:
        for position in range(offset, offset + size, 16):
            address = 0x08000000 + position
            if address >> 16 != upper:
                upper = address >> 16
                lines.append(record(0, 4, struct.pack(">H", upper)))
            lines.append(record(address & 65535, 0, image[position:position + 16]))
    return "\n".join(lines + [":00000001FF"]) + "\n"


def package(build, profile_id):
    data, image = manifest(build, profile_id)
    destination = build / f"ef_{profile_id}"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / f"ef_{profile_id}.bin").write_bytes(image)
    (destination / f"ef_{profile_id}.hex").write_text(intel_hex(image, segments_for(reviewed_profile(profile_id))))
    (destination / f"ef_{profile_id}.manifest.json").write_text(json.dumps(data, indent=2) + "\n")
    print(f"PASS: actual {profile_id} image: {data['flashLoadBytes']} load bytes, three writable regions; settings/autotune preserved")
    print(f"Wrote {destination}/ef_{profile_id}.hex")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--profile", default="radix")
    args = parser.parse_args()
    package(args.build.resolve(), args.profile)
