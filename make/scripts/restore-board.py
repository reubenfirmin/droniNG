#!/usr/bin/env python3
"""Restore a reference controller's original firmware for setup/UI testing.

No arguments: download and validate the pinned image without touching the board.
--flash: back up internal flash, restore clean firmware, read back, boot and check MSP.
Requires Python 3 and dfu-util 0.11; no Python packages or root privileges.
"""

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shlex
import shutil
import struct
import subprocess
import sys
import termios
import time
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "shared/firmware/test-restores.json"
USB_ROOT = Path("/sys/bus/usb/devices")
USB_PATH = re.compile(r"[0-9]+-[0-9]+(?:\.[0-9]+)*\Z")
DFU_ROW = re.compile(
    r'Found DFU: \[0483:df11\].*cfg=(\d+), intf=(\d+), '
    r'path="([0-9]+-[0-9]+(?:\.[0-9]+)*)", alt=(\d+), name="([^"]+)", serial="([^"]*)"')


class RestoreError(Exception):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def decode_hex(data, profile):
    """Validate Intel HEX records and construct the complete clean flash image."""
    base = int(profile["flashBase"], 0)
    image = bytearray(b"\xff" * profile["flashBytes"])
    written = bytearray(len(image))
    upper, eof = 0, False
    try:
        for number, line in enumerate(data.decode("ascii").splitlines(), 1):
            if not line or eof or not re.fullmatch(r":[0-9A-Fa-f]+", line):
                raise ValueError(f"invalid record on line {number}")
            record = bytes.fromhex(line[1:])
            if len(record) < 5 or len(record) != record[0] + 5 or sum(record) % 256:
                raise ValueError(f"invalid length/checksum on line {number}")
            length, address, kind = record[0], int.from_bytes(record[1:3], "big"), record[3]
            payload = record[4:-1]
            if kind == 0:
                offset = upper + address - base
                if not length or offset < 0 or offset + length > len(image):
                    raise ValueError("data outside the declared internal flash")
                if any(written[offset:offset + length]):
                    raise ValueError("overlapping image records")
                image[offset:offset + length] = payload
                written[offset:offset + length] = b"\x01" * length
            elif kind == 4 and length == 2 and address == 0:
                upper = int.from_bytes(payload, "big") << 16
            elif kind == 5 and length == 4 and address == 0:
                if int.from_bytes(payload, "big") != base:
                    raise ValueError("unexpected image entry address")
            elif kind == 1 and length == 0 and address == 0:
                eof = True
            else:
                raise ValueError(f"unsupported Intel HEX record type {kind}")
        if not eof or not all(written[:8]):
            raise ValueError("missing end record or reset vectors")
        stack, reset = struct.unpack_from("<II", image)
        ranges = [(int(a, 0), int(b, 0)) for a, b in profile["stackRanges"]]
        if stack % 8 or not any(a < stack <= b for a, b in ranges):
            raise ValueError("invalid initial stack pointer")
        offset = (reset & ~1) - base
        if not reset & 1 or not 0 <= offset < len(image) - 1 or not all(written[offset:offset + 2]):
            raise ValueError("reset vector does not point into the firmware")
    except (UnicodeError, ValueError) as error:
        raise RestoreError(f"Firmware HEX validation failed: {error}") from error
    return bytes(image)


def prepare_image(profile, directory, supplied=None):
    path = supplied or directory / profile["image"]
    if not path.exists():
        if supplied:
            raise RestoreError(f"Firmware file does not exist: {path}")
        print(f"Downloading {profile['image']} from its official release…", flush=True)
        request = urllib.request.Request(profile["url"], headers={"User-Agent": "DroniNG-test-restore"})
        with urllib.request.urlopen(request, timeout=45) as response:
            data = response.read(profile["bytes"] + 1)
    else:
        data = path.read_bytes()
    if len(data) != profile["bytes"] or digest(data) != profile["sha256"]:
        raise RestoreError(f"Firmware checksum differs from the pinned release: {path}")
    image = decode_hex(data, profile)
    if digest(image) != profile["flashSha256"]:
        raise RestoreError("The decoded full-flash image differs from its checksum pin.")
    if not path.exists():
        path.write_bytes(data)
    return image


@dataclass(frozen=True)
class DfuDevice:
    path: str
    serial: str
    layout: str
    configuration: int
    interface: int
    alternate: int

    def selector(self):
        args = ["-d", ",0483:df11", "-p", self.path, "-c", str(self.configuration),
                "-i", str(self.interface), "-a", str(self.alternate)]
        return args + (["-S", "," + self.serial] if self.serial else [])


def dfu_devices(listing):
    devices = []
    for row in DFU_ROW.finditer(listing):
        cfg, interface, path, alternate, layout, serial = row.groups()
        if layout.startswith("@Internal Flash"):
            devices.append(DfuDevice(path, serial, layout, int(cfg), int(interface), int(alternate)))
    return devices


def validate_geometry(device, profile):
    try:
        label, base, layout = device.layout.split("/")
        sectors = []
        for part in layout.split(","):
            match = re.fullmatch(r"\s*(\d+)\*(\d+)([BKMbkm])g\s*", part)
            if not match:
                raise ValueError("flash is not readable, erasable and writable")
            count, size, unit = match.groups()
            sectors.append([int(count), int(size) * {"B": 1, "K": 1024, "M": 1048576}[unit.upper()]])
        if (label.strip() != "@Internal Flash" or int(base, 0) != int(profile["flashBase"], 0)
                or sectors != profile["flashSectors"]
                or sum(count * size for count, size in sectors) != profile["flashBytes"]):
            raise ValueError("flash geometry does not match the board profile")
    except ValueError as error:
        raise RestoreError(f"DFU device is incompatible: {error}. No flash was erased.") from error


class Log:
    def __init__(self, directory, profile):
        self.directory = directory
        self.report = {"board": profile["name"], "target": profile["target"],
                       "version": profile["version"], "status": "preparing", "events": []}

    def write(self, message):
        print(message, flush=True)
        with (self.directory / "restore.log").open("a") as file:
            file.write(message + "\n")

    def update(self, status, **values):
        self.report.update(status=status, **values)
        self.report["events"].append({"status": status, "utc": datetime.now(timezone.utc).isoformat()})
        (self.directory / "result.json").write_text(json.dumps(self.report, indent=2) + "\n")


def usb_devices():
    found = {}
    for path in USB_ROOT.iterdir():
        if USB_PATH.fullmatch(path.name):
            try:
                found[path.name] = ((path / "idVendor").read_text().strip()
                                    + ":" + (path / "idProduct").read_text().strip())
            except FileNotFoundError:
                pass  # Device unplugged while reading the inventory.
    return found


def controller_path(profile, requested=None):
    if requested:
        return requested
    paths = [path for path, usb in usb_devices().items()
             if usb in profile["runtimeUsb"] or usb == "0483:df11"]
    if len(paths) > 1:
        raise RestoreError("Multiple controllers are attached. Select one with --usb-path "
                           + " or --usb-path ".join(paths) + ".")
    return paths[0] if paths else None


def ensure_gcs_closed():
    for process in Path("/proc").glob("[0-9]*/comm"):
        try:
            if process.read_text().strip() in ("droning-gcs", "dronin-gcs"):
                raise RestoreError("Close DroniNG before restoring so it cannot reconnect during flashing.")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass


def find_helper(supplied=None):
    candidates = [supplied] if supplied else [
        ROOT / "build/qt612-release/ground/gcs/bin/dfu-util",
        ROOT / "tools/firmware-installer/bin/dfu-util",
        shutil.which("dfu-util")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return Path(candidate).resolve()
    raise RestoreError("dfu-util is missing. Build it with: make firmware_installer_runtime")


class DfuRunner:
    def __init__(self, helper, log):
        self.helper, self.log = helper, log
        self.environment = os.environ.copy()
        prefix = helper.parent.parent
        libraries = [prefix / "lib/droning", prefix / "lib"]
        self.environment["LD_LIBRARY_PATH"] = ":".join(
            [str(path) for path in libraries]
            + ([self.environment["LD_LIBRARY_PATH"]] if self.environment.get("LD_LIBRARY_PATH") else []))

    def __call__(self, arguments):
        command = [str(self.helper)] + arguments
        self.log.write("$ " + shlex.join(command))
        try:
            result = subprocess.run(command, env=self.environment, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        except subprocess.TimeoutExpired as error:
            raise RestoreError("DFU timed out. Reconnect while holding BOOT and rerun the restore.") from error
        output = result.stdout
        with (self.log.directory / "restore.log").open("a") as file:
            file.write(output + "\n")
        if "LIBUSB_ERROR_ACCESS" in output or "Permission denied" in output:
            rules = next((path for path in [self.helper.parent.parent / "rules.udev",
                         ROOT / "build/qt612-release/shared/usb_ids/droning.udev"] if path.is_file()), None)
            fix = ("Run this once, then reconnect in BOOT mode:\n"
                   f"sudo install -m 0644 {shlex.quote(str(rules))} /etc/udev/rules.d/45-droning.rules"
                   " && sudo udevadm control --reload-rules") if rules else "Install the USB rules from your DroniNG package."
            raise RestoreError("Linux denied access to the controller. " + fix)
        if result.returncode:
            raise RestoreError(f"dfu-util failed ({result.returncode}):\n{output.strip()}")
        return output


def selected_device(run, expected_path):
    found = dfu_devices(run(["-d", ",0483:df11", "-l"]))
    if expected_path:
        found = [device for device in found if device.path == expected_path]
    if len(found) > 1:
        raise RestoreError("Multiple DFU targets found. Disconnect other boards or pass --usb-path.")
    return found[0] if found else None


def wait_for_dfu(run, expected_path, profile, timeout):
    deadline = time.monotonic() + timeout
    while True:
        device = selected_device(run, expected_path)
        if device:
            validate_geometry(device, profile)
            return device
        if time.monotonic() >= deadline:
            raise RestoreError("The controller did not appear in STM32 DFU mode. "
                               "Check its BOOT procedure and USB data cable. Manual: " + profile["manual"])
        time.sleep(1)


def restore_flash(run, device, profile, image, log):
    """No erase/write until the backup and a fresh device check have succeeded."""
    validate_geometry(device, profile)
    base = profile["flashBase"]
    span = f"{base}:{profile['flashBytes']}"
    selector = device.selector()
    log.update("backing-up", device=asdict(device), flashSha256=digest(image))
    log.write("Backing up the controller's complete internal flash…")
    backup_path = log.directory / "before.bin"
    run(selector + ["-s", span, "-U", str(backup_path)])
    backup = backup_path.read_bytes()
    if len(backup) != profile["flashBytes"]:
        raise RestoreError("Flash backup was incomplete. No flash was erased.")
    with backup_path.open("rb") as file:
        os.fsync(file.fileno())
    log.update("backup-complete", backupSha256=digest(backup))
    if selected_device(run, device.path) != device:
        raise RestoreError("The DFU device changed during backup. No flash was erased.")
    ensure_gcs_closed()
    image_path = log.directory / "restore.bin"
    image_path.write_bytes(image)
    # Writing a full-chip image makes dfu-util erase every declared flash sector.
    # Unused firmware/config space is 0xff. No option bytes or ROM are touched.
    log.update("writing")
    log.write("Restoring firmware and clean settings… Keep USB connected.")
    run(selector + ["-s", base, "-D", str(image_path)])
    log.update("verifying")
    log.write("Reading back all internal flash to verify every byte…")
    readback_path = log.directory / "readback.bin"
    run(selector + ["-s", span, "-U", str(readback_path)])
    readback = readback_path.read_bytes()
    if readback != image:
        raise RestoreError("Flash readback differs from the restore image. "
                           "The board remains in DFU; rerun the restore before reconnecting normally.")
    log.update("flash-verified", readbackSha256=digest(readback))
    log.write("Flash verified. Starting Betaflight…")
    # Leave only after verification; a one-byte upload avoids a second erase/write.
    run(selector + ["-s", f"{base}:1:leave", "-U", str(log.directory / "leave.bin")])


def msp_frames(buffer):
    """Consume complete MSP v1 responses, retaining partial frames across reads."""
    replies = {}
    while buffer:
        start = buffer.find(b"$M>")
        if start < 0:
            del buffer[:-2]
            break
        del buffer[:start]
        if len(buffer) < 6 or len(buffer) < buffer[3] + 6:
            break
        length, command = buffer[3:5]
        frame = buffer[:length + 6]
        checksum = 0
        for value in frame[3:-1]:
            checksum ^= value
        if checksum == frame[-1]:
            replies[command] = bytes(frame[5:-1])
            del buffer[:length + 6]
        else:
            del buffer[0]
    return replies


def query_msp(port, timeout=3):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, termios.TIOCEXCL)
        original = termios.tcgetattr(fd)
        settings = termios.tcgetattr(fd)
        settings[:6] = [0, 0, termios.CS8 | termios.CREAD | termios.CLOCAL, 0,
                       termios.B115200, termios.B115200]
        settings[6][termios.VMIN], settings[6][termios.VTIME] = 0, 0
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        termios.tcflush(fd, termios.TCIOFLUSH)
        deadline, next_request = time.monotonic() + timeout, 0
        replies, buffer = {}, bytearray()
        while time.monotonic() < deadline:
            if time.monotonic() >= next_request:
                for command in (1, 2, 3, 4):  # API, variant, version, board info; all read-only.
                    if command not in replies:
                        os.write(fd, b"$M<\x00" + bytes([command, command]))
                next_request = time.monotonic() + 0.5
            if select.select([fd], [], [], 0.1)[0]:
                buffer.extend(os.read(fd, 1024))
                replies.update(msp_frames(buffer))
            if all(command in replies for command in (1, 2, 3, 4)):
                return replies
        return replies
    finally:
        if "original" in locals():
            termios.tcsetattr(fd, termios.TCSANOW, original)
        os.close(fd)


def check_identity(replies, profile):
    expected = {1: bytes(profile["mspApi"]), 2: b"BTFL",
                3: bytes(map(int, profile["version"].split(".")))}
    if any(replies.get(command) != value for command, value in expected.items()):
        raise RestoreError("The running firmware does not report the expected Betaflight version/API.")
    if len(replies.get(4, b"")) < 6 or replies[4][:4] != profile["mspBoard"].encode("ascii"):
        raise RestoreError("The running firmware reports a different board target.")
    return {"variant": "BTFL", "version": profile["version"], "board": profile["mspBoard"],
            "api": ".".join(map(str, profile["mspApi"][1:]))}


def verify_boot(path, profile, timeout=30):
    deadline, last_error = time.monotonic() + timeout, "No Betaflight serial interface yet."
    while time.monotonic() < deadline:
        if usb_devices().get(path) == profile["restoredUsb"]:
            device = (USB_ROOT / path).resolve()
            ports = [Path("/dev") / tty.name for tty in Path("/sys/class/tty").glob("ttyACM*")
                     if device in (tty / "device").resolve().parents]
            if len(ports) == 1:
                try:
                    return check_identity(query_msp(ports[0]), profile)
                except (RestoreError, OSError, termios.error) as error:
                    last_error = str(error)
        time.sleep(1)
    raise RestoreError("Could not confirm Betaflight startup: " + last_error
                       + " Release BOOT, unplug/reconnect USB, then run this script with --check "
                       + f"--usb-path {path}. Do not repeat the flash just to check boot.")


def main():
    profiles = json.loads(CATALOG.read_text())["boards"]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--flash", action="store_true", help="restore firmware with clean settings")
    mode.add_argument("--check", action="store_true", help="check running Betaflight identity only")
    parser.add_argument("--board", choices=profiles, default="hglrc-f4-v6pro")
    parser.add_argument("--usb-path", help="USB topology to restore, e.g. 3-1.4.3.4")
    parser.add_argument("--dfu-util", type=Path, help="override the bundled dfu-util executable")
    parser.add_argument("--image", type=Path, help="offline copy of the same pinned official HEX")
    parser.add_argument("--output", type=Path, default=ROOT / "build/firmware-restore", help="image cache and per-run backups/logs")
    parser.add_argument("--wait", type=int, default=120, help="seconds to wait for hardware BOOT mode")
    args = parser.parse_args()
    if args.usb_path and not USB_PATH.fullmatch(args.usb_path):
        parser.error("--usb-path must be a USB topology such as 3-1.4.3.4")
    if args.wait < 1:
        parser.error("--wait must be positive")
    profile = profiles[args.board]
    print(f"{profile['name']}: restore {profile['firmware']} {profile['version']} ({profile['target']}).", flush=True)
    if args.flash or args.check:
        ensure_gcs_closed()
    path = controller_path(profile, args.usb_path)
    if args.check:
        if not path:
            raise RestoreError("No controller attached. Reconnect USB with BOOT released and retry.")
        identity = verify_boot(path, profile)
        print(f"Verified: Betaflight {identity['version']}, {identity['board']}, MSP API {identity['api']}.")
        return
    args.output.mkdir(parents=True, exist_ok=True)
    image = prepare_image(profile, args.output, args.image)
    print(f"Image verified: {profile['sha256']}\n"
          f"Restore covers {len(image)} bytes at {profile['flashBase']}, including clean settings.\n"
          "The factory STM32 ROM bootloader stays available; the dRonin flash bootloader is removed.")
    if not args.flash:
        print("Ready. To restore: python3 make/scripts/restore-board.py --flash")
        if path:
            print(f"Detected controller USB path: {path}")
        return
    helper = find_helper(args.dfu_util)
    directory = args.output / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    directory.mkdir()
    log = Log(directory, profile)
    run = DfuRunner(helper, log)
    log.write(f"Backups and diagnostics: {directory}")
    try:
        if "dfu-util 0.11" not in run(["-V"]):
            raise RestoreError("Use the bundled dfu-util 0.11 for this restore.")
        log.update("waiting-for-dfu", expectedUsbPath=path)
        log.write(profile["bootInstructions"])
        log.write(f"Waiting up to {args.wait}s for STM32 DFU" + (f" at {path}…" if path else "…"))
        device = wait_for_dfu(run, path, profile, args.wait)
        restore_flash(run, device, profile, image, log)
        log.update("checking-boot")
        log.write("Checking Betaflight version and board target on the same USB port…")
        identity = verify_boot(device.path, profile)
        log.update("complete", identity=identity)
        log.write(f"Restore complete: Betaflight {identity['version']}, {identity['board']}, MSP API {identity['api']}. "
                  "Open DroniNG to test connection and installation again.")
    except (RestoreError, OSError, KeyboardInterrupt) as error:
        log.update("failed", error=str(error) or "Interrupted")
        log.write(f"Restore stopped: {error or 'Interrupted'}. Diagnostics: {directory}")
        raise


if __name__ == "__main__":
    try:
        main()
    except (RestoreError, OSError, ValueError) as error:
        print(f"Restore failed: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("Restore interrupted. If writing had started, reconnect in BOOT mode and rerun.", file=sys.stderr)
        sys.exit(130)
