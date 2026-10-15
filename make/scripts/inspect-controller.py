#!/usr/bin/env python3
"""Read a connected Betaflight controller's USB and MSP identity without changing firmware or settings."""
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
from pathlib import Path
import sys


def main():
    devices = []
    for device in Path("/sys/bus/usb/devices").glob("*"):
        vendor, product = device / "idVendor", device / "idProduct"
        if not vendor.exists() or (vendor.read_text().strip(), product.read_text().strip()) != ("0483", "5740"):
            continue
        ports = [tty for tty in Path("/sys/class/tty").glob("ttyACM*")
                 if device.resolve() in tty.resolve().parents]
        for port in ports:
            devices.append((device, Path("/dev") / port.name))
    if len(devices) != 1:
        raise RuntimeError("Connect one Betaflight USB controller before inspecting its identity.")
    device, port = devices[0]
    spec = importlib.util.spec_from_file_location("controller_restore", Path(__file__).with_name("restore-board.py"))
    restore = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = restore
    spec.loader.exec_module(restore)
    # The existing restore utility's probe sends only read-only API, variant,
    # version and board-info requests and restores the serial settings on exit.
    replies = restore.query_msp(port)
    if not all(command in replies for command in (1, 2, 3, 4)):
        raise RuntimeError("Incomplete MSP identity. Disconnect the controller in other applications, then retry.")
    info = replies[4]
    result = {
        "usbPath": device.name,
        "usbProduct": (device / "product").read_text().strip(),
        "port": str(port),
        "variant": replies[2].decode("ascii"),
        "version": ".".join(map(str, replies[3])),
        "api": ".".join(map(str, replies[1])),
        "boardIdentifier": info[:4].decode("ascii"),
        "hardwareRevision": int.from_bytes(info[4:6], "little"),
        "boardInfoHex": info.hex(),
    }
    if len(info) >= 9 and info[8] and len(info) >= 9 + info[8]:
        result["target"] = info[9:9 + info[8]].decode("ascii")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        sys.exit(f"Controller inspection failed: {error}")
