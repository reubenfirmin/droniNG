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
OMNIBUS = json.loads(restore.CATALOG.read_text())["boards"]["omnibusf4sd"]
ENTRY_SPEC = importlib.util.spec_from_file_location("restore_omnibus", ROOT / "make/scripts/restore-omnibusf4sd.py")
entrypoint = importlib.util.module_from_spec(ENTRY_SPEC)
ENTRY_SPEC.loader.exec_module(entrypoint)
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


def omnibus_replies(target=b"OMNIBUSF4SD"):
    return {1: bytes(OMNIBUS["mspApi"]), 2: b"BTFL", 3: b"\x03\x04\x00",
            4: b"OBSD\x00\x00\x02\x03" + bytes([len(target)]) + target}


def sample_invalid():
    """Small saved ELF with an application at the native imported flash offset."""
    data = bytearray(52 + 64 + 10)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HH", data, 16, 2, 40)
    struct.pack_into("<I", data, 28, 52)
    struct.pack_into("<HH", data, 42, 32, 2)
    struct.pack_into("<8I", data, 52, 1, 116, 0x08000000, 0x08000000, 8, 8, 6, 4)
    struct.pack_into("<8I", data, 84, 1, 124, 0x08020000, 0x08020000, 2, 2, 5, 2)
    struct.pack_into("<II", data, 116, 0x20000400, 0x08020001)
    data[124:] = b"\xfe\xe7"
    image = bytearray(OMNIBUS["flashBytes"])
    image[:8], image[131072:131074] = data[116:124], data[124:]
    descriptor = bytearray.fromhex(OMNIBUS["invalidState"]["descriptorHex"])
    descriptor[40:60] = restore.hashlib.sha1(image[:131074]).digest()
    image[-100:] = descriptor
    profile = restore.select_profile({"omnibusf4sd": OMNIBUS}, "omnibusf4sd", True)
    profile.update(sourceBytes=len(data), sourceSha256=restore.digest(data),
                   descriptorHex=descriptor.hex(), sha256=restore.digest(image),
                   flashSha256=restore.digest(image))
    return bytes(data), bytes(image), profile


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

    def test_verified_offline_release_is_cached_for_future_restores(self):
        data, profile = sample_hex(), copy.deepcopy(OMNIBUS)
        image = restore.decode_hex(data, profile)
        profile.update(bytes=len(data), sha256=restore.digest(data), flashSha256=restore.digest(image))
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            supplied = home / "official.hex"
            supplied.write_bytes(data)
            with patch.object(restore.urllib.request, "urlopen") as download:
                self.assertEqual(restore.prepare_image(profile, home, supplied), image)
                supplied.unlink()
                self.assertEqual(restore.prepare_image(profile, home), image)
                download.assert_not_called()


class InvalidImageTests(unittest.TestCase):
    def test_reconstructs_saved_image_and_reuses_checked_cache_without_source(self):
        data, image, profile = sample_invalid()
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            source = home / "failure.elf"
            source.write_bytes(data)
            with patch.object(restore.urllib.request, "urlopen") as download:
                self.assertEqual(restore.prepare_image(profile, home, source), image)
                source.unlink()
                self.assertEqual(restore.prepare_image(profile, home), image)
                download.assert_not_called()
            cache = home / profile["image"]
            damaged = bytearray(cache.read_bytes())
            damaged[-1] ^= 1
            cache.write_bytes(damaged)
            with self.assertRaisesRegex(restore.RestoreError, "checksum pin"):
                restore.prepare_image(profile, home)

    def test_missing_evidence_and_changed_source_are_rejected_without_download(self):
        data, image, profile = sample_invalid()
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            source = home / "failure.elf"
            with patch.object(restore.urllib.request, "urlopen") as download:
                with self.assertRaisesRegex(restore.RestoreError, "missing"):
                    restore.prepare_image(profile, home, source)
                source.write_bytes(data[:-1] + b"\x00")
                with self.assertRaisesRegex(restore.RestoreError, "checksum pin"):
                    restore.prepare_image(profile, home, source)
                download.assert_not_called()
            self.assertFalse((home / profile["image"]).exists())

    def test_bad_application_descriptor_and_reserved_load_rejected(self):
        data, image, profile = sample_invalid()
        bad_descriptor = copy.deepcopy(profile)
        descriptor = bytearray.fromhex(profile["descriptorHex"])
        descriptor[40] ^= 1
        bad_descriptor["descriptorHex"] = descriptor.hex()
        with self.assertRaisesRegex(restore.RestoreError, "application checksum"):
            restore.decode_invalid_elf(data, bad_descriptor)
        damaged = bytearray(data)
        struct.pack_into("<I", damaged, 84 + 12, 0x08004000)
        changed = {**profile, "sourceSha256": restore.digest(damaged)}
        with self.assertRaisesRegex(restore.RestoreError, "reserved space"):
            restore.decode_invalid_elf(damaged, changed)

    def test_fixture_selection_preserves_default_and_refuses_unsupported_board(self):
        profiles = {"omnibusf4sd": OMNIBUS, "hglrc-f4-v6pro": PROFILE}
        self.assertIs(restore.select_profile(profiles, "omnibusf4sd"), OMNIBUS)
        invalid = restore.select_profile(profiles, "omnibusf4sd", True)
        self.assertEqual(invalid["testState"], "imu-startup-reset")
        self.assertEqual(invalid["flashSectors"], OMNIBUS["flashSectors"])
        with self.assertRaisesRegex(restore.RestoreError, "No pinned"):
            restore.select_profile(profiles, "hglrc-f4-v6pro", True)

    def test_preserved_physical_failure_reconstructs_exact_installed_image(self):
        profile = restore.select_profile({"omnibusf4sd": OMNIBUS}, "omnibusf4sd", True)
        evidence = ROOT / profile["source"]
        if not evidence.exists():
            self.skipTest("Preserved physical failure executable is not present in this checkout")
        with tempfile.TemporaryDirectory() as directory:
            image = restore.prepare_image(profile, Path(directory))
        self.assertEqual(len(image), 1048576)
        self.assertEqual(restore.digest(image),
                         "7007b1fb1cb974d7d574121d41c3983337aac460172e7e9ef267afae5ec7be63")


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

    def test_omnibus_restore_completes_only_with_fresh_matching_target(self):
        data, profile = sample_hex(), copy.deepcopy(OMNIBUS)
        profile.update(bytes=len(data), sha256=restore.digest(data), flashSha256=restore.digest(self.image))
        catalog = self.home / "omnibus-catalog.json"
        catalog.write_text(json.dumps({"boards": {"omnibusf4sd": profile}}))
        image = self.home / profile["image"]
        image.write_bytes(data)
        for target in (b"OMNIBUSF4SD", b"OMNIBUSF4"):
            output = self.home / target.decode()
            argv = ["restore-board.py", "--flash", "--board", "omnibusf4sd", "--usb-path", DEVICE.path,
                    "--dfu-util", str(self.helper), "--image", str(image), "--output", str(output)]
            with patch.object(restore, "CATALOG", catalog), patch.object(sys, "argv", argv), \
                    patch.object(restore, "verify_boot", side_effect=lambda *args: restore.check_identity(
                        omnibus_replies(target), profile)):
                if target == b"OMNIBUSF4SD":
                    restore.main()
                else:
                    with self.assertRaisesRegex(restore.RestoreError, "target name"):
                        restore.main()
            report = json.loads(next(output.glob("*/result.json")).read_text())
            self.assertEqual(report["readbackSha256"], restore.digest(self.image))
            self.assertEqual(report["status"], "complete" if target == b"OMNIBUSF4SD" else "failed")
            if report["status"] == "complete":
                self.assertEqual(report["identity"]["target"], "OMNIBUSF4SD")


    def test_invalid_restore_backs_up_verifies_and_records_expected_failure(self):
        data, image, profile = sample_invalid()
        catalog = self.home / "invalid-catalog.json"
        catalog.write_text(json.dumps({"boards": {"omnibusf4sd": {**OMNIBUS, "invalidState": profile}}}))
        source = self.home / "failure.elf"
        source.write_bytes(data)
        identity = {"target": "OMNIBUSF4SD", "state": "imu-startup-reset",
                    "usbPath": DEVICE.path, "usbEnumerations": ["10", "11"]}
        for outcome in ("complete", "failed"):
            output = self.home / ("invalid-" + outcome)
            argv = ["restore-board.py", "--flash", "--invalid", "--board", "omnibusf4sd",
                    "--usb-path", DEVICE.path, "--dfu-util", str(self.helper),
                    "--image", str(source), "--output", str(output)]
            with patch.object(restore, "CATALOG", catalog), patch.object(sys, "argv", argv), \
                    patch.object(restore, "verify_invalid_boot", return_value=identity) as boot, \
                    patch.object(restore, "query_msp") as msp:
                if outcome == "failed":
                    boot.side_effect = restore.RestoreError("Reset loop not observed")
                    with self.assertRaisesRegex(restore.RestoreError, "Reset loop not observed"):
                        restore.main()
                else:
                    restore.main()
                msp.assert_not_called()
                boot.assert_called_once_with(DEVICE.path, restore.select_profile(
                    json.loads(catalog.read_text())["boards"], "omnibusf4sd", True), 30)
            report = json.loads(next(output.glob("*/result.json")).read_text())
            self.assertEqual(report["status"], outcome)
            self.assertEqual(report["testState"], "imu-startup-reset")
            self.assertEqual(report["firmware"], "DroniNG")
            self.assertEqual(report["readbackSha256"], restore.digest(image))
            self.assertEqual(next(output.glob("*/before.bin")).read_bytes(), b"\xab" * 1048576)
            self.assertEqual(next(output.glob("*/readback.bin")).read_bytes(), image)
            if outcome == "complete":
                self.assertEqual(report["identity"], identity)
            else:
                self.assertIn("Reset loop not observed", report["error"])

    def test_invalid_prepare_never_opens_usb_or_flashes(self):
        data, image, profile = sample_invalid()
        source = self.home / "failure.elf"
        source.write_bytes(data)
        catalog = self.home / "invalid-catalog.json"
        catalog.write_text(json.dumps({"boards": {"omnibusf4sd": {**OMNIBUS, "invalidState": profile}}}))
        argv = ["restore-board.py", "--invalid", "--board", "omnibusf4sd",
                "--image", str(source), "--output", str(self.home)]
        with patch.object(restore, "CATALOG", catalog), patch.object(sys, "argv", argv), \
                patch.object(restore, "usb_devices", return_value={}), \
                patch.object(restore, "DfuRunner") as runner:
            restore.main()
            runner.assert_not_called()
        self.assertEqual((self.home / profile["image"]).read_bytes(), image)
        self.assertFalse((self.home / "before.bin").exists())


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

    def test_omnibus_rejects_missing_truncated_or_wrong_full_target(self):
        self.assertEqual(restore.check_identity(omnibus_replies(), OMNIBUS)["target"], "OMNIBUSF4SD")
        for board in (b"OBSD\x00\x00", omnibus_replies(b"OMNIBUSF4")[4],
                      omnibus_replies()[4][:-1], omnibus_replies(b"")[4]):
            replies = omnibus_replies()
            replies[4] = board
            with self.subTest(board=board), self.assertRaisesRegex(restore.RestoreError, "target name"):
                restore.check_identity(replies, OMNIBUS)

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


class InvalidBootTests(unittest.TestCase):
    def setUp(self):
        self.profile = restore.select_profile({"omnibusf4sd": OMNIBUS}, "omnibusf4sd", True)

    def test_requires_matching_native_product_and_two_fresh_enumerations(self):
        with patch.object(restore, "usb_devices", return_value={DEVICE.path: "20a0:4250"}), \
                patch.object(Path, "read_text", side_effect=["OMNIBUSF4SD", "10", "OMNIBUSF4SD", "11"]), \
                patch.object(restore.time, "sleep"), patch.object(restore, "query_msp") as msp:
            identity = restore.verify_boot(DEVICE.path, self.profile)
            msp.assert_not_called()
        self.assertEqual(identity["state"], "imu-startup-reset")
        self.assertEqual(identity["usbEnumerations"], ["10", "11"])
        self.assertEqual(identity["usbPath"], DEVICE.path)

    def test_steady_usb_wrong_firmware_or_dfu_cannot_count_as_reproduced(self):
        for devices in ({DEVICE.path: "20a0:4250"}, {DEVICE.path: "0483:5740"},
                        {DEVICE.path: "0483:df11"}, {"3-9": "20a0:4250"}, {}):
            with self.subTest(devices=devices), \
                    patch.object(restore, "usb_devices", return_value=devices), \
                    patch.object(Path, "read_text", side_effect=["OMNIBUSF4SD", "10"] * 2), \
                    patch.object(restore.time, "monotonic", side_effect=[0, 0, 1, 31]), \
                    patch.object(restore.time, "sleep"), \
                    self.assertRaisesRegex(restore.RestoreError, "--invalid --check"):
                restore.verify_boot(DEVICE.path, self.profile)

    def test_another_native_target_is_rejected(self):
        with patch.object(restore, "usb_devices", return_value={DEVICE.path: "20a0:4250"}), \
                patch.object(Path, "read_text", side_effect=["RADIX", "10"]), \
                self.assertRaisesRegex(restore.RestoreError, "product"):
            restore.verify_boot(DEVICE.path, self.profile)

    def test_reset_during_descriptor_read_is_retried(self):
        with patch.object(restore, "usb_devices", return_value={DEVICE.path: "20a0:4250"}), \
                patch.object(Path, "read_text", side_effect=[FileNotFoundError(), "OMNIBUSF4SD", "10",
                                                           "OMNIBUSF4SD", "11"]), \
                patch.object(restore.time, "sleep") as sleep:
            identity = restore.verify_boot(DEVICE.path, self.profile)
        self.assertEqual(identity["state"], "imu-startup-reset")
        self.assertEqual(sleep.call_count, 2)

    def test_invalid_check_uses_no_image_or_flash_helper(self):
        argv = ["restore-board.py", "--invalid", "--check", "--board", "omnibusf4sd",
                "--usb-path", DEVICE.path]
        identity = {"target": "OMNIBUSF4SD", "state": "imu-startup-reset"}
        with patch.object(sys, "argv", argv), patch.object(restore, "ensure_gcs_closed"), \
                patch.object(restore, "verify_invalid_boot", return_value=identity) as boot, \
                patch.object(restore, "prepare_image") as prepare, \
                patch.object(restore, "DfuRunner") as runner, contextlib.redirect_stdout(io.StringIO()):
            restore.main()
        boot.assert_called_once_with(DEVICE.path, self.profile, 30)
        prepare.assert_not_called()
        runner.assert_not_called()


class EntrypointTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.output = patch.object(entrypoint, "OUTPUT", self.home / "output")
        self.output.start()
        self.addCleanup(self.output.stop)
        self.root = patch.object(entrypoint, "ROOT", self.home)
        self.root.start()
        self.addCleanup(self.root.stop)
        self.console = contextlib.redirect_stdout(io.StringIO())
        self.console.__enter__()
        self.addCleanup(self.console.__exit__, None, None, None)

    def run_entrypoint(self, args=(), result=0):
        with patch.object(entrypoint.subprocess, "Popen") as start:
            process = start.return_value.__enter__.return_value
            process.stdout = io.StringIO("firmware recovery output\n")
            process.wait.return_value = result
            self.assertEqual(entrypoint.main(list(args)), result)
            return start.call_args.args[0]

    def test_no_arguments_restore_only_omnibus_and_capture_output(self):
        command = self.run_entrypoint()
        self.assertIn("--flash", command)
        self.assertEqual(command[command.index("--board") + 1], "omnibusf4sd")
        self.assertEqual(Path(command[2]).name, "restore-board.py")
        log = (entrypoint.OUTPUT / "run-restore.log").read_text()
        self.assertIn("firmware recovery output", log)
        self.assertIn("exited with status 0", log)

    def test_prepare_and_check_never_flash(self):
        for args in (("--prepare",), ("--check", "--usb-path", DEVICE.path)):
            with self.subTest(args=args):
                command = self.run_entrypoint(args)
                self.assertNotIn("--flash", command)
                if "--check" in args:
                    self.assertIn("--check", command)
                    self.assertEqual(command[command.index("--usb-path") + 1], DEVICE.path)

    def test_reuses_recovery_evidence_until_verified_cache_exists(self):
        evidence = self.home / "build/qt612-release/omnibusf4sd-tests" / entrypoint.IMAGE
        evidence.parent.mkdir(parents=True)
        evidence.write_bytes(b"offline release")
        command = self.run_entrypoint(("--prepare",))
        self.assertEqual(command[command.index("--image") + 1], str(evidence))
        (entrypoint.OUTPUT / entrypoint.IMAGE).write_bytes(b"cached release")
        self.assertNotIn("--image", self.run_entrypoint(("--prepare",)))

    def test_invalid_mode_supports_flash_prepare_check_and_separate_release_cache(self):
        evidence = self.home / "build/qt612-release/omnibusf4sd-tests" / entrypoint.IMAGE
        evidence.parent.mkdir(parents=True)
        evidence.write_bytes(b"offline Betaflight release")
        for args in (("--invalid",), ("--invalid", "--prepare"), ("--invalid", "--check")):
            with self.subTest(args=args):
                command = self.run_entrypoint(args)
                self.assertIn("--invalid", command)
                self.assertNotIn("--image", command)
                self.assertEqual("--flash" in command, len(args) == 1)
                self.assertEqual("--check" in command, "--check" in args)
        log = (entrypoint.OUTPUT / "run-restore.log").read_text()
        self.assertIn("--invalid --flash", log)

    def test_failure_status_and_history_are_retained(self):
        self.run_entrypoint(result=1)
        self.run_entrypoint(result=0)
        log = (entrypoint.OUTPUT / "run-restore.log").read_text()
        self.assertIn("exited with status 1", log)
        self.assertIn("exited with status 0", log)
        self.assertEqual(log.count("Recovery command:"), 2)

    def test_second_command_cannot_start_a_parallel_restore(self):
        entrypoint.OUTPUT.mkdir()
        with (entrypoint.OUTPUT / "restore.lock").open("a") as lock:
            entrypoint.fcntl.flock(lock, entrypoint.fcntl.LOCK_EX | entrypoint.fcntl.LOCK_NB)
            with patch.object(entrypoint.subprocess, "Popen") as start, \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stopped:
                entrypoint.main([])
            self.assertEqual(stopped.exception.code, 1)
            start.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
