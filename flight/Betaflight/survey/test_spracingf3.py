"""Original-F3 source/variant gates and independently reviewed wiring fixtures."""
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("spracingf3", ROOT / "make/scripts/spracingf3-dry-run.py")
DRY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DRY)
AUDIT = DRY.module("audit", ROOT / "make/scripts/betaflight-desk-check.py")


class SpracingF3Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        AUDIT.verify_sources(DRY.SURVEY / "spracingf3.lock.json", DRY.SURVEY / "spracingf3-source-manifest.json")
        cls.profile = DRY.profile(["cc"], AUDIT)

    def test_original_f3_transport_is_separate_from_evo(self):
        self.assertEqual(self.profile["boardIdentifier"], "SRF3")
        self.assertEqual(self.profile["transport"]["gcs"], "CP2102 / UART1")
        self.assertFalse(self.profile["transport"]["usbDfu"])
        self.assertFalse(self.profile["installable"])
        self.assertEqual(self.profile["serial"]["UART2"], {"tx": "PA14", "rx": "PA15"})

    def test_motor_alternate_functions_and_no_usb_on_motor_pins(self):
        motors = self.profile["motorOutputs"]
        self.assertEqual(len(motors), 8)
        self.assertEqual([(m["pin"], m["timer"], m["alternateFunction"]) for m in motors[:4]],
                         [("PA6", "TIM16", "GPIO_AF_1"), ("PA7", "TIM17", "GPIO_AF_1"),
                          ("PA11", "TIM4", "GPIO_AF_10"), ("PA12", "TIM4", "GPIO_AF_10")])
        self.assertEqual(len(self.profile["timers"]), 17)

    def test_i2c_driver_reuse_and_orientation(self):
        imu = self.profile["imu"]
        self.assertEqual((imu["bus"], imu["scl"], imu["sda"], imu["drdy"]), ("I2C1", "PB6", "PB7", "PC13"))
        self.assertEqual(imu["nativeOrientation"], "PIOS_MPU_TOP_180DEG")
        self.assertEqual(self.profile["sramBytes"], 40960)
        self.assertEqual(self.profile["ccmBytes"], 8192)

    def test_evo_or_unreviewed_variant_is_rejected(self):
        original = AUDIT.parse_defines
        def altered(output):
            macros = original(output)
            if "TARGET_BOARD_IDENTIFIER" in macros:
                macros["TARGET_BOARD_IDENTIFIER"] = '"SPEV"'
            return macros
        with mock.patch.object(AUDIT, "parse_defines", altered), self.assertRaisesRegex(ValueError, "wiring changed"):
            DRY.profile(["cc"], AUDIT)

    def test_partial_timer_translation_is_rejected(self):
        original = Path.read_text
        def altered(path, *args, **kwargs):
            data = original(path, *args, **kwargs)
            return data.replace("DEF_TIM(TIM16, CH1, PA6,", "UNREVIEWED(TIM16, CH1, PA6,") if path == DRY.TARGET / "target.c" else data
        with mock.patch.object(Path, "read_text", altered), self.assertRaisesRegex(ValueError, "incomplete F3 timer"):
            DRY.profile(["cc"], AUDIT)

    def test_tampered_driver_source_is_rejected(self):
        original = Path.read_bytes
        def altered(path):
            data = original(path)
            return data + b"tampered" if path.name == "accgyro_mpu6050.c" else data
        with mock.patch.object(Path, "read_bytes", altered), self.assertRaisesRegex(ValueError, "integrity failure"):
            AUDIT.verify_sources(DRY.SURVEY / "spracingf3.lock.json", DRY.SURVEY / "spracingf3-source-manifest.json")


if __name__ == "__main__":
    unittest.main()
