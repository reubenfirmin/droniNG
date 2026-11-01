#!/usr/bin/env python3
"""Read-only controller discovery and identity checks; no physical USB access."""
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("inspect_controller", ROOT / "make/scripts/inspect-controller.py")
INSPECT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSPECT)


class ControllerInspectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.usb, self.tty = self.root / "usb", self.root / "tty"
        self.usb.mkdir(); self.tty.mkdir()

    def device(self, name, ids, tty=None):
        path = self.usb / name
        path.mkdir()
        for field, value in zip(("idVendor", "idProduct", "product"), (*ids, "USB device")):
            (path / field).write_text(value)
        if tty:
            interface = path / "interface" / tty
            interface.mkdir(parents=True)
            (self.tty / tty).symlink_to(interface)
        return path

    def test_native_and_cp2102_discovery_without_guessing_board_from_bridge(self):
        bridge = self.device("3-1", ("10c4", "ea60"), "ttyUSB0")
        native = self.device("3-2", ("0483", "5740"), "ttyACM0")
        self.device("3-3", ("9999", "5740"), "ttyACM1")
        self.device("3-4", ("10c4", "ea60"), "ttyS0")
        with mock.patch.multiple(INSPECT, USB_ROOT=self.usb, TTY_ROOT=self.tty):
            self.assertEqual(INSPECT.candidates(), [(bridge, Path("/dev/ttyUSB0")), (native, Path("/dev/ttyACM0"))])

    def test_multiple_candidates_need_explicit_selection(self):
        devices = [(self.root / "a", Path("/dev/ttyUSB0")), (self.root / "b", Path("/dev/ttyACM0"))]
        with self.assertRaisesRegex(RuntimeError, "Multiple.*--port"):
            INSPECT.select_device(devices)
        self.assertEqual(INSPECT.select_device(devices, Path("/dev/ttyUSB0")), devices[0])
        with self.assertRaisesRegex(RuntimeError, "No matching"):
            INSPECT.select_device(devices, Path("/dev/ttyUSB99"))

    def test_bridge_is_not_a_firmware_identity(self):
        bridge = self.device("3-1", ("10c4", "ea60"))
        with self.assertRaisesRegex(RuntimeError, "Incomplete MSP"):
            INSPECT.identity({}, bridge, Path("/dev/ttyUSB0"))

    def test_original_f3_accepts_the_real_six_byte_board_info(self):
        bridge = self.device("3-1", ("10c4", "ea60"))
        replies = {1: b"\x00\x01\x1f", 2: b"BTFL", 3: b"\x03\x01\x05", 4: b"SRF3\x00\x00"}
        result = INSPECT.identity(replies, bridge, Path("/dev/ttyUSB0"))
        self.assertEqual((result["version"], result["api"], result["boardIdentifier"]), ("3.1.5", "0.1.31", "SRF3"))
        self.assertNotIn("target", result)
        self.assertNotEqual(result["boardIdentifier"], "SPEV")

    def test_newer_full_target_still_supported(self):
        native = self.device("3-1", ("0483", "5740"))
        replies = {1: b"\0\1\x27", 2: b"BTFL", 3: b"\3\4\0",
                   4: b"OBSD\0\0\0\0\x0bOMNIBUSF4SD"}
        self.assertEqual(INSPECT.identity(replies, native, Path("/dev/ttyACM0"))["target"], "OMNIBUSF4SD")

    def test_malformed_or_unsupported_identity_is_rejected(self):
        device = self.device("3-1", ("10c4", "ea60"))
        replies = {1: b"\0\1\x1f", 2: b"BTFL", 3: b"\3\1\5", 4: b"SRF3\0\0"}
        for command, bad, message in ((1, b"\0\2\0", "Unsupported MSP"), (2, b"bad!", "Invalid MSP"),
                                       (3, b"\3\1", "Incomplete MSP"), (4, b"SRF3\0\0\0\0\x0bX", "Truncated"),
                                       (4, b"SRF3\0\0\0\0\x01!", "Invalid MSP target")):
            with self.subTest(command=command, bad=bad), self.assertRaisesRegex(RuntimeError, message):
                INSPECT.identity({**replies, command: bad}, device, Path("/dev/ttyUSB0"))


if __name__ == "__main__":
    unittest.main()
