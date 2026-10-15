#!/usr/bin/env python3
"""Read native controller health for 60 seconds, with logs saved automatically.

Sends only UAVTalk object requests, acknowledgments and the temporary GCS
connection handshake. Does not write controller settings, reboot or flash.
Close DroniNG before running. Uses the selected build's generated object schema.
--reset-usb first resets the host USB connection to recover a stalled interface.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
from collections import Counter
from datetime import datetime, timezone
import errno
import fcntl
import json
import os
from pathlib import Path
import re
import select
import struct
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/qt612-release"
NAMES = ("GCSTelemetryStats", "FlightTelemetryStats", "SystemStats", "SystemAlarms",
         "BaroAltitude", "GPSPosition", "AttitudeActual", "FirmwareIAPObj", "BoardProfile")
FORMATS = {"float": "f", "quint32": "I", "qint32": "i", "quint16": "H", "qint16": "h",
           "quint8": "B", "qint8": "b"}


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ (0x07 if crc & 0x80 else 0)) & 0xff
    return crc


def frame(kind, object_id, payload=b""):
    data = struct.pack("<BBHI", 0x3c, kind, 8 + len(payload), object_id) + payload
    return data + bytes([crc8(data)])


def schemas():
    generated = BUILD / "uavobject-synthetics/gcs"
    result, id_names = {}, {}
    for header in generated.glob("*.h"):
        source = header.read_text()
        name = re.search(r"class UAVOBJECTS_EXPORT (\w+):", source)
        identifier = re.search(r"OBJID = (0x[0-9A-Fa-f]+)", source)
        if name and identifier:
            id_names[int(identifier[1], 16)] = name[1]
    for name in NAMES:
        source = (generated / (name.lower() + ".h")).read_text()
        declaration = re.search(r"typedef struct \{([^}]+)\}\s*__attribute__", source, re.S)[1]
        fields = re.findall(r"(float|quint32|qint32|quint16|qint16|quint8|qint8)\s+(\w+)(?:\[(\d+)\])?;", declaration)
        fmt = "<" + "".join((count or "1") + FORMATS[kind] for kind, _, count in fields)
        size = int(re.search(r"NUMBYTES = (\d+)", source)[1])
        if struct.calcsize(fmt) != size:
            raise RuntimeError(f"Generated {name} field layout does not match its wire size")
        xml_fields = ET.parse(ROOT / f"shared/uavobjectdefinition/{name.lower()}.xml").findall("object/field")
        options = {field.attrib["name"]: [option.text for option in field.findall("options/option")]
                   for field in xml_fields}
        elements = {field.attrib["name"]: [element.text for element in field.findall("elementnames/elementname")]
                    for field in xml_fields}
        result[name] = {"id": int(re.search(r"OBJID = (0x[0-9A-Fa-f]+)", source)[1], 16),
                        "fields": fields, "fmt": fmt, "size": size, "options": options, "elements": elements}
    return result, id_names


def decode(schema, payload):
    values, position, result = struct.unpack(schema["fmt"], payload), 0, {}
    for _, name, count in schema["fields"]:
        count = int(count or 1)
        value = list(values[position:position + count]); position += count
        options = schema["options"].get(name, [])
        if options:
            value = [options[item] if item < len(options) else item for item in value]
        elements = schema["elements"].get(name, [])
        result[name] = dict(zip(elements, value)) if len(elements) == count else value[0] if count == 1 else value
    return result


def find_controller():
    for process in Path("/proc").glob("[0-9]*/comm"):
        try:
            if process.read_text().strip() in ("droning-gcs", "dronin-gcs"):
                raise RuntimeError("Close DroniNG before inspecting the controller")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
    devices = []
    for node in Path("/sys/class/hidraw").glob("hidraw*"):
        for parent in node.resolve().parents:
            try:
                if (parent / "idVendor").read_text().strip() == "20a0" and (parent / "idProduct").read_text().strip() == "4250":
                    devices.append((Path("/dev") / node.name, parent.name, (parent / "product").read_text().strip()))
                    break
            except OSError:
                continue
    if len(devices) != 1:
        raise RuntimeError("Connect one native DroniNG controller before inspecting")
    return devices[0]


def inspect(seconds, reset_usb=False):
    objects, id_names = schemas()
    by_id = {schema["id"]: name for name, schema in objects.items()}
    node, topology, product = find_controller()
    if reset_usb:
        device = Path("/sys/bus/usb/devices") / topology
        bus, number = int((device / "busnum").read_text()), int((device / "devnum").read_text())
        print(f"Resetting only the USB connection at {topology}; firmware and settings stay intact", flush=True)
        usb = os.open(f"/dev/bus/usb/{bus:03d}/{number:03d}", os.O_WRONLY)
        try:
            fcntl.ioctl(usb, 0x5514)  # USBDEVFS_RESET, _IO('U', 20)
        finally:
            os.close(usb)
        time.sleep(1)
        node, current_topology, current_product = find_controller()
        if (current_topology, current_product) != (topology, product):
            raise RuntimeError("The controller changed after the USB connection reset")
    directory = BUILD / "controller-inspection" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    directory.mkdir(parents=True)
    print(f"Reading {product} at {topology} ({node}); evidence: {directory}", flush=True)
    latest, counts, errors, stream = {}, Counter(), Counter(), bytearray()
    fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)
    start, last_rx, next_handshake, next_requests, next_report = time.monotonic(), time.monotonic(), 0, 0, 0
    state = 1
    poll = select.poll(); poll.register(fd, select.POLLIN | select.POLLERR | select.POLLHUP)

    def send(data):
        for offset in range(0, len(data), 62):
            part = data[offset:offset + 62]
            report = bytes([2, len(part)]) + part + bytes(62 - len(part))
            if os.write(fd, report) != 64:
                raise RuntimeError("USB HID write did not send a complete report")

    def handshake(status):
        # The wire schema is checked above; all handshake statistics start at zero.
        send(frame(0x20, objects["GCSTelemetryStats"]["id"], struct.pack("<ffIIIB", 0, 0, 0, 0, 0, status)))

    failure = None
    try:
        with (directory / "samples.jsonl").open("w") as log:
            while time.monotonic() - start < seconds:
                now = time.monotonic()
                if now >= next_handshake:
                    handshake(state); next_handshake = now + (1 if state != 3 else 4)
                if now >= next_requests:
                    # Spread requests rather than filling the firmware's five-request queue.
                    request = NAMES[1 + counts["requests"] % (len(NAMES) - 1)]
                    send(frame(0x21, objects[request]["id"]))
                    counts["requests"] += 1; next_requests = now + 0.15
                for _, flags in poll.poll(50):
                    if flags & (select.POLLERR | select.POLLHUP | select.POLLNVAL):
                        raise OSError(errno.ENODEV, "USB HID reported a disconnected device")
                    try:
                        report = os.read(fd, 64)
                    except BlockingIOError:
                        continue
                    if len(report) < 2 or report[0] != 1 or report[1] > len(report) - 2:
                        errors["invalidHidReport"] += 1; continue
                    stream.extend(report[2:2 + report[1]])
                while len(stream) >= 8:
                    if stream[0] != 0x3c:
                        del stream[0]; errors["sync"] += 1; continue
                    length = int.from_bytes(stream[2:4], "little")
                    if length < 8 or length > 256:
                        del stream[0]; errors["length"] += 1; continue
                    if len(stream) <= length:
                        break
                    data = bytes(stream[:length]); checksum = stream[length]; del stream[:length + 1]
                    if crc8(data) != checksum:
                        errors["crc"] += 1; continue
                    kind, identifier = data[1], int.from_bytes(data[4:8], "little")
                    last_rx = time.monotonic()
                    if kind == 0x22:
                        send(frame(0x23, identifier))
                    name = by_id.get(identifier)
                    counts[id_names.get(identifier, hex(identifier))] += 1
                    if name and kind in (0x20, 0x22) and len(data) - 8 == objects[name]["size"]:
                        sample = decode(objects[name], data[8:]); latest[name] = sample
                        log.write(json.dumps({"elapsed": round(now - start, 3), "object": name, "data": sample}) + "\n")
                        if name == "FlightTelemetryStats":
                            state = 3 if sample["Status"] in ("HandshakeAck", "Connected") else 1
                if now >= next_report:
                    stats = dict(latest.get("SystemStats", {}))
                    for key in ("EventSystemWarningID", "ObjectManagerCallbackID", "ObjectManagerQueueID"):
                        if key in stats: stats[key] = id_names.get(stats[key], hex(stats[key]))
                    print(json.dumps({"elapsed": round(now - start, 1), "SystemStats": stats,
                                      "BaroAltitude": latest.get("BaroAltitude"),
                                      "SystemAlarms": latest.get("SystemAlarms"),
                                      "FlightTelemetryStats": latest.get("FlightTelemetryStats")}), flush=True)
                    next_report = now + 5
                if now - last_rx > 8:
                    raise RuntimeError("No valid UAVTalk frames received for eight seconds")
            missing = set(("FlightTelemetryStats", "SystemStats", "SystemAlarms")) - latest.keys()
            if missing:
                raise RuntimeError("Telemetry responded without the required diagnostic objects: " + ", ".join(sorted(missing)))
    except (OSError, RuntimeError, ValueError) as error:
        failure = str(error)
        raise
    finally:
        try:
            handshake(0)
        except OSError:
            pass
        os.close(fd)
        result = {"usbPath": topology, "product": product, "seconds": round(time.monotonic() - start, 1),
                  "success": failure is None, "error": failure,
                  "counts": dict(counts), "parseErrors": dict(errors), "last": latest}
        (directory / "report.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"Evidence: {directory / 'report.json'}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--reset-usb", action="store_true", help="reset the host USB connection before inspecting")
    args = parser.parse_args()
    if args.seconds < 1: parser.error("--seconds must be positive")
    try:
        inspect(args.seconds, args.reset_usb)
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f"Inspection failed: {error}\n")
