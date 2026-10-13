#!/usr/bin/env python3
"""Validate the native inspection command's framing and generated wire schema."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("inspect_uavtalk", ROOT / "make/scripts/inspect-uavtalk.py")
INSPECT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSPECT)


class NativeInspectionTests(unittest.TestCase):
    def test_crc_uses_uavtalk_crc8(self):
        self.assertEqual(INSPECT.crc8(b"123456789"), 0xf4)
        request = INSPECT.frame(0x21, 0x11223344)
        self.assertEqual(request[:8], bytes.fromhex("3c21080044332211"))
        self.assertEqual(len(request), 9)
        self.assertEqual(request[-1], INSPECT.crc8(request[:-1]))

    def test_handshake_matches_selected_build_schema(self):
        objects, _ = INSPECT.schemas()
        schema = objects["GCSTelemetryStats"]
        payload = struct.pack("<ffIIIB", 0, 0, 0, 0, 0, 1)
        self.assertEqual(len(payload), schema["size"])
        self.assertEqual(INSPECT.decode(schema, payload)["Status"], "HandshakeReq")
        connected = payload[:-1] + bytes([3])
        self.assertEqual(INSPECT.decode(schema, connected)["Status"], "Connected")

    def test_alarm_labels_and_warning_identifiers(self):
        objects, names = INSPECT.schemas()
        schema = objects["SystemAlarms"]
        payload = bytearray(schema["size"])
        # Fields are sorted by their wire width; Alarm is the first uint8 array.
        field_type, field_name, count = schema["fields"][0]
        self.assertEqual((field_type, field_name), ("quint8", "Alarm"))
        labels = schema["elements"]["Alarm"]
        payload[labels.index("EventSystem")] = schema["options"]["Alarm"].index("Warning")
        decoded = INSPECT.decode(schema, payload)
        self.assertEqual(decoded["Alarm"]["EventSystem"], "Warning")
        self.assertEqual(names[objects["BaroAltitude"]["id"]], "BaroAltitude")
        self.assertEqual(int(count), len(labels))


if __name__ == "__main__":
    unittest.main()
