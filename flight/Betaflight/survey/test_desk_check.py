"""Independent wiring fixtures and rejection checks for the bounded survey."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("audit", ROOT / "make/scripts/betaflight-desk-check.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class SurveyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        lock, _ = audit.verify_sources()
        cls.table = audit.f405_timers(["cc"])
        cls.targets = {}
        for p in lock["repositories"]["config"]["files"]:
            if p.endswith("config.h"):
                t = audit.inspect_config(["cc"], audit.SURVEY / "upstream/config" / p, cls.table)
                cls.targets[t["target"]] = t

    def test_bus_and_irq_fixtures(self):
        expected = {
            "SPEEDYBEEF405V4": ("SPI1", "PA4", "PC4"),
            "SPEEDYBEEF405V5": ("SPI1", "PA4", "PC4"),
            "GEPRCF405": ("SPI3", "PA15", "PC3"),
            "MAMBAF405_2022A": ("SPI1", "PA4", "PC4"),
            "IFLIGHT_BLITZ_F722": ("SPI1", "PA4", "PC4"),
            "KAKUTEH7V2": ("SPI4", "PE4", "PE1"),
        }
        for name, values in expected.items():
            with self.subTest(name=name):
                gyro = self.targets[name]["gyro"]
                self.assertEqual(tuple(gyro[k] for k in ("bus", "cs", "drdy")), values)

    def test_empty_flags_do_not_consume_following_macros(self):
        output = "#define USE_GYRO \n#define USE_ACC\n#define SPI_BUS SPI1\n#define PIN PA4\n"
        self.assertEqual(audit.parse_defines(output),
                         {"USE_GYRO": "", "USE_ACC": "", "SPI_BUS": "SPI1", "PIN": "PA4"})
        for t in self.targets.values():
            self.assertEqual(t["board_macros"]["USE_MAX7456"], "")
            self.assertEqual(t["board_macros"]["UART6_TX_PIN"], "PC6")
            self.assertIn("I2C1_SDA_PIN", t["board_macros"])

    def test_alias_and_one_based_timer_occurrence(self):
        # Independent expected mappings from the F405 alternate-function table.
        for pin, occurrence, timer, channel in (("PB1", 2, "TIM3", "CH4"),
                                                ("PC8", 2, "TIM8", "CH3"),
                                                ("PB15", 3, "TIM12", "CH2")):
            result = audit.timer_record(pin, occurrence, -1, self.table)
            self.assertEqual((result["timer"], result["channel"]), (timer, channel))
        v5 = self.targets["SPEEDYBEEF405V5"]
        self.assertEqual(v5["timer_mappings"][0]["pin"], "PB1")
        self.assertEqual(v5["timer_mappings"][-1]["timer"], "TIM12")
        self.assertEqual(sum("timer" in r for t in self.targets.values()
                             for r in t["timer_mappings"]), 40)

    def test_processor_gates(self):
        self.assertEqual(sum(t["processor_backend_available"] for t in self.targets.values()), 4)
        for name in ("IFLIGHT_BLITZ_F722", "KAKUTEH7V2"):
            self.assertFalse(self.targets[name]["processor_backend_available"])
            self.assertTrue(all("timer" not in r for r in self.targets[name]["timer_mappings"]))
        self.assertTrue(all(not t["installable"] for t in self.targets.values()))

    def test_specific_review_items_survive_normalization(self):
        geprc = self.targets["GEPRCF405"]
        self.assertEqual(set(geprc["imu_alternatives"]), {"MPU6000", "ICM42688P", "BMI270", "LSM6DSK320X"})
        self.assertIn({"pin": "PB1", "roles": ["LED_STRIP_PIN", "MOTOR7_PIN"]}, geprc["shared_pin_roles"])
        self.assertIsNone(self.targets["MAMBAF405_2022A"]["hse_mhz_explicit"])
        self.assertEqual(self.targets["MAMBAF405_2022A"]["board_macros"]["ADC_INSTANCE"], "ADC3")
        self.assertEqual(self.targets["SPEEDYBEEF405V5"]["gyro"]["external_clock_pin"], "PB15")
        self.assertEqual(self.targets["KAKUTEH7V2"]["gyro"]["alignment"], "CW0_DEG")
        self.assertEqual(audit.dependency_inventory()["bmi270_config_blob_bytes"], 328)

    def test_invalid_occurrence_and_alias_are_rejected(self):
        for pin, occurrence in (("PB1", 0), ("PB1", 4), ("UNKNOWN_PIN", 1)):
            with self.assertRaises(ValueError):
                audit.timer_record(pin, occurrence, 0, self.table)
        with self.assertRaises(ValueError):
            audit.resolve({"A": "B", "B": "A"}, "A")

    def test_changed_upstream_bytes_are_rejected(self):
        original = Path.read_bytes

        def altered(path):
            data = original(path)
            return data + b"tampered" if path.name == "config.h" else data

        with mock.patch.object(Path, "read_bytes", altered):
            with self.assertRaisesRegex(ValueError, "integrity failure"):
                audit.verify_sources()


if __name__ == "__main__":
    unittest.main()
