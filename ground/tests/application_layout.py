#!/usr/bin/env python3
"""Exercise the real GCS save/close/restart workflow on a private Xvfb display."""

import argparse
import ctypes as c
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


class Display:
    def __init__(self):
        self.x = c.CDLL(ctypes.util.find_library("X11"))
        self.test = c.CDLL(ctypes.util.find_library("Xtst"))
        pointer, window = c.c_void_p, c.c_ulong
        declarations = {
            "XOpenDisplay": ([c.c_char_p], pointer),
            "XCloseDisplay": ([pointer], c.c_int),
            "XDefaultRootWindow": ([pointer], window),
            "XQueryTree": ([pointer, window, c.POINTER(window), c.POINTER(window),
                            c.POINTER(c.POINTER(window)), c.POINTER(c.c_uint)], c.c_int),
            "XFetchName": ([pointer, window, c.POINTER(pointer)], c.c_int),
            "XFree": ([pointer], c.c_int),
            "XSetInputFocus": ([pointer, window, c.c_int, window], c.c_int),
            "XRaiseWindow": ([pointer, window], c.c_int),
            "XMoveResizeWindow": ([pointer, window, c.c_int, c.c_int,
                                   c.c_uint, c.c_uint], c.c_int),
            "XGetGeometry": ([pointer, window, c.POINTER(window), c.POINTER(c.c_int),
                              c.POINTER(c.c_int), c.POINTER(c.c_uint), c.POINTER(c.c_uint),
                              c.POINTER(c.c_uint), c.POINTER(c.c_uint)], c.c_int),
            "XStringToKeysym": ([c.c_char_p], window),
            "XKeysymToKeycode": ([pointer, window], c.c_uint),
            "XSync": ([pointer, c.c_int], c.c_int),
        }
        for name, (arguments, result) in declarations.items():
            function = getattr(self.x, name)
            function.argtypes, function.restype = arguments, result
        self.test.XTestFakeKeyEvent.argtypes = [pointer, c.c_uint, c.c_int, window]
        self.test.XTestFakeKeyEvent.restype = c.c_int
        self.display = self.x.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("Cannot connect to private Xvfb display")

    def main_window(self, process):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError("Application exited before showing its main window")
            root, parent, children, count = c.c_ulong(), c.c_ulong(), c.POINTER(c.c_ulong)(), c.c_uint()
            self.x.XQueryTree(self.display, self.x.XDefaultRootWindow(self.display),
                             c.byref(root), c.byref(parent), c.byref(children), c.byref(count))
            try:
                for index in range(count.value):
                    name = c.c_void_p()
                    self.x.XFetchName(self.display, children[index], c.byref(name))
                    if name.value:
                        try:
                            if c.string_at(name) == b"droniNG GCS":
                                return children[index]
                        finally:
                            self.x.XFree(name)
            finally:
                if children:
                    self.x.XFree(children)
            time.sleep(0.1)
        raise AssertionError("Main window did not appear within 20 seconds")

    def geometry(self, window):
        root, x, y = c.c_ulong(), c.c_int(), c.c_int()
        width, height, border, depth = [c.c_uint() for _ in range(4)]
        assert self.x.XGetGeometry(self.display, window, c.byref(root), c.byref(x),
                                  c.byref(y), c.byref(width), c.byref(height),
                                  c.byref(border), c.byref(depth))
        return [x.value, y.value, width.value, height.value]

    def resize(self, window):
        self.x.XMoveResizeWindow(self.display, window, 120, 80, 1100, 740)
        self.x.XSync(self.display, False)
        time.sleep(0.3)

    def keys(self, window, *names):
        self.x.XRaiseWindow(self.display, window)
        self.x.XSetInputFocus(self.display, window, 2, 0)
        codes = [self.x.XKeysymToKeycode(self.display, self.x.XStringToKeysym(name.encode()))
                 for name in names]
        assert all(codes), names
        for pressed, sequence in ((True, codes), (False, reversed(codes))):
            for code in sequence:
                assert self.test.XTestFakeKeyEvent(self.display, code, pressed, 0)
        self.x.XSync(self.display, False)
        time.sleep(0.3)

    def close(self):
        self.x.XCloseDisplay(self.display)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def layout_only(settings):
    return {key: value for key, value in settings.items()
            if key.startswith(("UAVGadgetManager/Mode1/", "MainWindow/", "Workspace/", "ModePriorities/"))}


def views(settings):
    return sum(key.startswith("UAVGadgetManager/Mode1/") and key.endswith("/classId")
               for key in settings)


def run(arguments):
    application = arguments.application.resolve(strict=True)
    normalizer = arguments.normalizer.resolve(strict=True)
    source = arguments.profile.resolve(strict=True)
    original_hash = digest(source)
    output = Path(tempfile.mkdtemp(prefix="layout-check-", dir=arguments.output)).resolve()
    profile = output / "profile.xml"
    shutil.copyfile(source, output / "input-profile.xml")
    tree = ET.parse(source)
    # Change only the test copy: turn off automatic connections and expose split actions.
    for path, value in (("General/AutoConnect", "false"), ("General/AutoSelect", "false"),
                        ("General/SaveSettingsOnExit", "true"), ("MainWindow/Maximized", "false"),
                        ("UAVGadgetManager/Mode1/showToolbars", "true")):
        element = tree.find(path)
        if element is None:
            raise AssertionError(f"Required test profile setting missing: {path}")
        element.text = value
    tree.write(profile, encoding="utf-8", xml_declaration=True)
    environment = dict(os.environ)
    for name in ("LD_PRELOAD", "LD_AUDIT", "QT_QUICK_BACKEND", "GALLIUM_DRIVER",
                 "MESA_LOADER_DRIVER_OVERRIDE", "QT_QPA_PLATFORM_PLUGIN_PATH"):
        environment.pop(name, None)
    libraries = application / "lib/droning"
    environment.update(LD_LIBRARY_PATH=f"{libraries}:{libraries}/plugins/droniNG",
                       QT_PLUGIN_PATH=str(libraries / "qtplugins"),
                       QML_IMPORT_PATH=str(libraries / "qml"),
                       QML2_IMPORT_PATH=str(libraries / "qml"),
                       QT_QPA_PLATFORM="xcb", QT_QPA_PLATFORMTHEME="", QT_STYLE_OVERRIDE="Fusion",
                       QT_SCALE_FACTOR="1", LIBGL_ALWAYS_SOFTWARE="1", QSG_RHI_BACKEND="opengl",
                       QML_DISABLE_DISK_CACHE="true", QT_FORCE_STDERR_LOGGING="1")
    for variable, directory in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                                ("XDG_CACHE_HOME", "cache"), ("XDG_RUNTIME_DIR", "runtime"),
                                ("TMPDIR", "tmp")):
        path = output / directory
        path.mkdir(mode=0o700)
        environment[variable] = str(path)
    command = [str(application / "bin/droning-gcs"), "--multiple-ok"]
    disabled = ("RawHID", "Serial", "IPconnection", "Uploader", "UsageStats", "SetupWizard")
    for name in disabled:
        command.extend(["--no-load", name])
    command.append(str(profile))
    display = Display()
    report = {"application": str(application),
              "application_binary_sha256": digest(application / "bin/droning-gcs"),
              "normalizer_binary_sha256": digest(normalizer),
              "fixture_sha256": digest(Path(__file__)), "source_profile_sha256": original_hash,
              "disabled_plugins": disabled, "display": "private Xvfb; software OpenGL", "runs": []}
    try:
        for phase in ("split", "restore", "remove", "restore-removal"):
            log = output / f"{phase}.log"
            with log.open("wb") as stream:
                process = subprocess.Popen(command, env=environment, stdout=stream,
                                           stderr=subprocess.STDOUT)
                try:
                    window = display.main_window(process)
                    time.sleep(0.8)
                    display.keys(window, "Control_L", "2")
                    if phase == "split":
                        display.resize(window)
                        display.keys(window, "Control_L", "Shift_L", "s")
                        baseline = normalize(normalizer, profile, output / "baseline.json", environment)
                        display.keys(window, "Control_L", "Shift_L", "Right")
                        display.keys(window, "Control_L", "Shift_L", "Down")
                    elif phase == "remove":
                        display.keys(window, "Control_L", "Shift_L", "c")
                    geometry = display.geometry(window)
                    display.keys(window, "Control_L", "q")
                    assert process.wait(timeout=10) == 0, f"{phase}: application exit; see {log}"
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
            snapshot = normalize(normalizer, profile, output / f"{phase}.json", environment)
            if phase == "split":
                assert views(snapshot) == views(baseline) + 2, "Split shortcuts did not add two views"
                expected, expected_geometry = layout_only(snapshot), geometry
            elif phase == "remove":
                assert views(snapshot) == views(previous) - 1, "Close-view shortcut did not remove a view"
                expected = layout_only(snapshot)
            else:
                changed = [key for key in expected.keys() | layout_only(snapshot).keys()
                           if expected.get(key) != layout_only(snapshot).get(key)]
                assert not changed, f"{phase}: layout changed across restart: {sorted(changed)}"
            assert geometry == expected_geometry, f"{phase}: window geometry changed across restart"
            report["runs"].append({"phase": phase, "mode1_views": views(snapshot), "geometry": geometry,
                                   "log_sha256": digest(log), "snapshot_sha256": digest(output / f"{phase}.json")})
            previous = snapshot
            print(f"PASS: {phase}: {views(snapshot)} workspace views; geometry {geometry}; normal exit", flush=True)
    finally:
        display.close()
    assert digest(source) == original_hash, "Original profile changed"
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: real application split/save/restart/remove/restart; original profile unchanged\nEvidence: {output}")


def normalize(binary, profile, destination, environment):
    result = subprocess.run([str(binary), "--normalize", str(profile), str(destination)],
                            env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
    assert result.returncode == 0, result.stdout.decode(errors="replace")
    return json.loads(destination.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--normalizer", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--inside-xvfb", action="store_true", help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)
    if arguments.inside_xvfb:
        run(arguments)
    else:
        # Always create a dedicated display; never send input to the user's desktop.
        result = subprocess.run(["xvfb-run", "-d", "--server-args=-screen 0 1440x1000x24",
                                 sys.executable, str(Path(__file__).resolve()), *sys.argv[1:],
                                 "--inside-xvfb"], timeout=120)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
