#!/usr/bin/env python3
"""Check catalog compatibility against pinned board definitions and current PiOS."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPAT = ROOT / "shared/firmware/compatibility"
REVO = ROOT / "flight/targets/revolution/board-info"


def preprocess(path):
    output = subprocess.check_output(
        ["cc", "-E", "-dM", "-x", "c", str(path)], text=True)
    return dict(re.findall(r'^#define (\w+)(?:[ \t]+([^\n]*))?$', output, re.M))


def initializer(source, name):
    return re.search(r'\b' + name + r'\b[^=]*=\s*\{(.*?)^\};',
                     source, re.M | re.S).group(1)


def pins(source):
    return ["P" + port + pin for port, pin in re.findall(
        r'\.gpio\s*=\s*GPIO([A-K]),.*?\.GPIO_Pin\s*=\s*GPIO_Pin_(\d+)',
        source, re.S)]


class FirmwareCompatibility(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.record = json.loads((COMPAT / "hglrc-f4-v6pro.json").read_text())
        cls.board = next(board for board in json.loads(
            (ROOT / "shared/firmware/board-catalog.json").read_text())["boards"]
                         if board["id"] == cls.record["board"])
        cls.hw = (REVO / "board_hw_defs.c").read_text()
        cls.omnibus = preprocess(COMPAT / "betaflight-3.2.3/omnibus-target.h")
        cls.original = preprocess(COMPAT / "betaflight-3.2.3/revo-target.h")

    def test_pinned_sources(self):
        self.assertEqual(self.record["upstreamCommit"],
                         "cb962eda1a1aa7575cf36001a8ad080c96a5d9fd")
        for source in self.record["sources"]:
            self.assertEqual(hashlib.sha256((COMPAT / source["file"]).read_bytes()).hexdigest(),
                             source["sha256"], source["path"])

    def test_mcu_and_firmware_identity(self):
        info = (REVO / "board-info.mk").read_text()
        self.assertIn("STM32F405RGT", info)
        self.assertEqual(self.board["target"], "revolution")
        self.assertEqual(self.board["systemBootloader"], "STM32 system bootloader (USB DFU)")
        self.assertEqual(self.board["systemBootloader"], self.record["hardware"]["systemBootloader"])
        for name, value in (("BOARD_TYPE", self.board["boardType"]),
                            ("BOARD_REVISION", self.board["boardRevision"])):
            actual = re.search(name + r"\s*:=\s*(0x[0-9a-fA-F]+)", info).group(1)
            self.assertEqual(int(actual, 16), value)

    def test_spi_and_interrupt(self):
        self.assertEqual(self.omnibus["MPU6000_SPI_INSTANCE"], "SPI1")
        self.assertEqual(self.omnibus["MPU6000_CS_PIN"], "PA4")
        self.assertEqual(self.omnibus["MPU_INT_EXTI"], "PC4")
        self.assertEqual(pins(initializer(self.hw, "pios_spi_gyro_cfg")),
                         ["PA5", "PA6", "PA7", "PA4"])
        self.assertEqual(pins(initializer(self.hw, "pios_exti_mpu_cfg")), ["PC4"])
        self.assertEqual(self.omnibus["MAX7456_SPI_INSTANCE"], "SPI3")
        self.assertEqual(self.omnibus["MAX7456_SPI_CS_PIN"], "PA15")
        self.assertEqual(self.omnibus["SPI3_NSS_PIN"], "PB3")
        self.assertEqual(pins(initializer(self.hw, "pios_spi_telem_flash_cfg")),
                         ["PC10", "PC11", "PC12", "PA15", "PB3"])
        for signal, pin in (("SCK", "PC10"), ("MISO", "PC11"), ("MOSI", "PC12")):
            self.assertEqual(self.omnibus["SPI3_" + signal + "_PIN"], pin)

    def test_uarts_inverter_adc_and_usb(self):
        for number, native in ((1, "pios_usart_main_cfg"), (3, "pios_usart_flexi_cfg"),
                               (6, "pios_rxportusart_cfg")):
            expected = self.record["hardware"]["uart" + str(number)]
            self.assertEqual(pins(initializer(self.hw, native)), expected[::-1])
            self.assertEqual([self.omnibus[f"UART{number}_TX_PIN"],
                              self.omnibus[f"UART{number}_RX_PIN"]], expected)
        self.assertEqual(pins(initializer(self.hw, "pios_sbus_cfg")), ["PC0"])
        self.assertEqual(self.omnibus["INVERTER_PIN_UART1"], "PC0")
        self.assertIn("{GPIOC, GPIO_Pin_1,", initializer(self.hw, "pios_adc_cfg"))
        self.assertIn("{GPIOC, GPIO_Pin_2,", initializer(self.hw, "pios_adc_cfg"))
        self.assertEqual(self.omnibus["CURRENT_METER_ADC_PIN"], "PC1")
        self.assertEqual(self.omnibus["VBAT_ADC_PIN"], "PC2")
        self.assertEqual(pins(initializer(self.hw, "pios_usb_main_rm2_cfg")), ["PC5"])
        self.assertEqual(self.omnibus["VBUS_SENSING_PIN"], "PC5")

    def test_motor_pads_and_valid_alternate_timers(self):
        native = initializer(self.hw, "pios_tim_servoport_all_pins")
        self.assertEqual(pins(native)[:4], self.record["hardware"]["motorPins"])
        upstream = (COMPAT / "betaflight-3.2.3/omnibus-target.c").read_text()
        outputs = re.findall(r"DEF_TIM\(\s*(TIM\d+),\s*(CH\d+),\s*(P[A-K]\d+),\s*TIM_USE_MOTOR", upstream)[:4]
        self.assertEqual([entry[2] for entry in outputs], self.record["hardware"]["motorPins"])
        spec = importlib.util.spec_from_file_location("desk_check", ROOT / "make/scripts/betaflight-desk-check.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        timers = module.f405_timers(["cc"])
        native_timers = re.findall(r"\.timer\s*=\s*(TIM\d+),\s*\.timer_chan\s*=\s*TIM_Channel_(\d+)", native)[:4]
        for pin, (timer, channel) in zip(pins(native), native_timers):
            self.assertIn({"timer": timer, "channel": "CH" + channel}, timers[pin])
        for timer, channel, pin in outputs:
            self.assertIn({"timer": timer, "channel": channel}, timers[pin])

    def test_sensor_axes_execute_current_firmware_math(self):
        self.assertEqual(self.omnibus["GYRO_MPU6000_ALIGN"], "CW180_DEG")
        self.assertEqual(self.omnibus["ACC_MPU6000_ALIGN"], "CW180_DEG")
        self.assertEqual(self.original["GYRO_MPU6000_ALIGN"], "CW270_DEG")
        self.assertIn("PIOS_MPU_TOP_180DEG", initializer(self.hw, "pios_mpu_cfg"))
        settings = {(entry["object"], entry["field"], entry.get("element")): entry["value"]
                    for entry in self.board["settings"]}
        self.assertEqual(settings[("HwRevolution", "SPIPeripheral", None)], "MAX7456 OSD")
        self.assertEqual(settings[("HwRevolution", "Magnetometer", None)], "None")
        angles = [settings[("AttitudeSettings", "BoardRotation", axis)] / 100
                  for axis in ("Roll", "Pitch", "Yaw")]
        # Use the driver's current case body and the real firmware quaternion/
        # vector routines. Independent expected body axes for CW180 are
        # (-raw X, +raw Y, -raw Z), converting Betaflight FLU to PiOS FRD.
        mpu = (ROOT / "flight/PiOS/Common/pios_mpu.c").read_text()
        transform = re.search(r"case PIOS_MPU_TOP_180DEG:(.*?)\s*break;", mpu, re.S).group(1)
        case = """
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "coordinate_conversions.h"
struct vector { float x, y, z; };
static void transform(float x, float y, float z, struct vector *accel_data, struct vector *gyro_data) {
    float accel_x = x, accel_y = y, accel_z = z;
    float gyro_x = x, gyro_y = y, gyro_z = z;
""" + transform + """
}
int main(void) {
    const float angles[3] = {""" + ",".join(map(str, angles)) + """};
    float q[4], matrix[3][3];
    RPY2Quaternion(angles, q); Quaternion2R(q, matrix);
    const float vectors[][3] = {{1,0,0},{0,1,0},{0,0,1},{3,-4,7},{-9,5,-2}};
    for (unsigned i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
        struct vector accel, gyro;
        transform(vectors[i][0], vectors[i][1], vectors[i][2], &accel, &gyro);
        float a[3] = {accel.x, accel.y, accel.z}, g[3] = {gyro.x, gyro.y, gyro.z};
        float ar[3], gr[3];
        rot_mult(matrix, a, ar, true); rot_mult(matrix, g, gr, true);
        const float expected[3] = {-vectors[i][0], vectors[i][1], -vectors[i][2]};
        for (int j = 0; j < 3; ++j)
            if (fabsf(ar[j]-expected[j]) > 0.0001f || fabsf(gr[j]-expected[j]) > 0.0001f) return 1;
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="firmware-axes-") as directory:
            source = Path(directory) / "axes.c"
            source.write_text(case)
            binary = Path(directory) / "axes"
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "flight/Libraries/math"),
                            "-I", str(ROOT / "shared/api"),
                            str(source), str(ROOT / "flight/Libraries/math/coordinate_conversions.c"),
                            "-lm", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
