#!/usr/bin/env python3
"""Restore utility regression checks; all USB/flash operations use a fake helper."""

import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import pty
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("restore_board", ROOT / "make/scripts/restore-board.py")
restore = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = restore
SPEC.loader.exec_module(restore)
PROFILE = json.loads(restore.CATALOG.read_text())["boards"]["hglrc-f4-v6pro"]
LAYOUT = "@Internal Flash /0x08000000/04*016Kg,01*064Kg,07*128Kg"
DEVICE = restore.DfuDevice("3-1.4.3.4", "12345678", LAYOUT, 1, 0, 0)

FAKE_HELPER = r'''#!/usr/bin/env python3
import json, os
from pathlib import Path
import sys
args = sys.argv[1:]
home = Path(os.environ["RESTORE_TEST_DIRECTORY"])
case = os.environ.get("RESTORE_TEST_CASE", "success")
with (home / "calls.jsonl").open("a") as log:
    log.write(json.dumps(args) + "\n")
if case == "access":
    print("Cannot open DFU device 0483:df11 (LIBUSB_ERROR_ACCESS)")
    sys.exit(74)
if "-V" in args:
    print("dfu-util 0.11")
elif "-l" in args:
    serial = "changed" if case == "changed" and (home / "before.bin").exists() else "12345678"
    layout = "@Internal Flash /0x08000000/04*016Kg,01*064Kg,07*128Kg"
    if case == "geometry":
        layout = "@Internal Flash /0x08000000/04*016Kg,01*064Kg,03*128Kg"
    if case != "missing":
        print(f'Found DFU: [0483:df11] ver=2200, devnum=9, cfg=1, intf=0, path="3-1.4.3.4", alt=0, name="{layout}", serial="{serial}"')
    if case == "ambiguous":
        print(f'Found DFU: [0483:df11] ver=2200, devnum=10, cfg=1, intf=0, path="3-1.4.3.5", alt=0, name="{layout}", serial="other"')
elif "-D" in args:
    if case == "write-error":
        print("USB disconnected during download")
        sys.exit(74)
    (home / "fake-flash.bin").write_bytes(Path(args[args.index("-D") + 1]).read_bytes())
elif "-U" in args:
    address = args[args.index("-s") + 1]
    path = Path(args[args.index("-U") + 1])
    if ":leave" in address:
        (home / "booted").touch()
        path.write_bytes(b"\x00")
    elif path.name == "before.bin":
        path.write_bytes(b"\xab" * (123 if case == "short-backup" else 1048576))
    else:
        data = bytearray((home / "fake-flash.bin").read_bytes())
        if case == "readback-error":
            data[-1] ^= 1
        path.write_bytes(data)
'''


def hex_record(address, kind, data):
    record = bytes([len(data)]) + address.to_bytes(2, "big") + bytes([kind]) + data
    return ":" + (record + bytes([-sum(record) % 256])).hex().upper() + "\n"


def sample_hex(reset=0x08000009, stack=0x10010000):
    return (hex_record(0, 4, b"\x08\x00")
            + hex_record(0, 0, struct.pack("<II", stack, reset) + b"\x00\xbf\x00\xbf")
            + hex_record(0, 1, b"")).encode("ascii")


def msp_response(command, payload):
    body = bytes([len(payload), command]) + payload
    check = 0
    for value in body:
        check ^= value
    return b"$M>" + body + bytes([check])


class ImageTests(unittest.TestCase):
    def test_clean_full_flash_and_ccm_stack(self):
        image = restore.decode_hex(sample_hex(), PROFILE)
        self.assertEqual(len(image), 1048576)
        self.assertEqual(image[12:], b"\xff" * (1048576 - 12))

    def test_invalid_checksum_overlap_vectors_and_bounds(self):
        invalid = [sample_hex()[:-3] + b"01\n", sample_hex(reset=0x08000020),
                   sample_hex(reset=0x08000008), sample_hex(stack=0x40000000),
                   sample_hex().replace(b":00000001FF\n", hex_record(0, 0, b"123").encode()
                                        + b":00000001FF\n"),
                   (hex_record(0, 4, b"\x08\x10") + hex_record(0, 0, b"12345678")
                    + hex_record(0, 1, b"")).encode()]
        for data in invalid:
            with self.subTest(data=data), self.assertRaises(restore.RestoreError):
                restore.decode_hex(data, PROFILE)

    def test_changed_release_bytes_rejected_without_download(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / PROFILE["image"]
            path.write_bytes(sample_hex())
            with patch.object(restore.urllib.request, "urlopen") as download:
                with self.assertRaisesRegex(restore.RestoreError, "checksum"):
                    restore.prepare_image(PROFILE, Path(directory))
                download.assert_not_called()

    def test_changed_decoded_pin_rejected(self):
        data, profile = sample_hex(), copy.deepcopy(PROFILE)
        profile.update(bytes=len(data), sha256=restore.digest(data))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / profile["image"]
            path.write_bytes(data)
            with self.assertRaisesRegex(restore.RestoreError, "full-flash"):
                restore.prepare_image(profile, Path(directory))


class FlashTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.helper = self.home / "dfu-util"
        self.helper.write_text(FAKE_HELPER)
        self.helper.chmod(0o755)
        self.log = restore.Log(self.home, PROFILE)
        self.image = restore.decode_hex(sample_hex(), PROFILE)
        self.environment = patch.dict(os.environ, {"RESTORE_TEST_DIRECTORY": str(self.home),
                                                  "RESTORE_TEST_CASE": "success"})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.closed = patch.object(restore, "ensure_gcs_closed")
        self.closed.start()
        self.addCleanup(self.closed.stop)
        self.output = contextlib.redirect_stdout(io.StringIO())
        self.output.__enter__()
        self.addCleanup(self.output.__exit__, None, None, None)

    def runner(self, case="success"):
        os.environ["RESTORE_TEST_CASE"] = case
        return restore.DfuRunner(self.helper, self.log)

    def calls(self):
        return [json.loads(line) for line in (self.home / "calls.jsonl").read_text().splitlines()]

    def test_backup_write_full_readback_and_only_then_boot(self):
        run = self.runner()
        device = restore.wait_for_dfu(run, DEVICE.path, PROFILE, 1)
        restore.restore_flash(run, device, PROFILE, self.image, self.log)
        calls = self.calls()
        transfers = [call for call in calls if "-s" in call]
        self.assertEqual([call[call.index("-s") + 1] for call in transfers],
                         ["0x08000000:1048576", "0x08000000", "0x08000000:1048576", "0x08000000:1:leave"])
        self.assertEqual(["-D" in call for call in transfers], [False, True, False, False])
        for call in transfers:
            self.assertEqual(call[:len(DEVICE.selector())], DEVICE.selector())
            self.assertFalse(any(word in " ".join(call) for word in ("force", "unprotect", "mass-erase")))
        self.assertEqual((self.home / "before.bin").read_bytes(), b"\xab" * 1048576)
        self.assertEqual((self.home / "readback.bin").read_bytes(), self.image)
        self.assertTrue((self.home / "booted").is_file())
        report = json.loads((self.home / "result.json").read_text())
        self.assertEqual(report["backupSha256"], restore.digest(b"\xab" * 1048576))
        self.assertEqual(report["readbackSha256"], restore.digest(self.image))

    def test_bad_backup_or_changed_device_never_erases(self):
        for case in ("short-backup", "changed"):
            with self.subTest(case=case):
                run = self.runner(case)
                with self.assertRaises(restore.RestoreError):
                    restore.restore_flash(run, DEVICE, PROFILE, self.image, self.log)
                self.assertFalse(any("-D" in call for call in self.calls()))

    def test_write_or_readback_error_never_boots(self):
        for case in ("write-error", "readback-error"):
            with self.subTest(case=case):
                run = self.runner(case)
                with self.assertRaises(restore.RestoreError):
                    restore.restore_flash(run, DEVICE, PROFILE, self.image, self.log)
                self.assertFalse((self.home / "booted").exists())
                self.assertFalse(any(":leave" in " ".join(call) for call in self.calls()))

    def test_incompatible_or_ambiguous_devices_never_erase(self):
        for case in ("geometry", "ambiguous"):
            with self.subTest(case=case), self.assertRaises(restore.RestoreError):
                restore.wait_for_dfu(self.runner(case), None, PROFILE, 1)
        self.assertFalse(any("-D" in call for call in self.calls()))

    def test_usb_path_refuses_other_device(self):
        self.assertIsNone(restore.selected_device(self.runner(), "3-9"))
        self.assertEqual(restore.selected_device(self.runner("ambiguous"), DEVICE.path), DEVICE)

    def test_missing_device_times_out_with_manual(self):
        with patch.object(restore.time, "monotonic", side_effect=[0, 2]):
            with self.assertRaisesRegex(restore.RestoreError, "Manual: https://"):
                restore.wait_for_dfu(self.runner("missing"), DEVICE.path, PROFILE, 1)
        self.assertFalse(any("-D" in call for call in self.calls()))

    def test_permission_failure_is_actionable(self):
        with self.assertRaisesRegex(restore.RestoreError, "Linux denied access"):
            restore.selected_device(self.runner("access"), DEVICE.path)
        self.assertFalse(any("-D" in call for call in self.calls()))

    def test_readonly_default_never_invokes_helper(self):
        data, profile = sample_hex(), copy.deepcopy(PROFILE)
        profile.update(bytes=len(data), sha256=restore.digest(data), flashSha256=restore.digest(self.image))
        catalog = self.home / "catalog.json"
        catalog.write_text(json.dumps({"boards": {"hglrc-f4-v6pro": profile}}))
        (self.home / profile["image"]).write_bytes(data)
        with patch.object(restore, "CATALOG", catalog), patch.object(restore, "usb_devices", return_value={}), \
                patch.object(sys, "argv", ["restore-board.py", "--output", str(self.home)]), \
                patch.object(restore, "DfuRunner") as runner:
            restore.main()
            runner.assert_not_called()
        self.assertFalse((self.home / "before.bin").exists())

    def test_complete_command_and_postboot_failure_record_outcomes(self):
        data, profile = sample_hex(), copy.deepcopy(PROFILE)
        profile.update(bytes=len(data), sha256=restore.digest(data), flashSha256=restore.digest(self.image))
        catalog = self.home / "catalog.json"
        catalog.write_text(json.dumps({"boards": {"hglrc-f4-v6pro": profile}}))
        image = self.home / profile["image"]
        image.write_bytes(data)
        identity = {"variant": "BTFL", "version": "3.2.3", "board": "OBF4", "api": "1.36"}
        for outcome in ("complete", "failed"):
            output = self.home / outcome
            argv = ["restore-board.py", "--flash", "--usb-path", DEVICE.path, "--dfu-util", str(self.helper),
                    "--image", str(image), "--output", str(output)]
            with patch.object(restore, "CATALOG", catalog), patch.object(sys, "argv", argv), \
                    patch.object(restore, "verify_boot", return_value=identity) as boot:
                if outcome == "failed":
                    boot.side_effect = restore.RestoreError("Wrong running identity")
                    with self.assertRaisesRegex(restore.RestoreError, "Wrong running identity"):
                        restore.main()
                else:
                    restore.main()
                boot.assert_called_once_with(DEVICE.path, profile)
            report = json.loads(next(output.glob("*/result.json")).read_text())
            self.assertEqual(report["status"], outcome)
            self.assertEqual(report["readbackSha256"], restore.digest(self.image))
            if outcome == "complete":
                self.assertEqual(report["identity"], identity)
            else:
                self.assertEqual(report["error"], "Wrong running identity")


class BootTests(unittest.TestCase):
    def test_actual_serial_protocol_uses_only_readonly_identity_commands(self):
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        received = []
        payloads = {1: bytes(PROFILE["mspApi"]), 2: b"BTFL", 3: b"\x03\x02\x03",
                    4: b"OBF4\x00\x00\x02"}

        def controller():
            data = bytearray()
            while len(received) < 4:
                data.extend(os.read(master, 1024))
                while len(data) >= 6:
                    frame = bytes(data[:6])
                    del data[:6]
                    received.append(frame)
                    if frame[:4] == b"$M<\x00" and frame[4] in payloads and frame[4] == frame[5]:
                        response = msp_response(frame[4], payloads[frame[4]])
                        os.write(master, b"noise" + response[:4])
                        os.write(master, response[4:])

        thread = threading.Thread(target=controller, daemon=True)
        thread.start()
        replies = restore.query_msp(os.ttyname(slave))
        thread.join(timeout=3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(received, [b"$M<\x00" + bytes([n, n]) for n in (1, 2, 3, 4)])
        self.assertEqual(restore.check_identity(replies, PROFILE)["version"], "3.2.3")

    def test_wrong_variant_version_api_or_target_fails(self):
        expected = {1: bytes(PROFILE["mspApi"]), 2: b"BTFL", 3: b"\x03\x02\x03", 4: b"OBF4\x00\x00"}
        for command, wrong in ((1, b"\x00\x01\x25"), (2, b"DRON"),
                               (3, b"\x04\x00\x00"), (4, b"REVO\x00\x00"), (4, b"OBF4")):
            replies = dict(expected)
            replies[command] = wrong
            with self.subTest(command=command), self.assertRaises(restore.RestoreError):
                restore.check_identity(replies, PROFILE)

    def test_partial_bad_checksum_and_noisy_frames(self):
        good = msp_response(2, b"BTFL")
        bad = bytearray(good)
        bad[-1] ^= 1
        buffer = bytearray(b"noise" + bad + good[:4])
        self.assertEqual(restore.msp_frames(buffer), {})
        buffer.extend(good[4:])
        self.assertEqual(restore.msp_frames(buffer), {2: b"BTFL"})

    def test_multiple_controllers_require_explicit_path(self):
        with patch.object(restore, "usb_devices", return_value={"3-1": "20a0:4250", "3-2": "0483:df11"}):
            with self.assertRaisesRegex(restore.RestoreError, "--usb-path"):
                restore.controller_path(PROFILE)
            self.assertEqual(restore.controller_path(PROFILE, "3-1"), "3-1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
