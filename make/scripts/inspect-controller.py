#!/usr/bin/env python3
"""Read a connected Betaflight controller's USB and MSP identity without changing firmware or settings."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys


USB_ROOT = Path("/sys/bus/usb/devices")
TTY_ROOT = Path("/sys/class/tty")
# A bridge is only a probe candidate; MSP, never its USB name, identifies a board.
USB_IDS = {("0483", "5740"), ("10c4", "ea60")}


def candidates():
    devices = []
    for device in sorted(USB_ROOT.glob("*")):
        vendor, product = device / "idVendor", device / "idProduct"
        if not vendor.exists() or not product.exists():
            continue
        if (vendor.read_text().strip(), product.read_text().strip()) not in USB_IDS:
            continue
        for tty in sorted(TTY_ROOT.glob("tty*")):
            if tty.name.startswith(("ttyACM", "ttyUSB")) and device.resolve() in tty.resolve().parents:
                devices.append((device, Path("/dev") / tty.name))
    return devices


def select_device(devices, port=None):
    if port is not None:
        devices = [(device, node) for device, node in devices if node.resolve() == port.resolve()]
    if not devices:
        raise RuntimeError("No matching STM32 or CP2102 serial controller found. Connect USB with BOOT released; "
                           "close other controller applications before inspecting.")
    if len(devices) != 1:
        raise RuntimeError("Multiple serial candidates found; select one with --port: "
                           + ", ".join(str(node) for _, node in devices))
    return devices[0]


def identity(replies, device, port):
    if (len(replies.get(1, b"")) != 3 or len(replies.get(2, b"")) != 4
            or len(replies.get(3, b"")) != 3 or len(replies.get(4, b"")) < 6):
        raise RuntimeError("Incomplete MSP identity. Disconnect the controller in other applications, then retry.")
    if replies[1][1] != 1:
        raise RuntimeError("Unsupported MSP API major; board identity was not accepted.")
    variant, board = replies[2].decode("ascii"), replies[4][:4].decode("ascii")
    if not all(re.fullmatch(r"[A-Z0-9]{4}", item) for item in (variant, board)):
        raise RuntimeError("Invalid MSP firmware or board identifier.")
    info = replies[4]
    result = {
        "usbPath": device.name,
        "usbVendorId": (device / "idVendor").read_text().strip(),
        "usbProductId": (device / "idProduct").read_text().strip(),
        "usbProduct": (device / "product").read_text().strip() if (device / "product").exists() else "",
        "port": str(port),
        "variant": variant,
        "version": ".".join(map(str, replies[3])),
        "api": ".".join(map(str, replies[1])),
        "boardIdentifier": board,
        "hardwareRevision": int.from_bytes(info[4:6], "little"),
        "boardInfoHex": info.hex(),
    }
    if len(info) >= 9 and info[8]:
        if len(info) < 9 + info[8]:
            raise RuntimeError("Truncated MSP target name.")
        target = info[9:9 + info[8]].decode("ascii")
        if not re.fullmatch(r"[A-Z0-9_-]+", target):
            raise RuntimeError("Invalid MSP target name.")
        result["target"] = target
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=Path, help="Select a candidate, e.g. /dev/ttyUSB0")
    parser.add_argument("--output", type=Path, help="Also save the identity as JSON")
    args = parser.parse_args()
    device, port = select_device(candidates(), args.port)
    spec = importlib.util.spec_from_file_location("controller_restore", Path(__file__).with_name("restore-board.py"))
    restore = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = restore
    spec.loader.exec_module(restore)
    # The existing restore utility's probe sends only read-only API, variant,
    # version and board-info requests and restores the serial settings on exit.
    replies = restore.query_msp(port)
    output = json.dumps(identity(replies, device, port), indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end="")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        sys.exit(f"Controller inspection failed: {error}")
