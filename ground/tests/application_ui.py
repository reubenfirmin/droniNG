#!/usr/bin/env python3
"""Navigate the actual GCS on a private Xvfb display and accessibility bus."""

import argparse
import ctypes as c
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
from application_layout import Display, digest


def wait_for(description, callback, process, seconds=15):
    from gi.repository import GLib
    context = GLib.MainContext.default()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        while context.pending():
            context.iteration(False)
        if process.poll() is not None:
            raise AssertionError(f"Application exited while waiting for {description}")
        value = callback()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for {description}")


def run(arguments):
    application = arguments.application.resolve(strict=True)
    source = arguments.profile.resolve(strict=True)
    original_hash = digest(source)
    output = Path(tempfile.mkdtemp(prefix="ui-check-", dir=arguments.output)).resolve()
    profile = output / "profile.xml"
    shutil.copyfile(source, output / "input-profile.xml")
    tree = ET.parse(source)
    for path, value in (("General/AutoConnect", "false"), ("General/AutoSelect", "false"),
                        ("General/SaveSettingsOnExit", "true"), ("MainWindow/Maximized", "false")):
        element = tree.find(path)
        assert element is not None, path
        element.text = value
    tree.write(profile, encoding="utf-8", xml_declaration=True)
    environment = dict(os.environ)
    for name in ("LD_PRELOAD", "LD_AUDIT", "QT_QUICK_BACKEND", "GALLIUM_DRIVER",
                 "MESA_LOADER_DRIVER_OVERRIDE", "QT_QPA_PLATFORM_PLUGIN_PATH"):
        environment.pop(name, None)
    libraries = application / "lib/droning"
    environment.update(LD_LIBRARY_PATH=str(libraries) + ":" + str(libraries / "plugins/droniNG"),
                       QT_PLUGIN_PATH=str(libraries / "qtplugins"),
                       QML2_IMPORT_PATH=str(libraries / "qml"), QT_QPA_PLATFORM="xcb",
                       QT_QPA_PLATFORMTHEME="", QT_STYLE_OVERRIDE="Fusion", QT_SCALE_FACTOR="1",
                       LIBGL_ALWAYS_SOFTWARE="1", QSG_RHI_BACKEND="opengl",
                       QML_DISABLE_DISK_CACHE="true", QT_FORCE_STDERR_LOGGING="1",
                       QT_LINUX_ACCESSIBILITY_ALWAYS_ON="1", GSETTINGS_BACKEND="memory")
    for variable, directory in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                                ("XDG_CACHE_HOME", "cache"), ("XDG_RUNTIME_DIR", "runtime"),
                                ("TMPDIR", "tmp")):
        path = output / directory
        path.mkdir(mode=0o700)
        environment[variable] = str(path)
        os.environ[variable] = str(path)
    import pyatspi
    from PIL import Image
    from Xlib import X, display as xdisplay, error as xerror

    desktop = pyatspi.Registry.getDesktop(0)
    display = Display()
    display.test.XTestFakeMotionEvent.argtypes = [c.c_void_p, c.c_int, c.c_int, c.c_int, c.c_ulong]
    display.test.XTestFakeMotionEvent.restype = c.c_int
    display.test.XTestFakeButtonEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]
    display.test.XTestFakeButtonEvent.restype = c.c_int
    capture = xdisplay.Display()
    disabled = ("RawHID", "Serial", "IPconnection", "Uploader", "UsageStats", "SetupWizard")
    command = [str(application / "bin/droning-gcs"), "--multiple-ok"]
    for name in disabled:
        command.extend(["--no-load", name])
    command.append(str(profile))
    report = {"application": str(application), "application_binary_sha256": digest(application / "bin/droning-gcs"),
              "fixture_sha256": digest(Path(__file__)), "source_profile_sha256": original_hash,
              "disabled_plugins": disabled, "display": "private Xvfb; software OpenGL",
              "data": "disconnected; no telemetry", "checks": []}
    with (output / "application.log").open("wb") as stream:
        process = subprocess.Popen(command, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        try:
            window = display.main_window(process)
            wait_for("mapped main window", lambda: capture.create_resource_object("window", window)
                     .get_attributes().map_state == X.IsViewable, process)
            display.resize(window)
            accessible = wait_for("application accessibility tree",
                                  lambda: next((app for app in desktop
                                                if app.get_process_id() == process.pid), None), process)

            def visible_nodes(node=accessible, depth=0):
                showing = node.getState().contains(pyatspi.STATE_SHOWING)
                if depth and not showing:
                    return
                yield node
                for child in node:
                    if child:
                        yield from visible_nodes(child, depth + 1)

            def record(label, target=window):
                from gi.repository import GLib
                context = GLib.MainContext.default()
                while context.pending():
                    context.iteration(False)
                nodes = [{"role": node.getRoleName(), "name": node.name,
                          "selected": node.getState().contains(pyatspi.STATE_SELECTED),
                          "enabled": node.getState().contains(pyatspi.STATE_ENABLED)}
                         for node in visible_nodes()]
                (output / f"{label}.json").write_text(json.dumps(nodes, indent=2) + "\n")
                x, y, width, height = display.geometry(target)
                raw = capture.screen().root.get_image(x, y, width, height, X.ZPixmap, 0xffffffff)
                image = Image.frombytes("RGB", (width, height), raw.data, "raw", "BGRX")
                assert len(image.getcolors(width * height) or []) > 10, "Blank application view"
                path = output / f"{label}.png"
                image.save(path)
                report["checks"].append({"label": label, "image_sha256": digest(path),
                                         "visible_accessible_nodes": len(nodes)})
                print(f"PASS: {label}; {len(nodes)} visible accessibility nodes", flush=True)
                return nodes

            def send(*names):
                codes = [display.x.XKeysymToKeycode(display.display,
                         display.x.XStringToKeysym(name.encode())) for name in names]
                assert all(codes), names
                for pressed, sequence in ((True, codes), (False, reversed(codes))):
                    for code in sequence:
                        assert display.test.XTestFakeKeyEvent(display.display, code, pressed, 0)
                display.x.XSync(display.display, False)

            def click(node, target=window):
                x, y, width, height = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
                point = (x + width // 2, y + height // 2)
                left, top, frame_width, frame_height = display.geometry(target)
                assert left <= point[0] < left + frame_width and top <= point[1] < top + frame_height
                assert display.test.XTestFakeMotionEvent(display.display, -1, *point, 0)
                for pressed in (True, False):
                    assert display.test.XTestFakeButtonEvent(display.display, 1, pressed, 0)
                display.x.XSync(display.display, False)

            # Keyboard navigation exercises the actual application mode shortcuts.
            record("initial")
            for shortcut, label, title in ((1, "welcome", "Welcome"), (2, "flight", "Flight data"),
                                           (3, "configuration", "Configuration"),
                                           (4, "system", "System"), (5, "scopes", "Scopes"),
                                           (6, "advanced", "Advanced")):
                display.keys(window, "Control_L", str(shortcut))
                wait_for(title + " selection", lambda: any(node.name == title
                         and (node.getRoleName() == "page tab list"
                              or (node.getRoleName() == "page tab"
                                  and node.getState().contains(pyatspi.STATE_SELECTED)))
                         for node in visible_nodes()), process)
                time.sleep(0.5)
                record(label)

            if arguments.check_replay_seek:
                display.keys(window, "Control_L", "4")
                nodes = list(visible_nodes())
                label_index = next(index for index, node in enumerate(nodes)
                                   if node.name == "Jump to:")
                position = nodes[label_index + 1]
                assert position.getRoleName() == "spin button", "Replay position control missing"
                click(position)
                send("Control_L", "a")
                send("1")
                send("0")
                send("Return")
                wait_for("idle replay seek to ten seconds",
                         lambda: position.queryValue().currentValue == 10, process)
                record("replay-seek-idle")
                click(position)
                send("Control_L", "a")
                send("0")
                send("Return")
                wait_for("idle replay seek back to zero",
                         lambda: position.queryValue().currentValue == 0, process)
                report["idle_replay_seek_checked"] = True
                print("PASS: actual replay position control works with no log loaded", flush=True)

            # These clicks select configuration pages, with every controller transport disabled.
            display.keys(window, "Control_L", "3")
            for title in ("Vehicle", "Input", "Output", "Attitude", "Stabilization", "Modules", "Camera Stab"):
                tab = wait_for(title + " tab", lambda: next((node for node in visible_nodes()
                               if node.name == title and node.getRoleName() == "list item"), None), process)
                click(tab)
                wait_for(title + " tab selection", lambda: tab.getState().contains(pyatspi.STATE_SELECTED), process)
                time.sleep(0.3)
                record("config-" + title.lower().replace(" ", "-"))

            display.keys(window, "Control_L", "Shift_L", "s")
            general_before = ET.tostring(ET.parse(profile).getroot().find("General"))
            for menu, mnemonic, title in (("t", "o", "Options"),
                                           ("h", "p", "Installed Plugins"),
                                           ("h", "g", "About droniNG GCS"),
                                           ("h", "a", "About GCS Authors")):
                display.keys(window, "Alt_L", menu)
                time.sleep(0.2)
                # A menu owns the keyboard grab; use events without refocusing the main window.
                send(mnemonic)

                def dialog_window():
                    for child in capture.screen().root.query_tree().children:
                        try:
                            if child.get_wm_name() == title:
                                return child.id
                        except xerror.BadWindow:
                            # Menu windows can disappear between XQueryTree and GetProperty.
                            continue
                    return None

                dialog = wait_for(title, dialog_window, process)
                wait_for(title + " accessibility", lambda: any(node.name == title
                         for node in visible_nodes()), process)
                record(title.lower().replace(" ", "-"), dialog)
                if title == "About droniNG GCS" and arguments.expected_uavo_hash:
                    expected = "UAVO hash " + arguments.expected_uavo_hash[:8]
                    wait_for("correct telemetry definition hash in About", lambda: any(
                        expected in node.name for node in visible_nodes()), process)
                    report["uavo_hash_display_checked"] = arguments.expected_uavo_hash
                    print("PASS: About displays " + expected, flush=True)
                if title == "About droniNG GCS" and arguments.expected_revision:
                    expected = "From revision " + arguments.expected_revision
                    wait_for("preserved source revision in About", lambda: any(
                        expected in node.name for node in visible_nodes()), process)
                    report["revision_display_checked"] = arguments.expected_revision
                    print("PASS: About displays " + expected, flush=True)
                if title == "Options":
                    def editable_text():
                        return next((node for node in visible_nodes()
                                     if node.getRoleName() == "text" and node.name == "Proxy Settings"
                                     and node.getState().contains(pyatspi.STATE_ENABLED)), None)
                    field = wait_for("enabled proxy text field", editable_text, process)
                    click(field, dialog)
                    wait_for("focused proxy text field", lambda: field.getState()
                             .contains(pyatspi.STATE_FOCUSED), process)
                    send("Control_L", "a")
                    for character in "qt6":
                        send(character)
                    wait_for("typed options text", lambda: field.queryText().getText(0, -1) == "qt6", process)
                    record("options-typed", dialog)
                display.keys(dialog, "Escape")
                wait_for(title + " dismissal", lambda: not any(node.name == title
                         for node in visible_nodes()), process)

            display.keys(window, "Control_L", "Shift_L", "s")
            assert ET.tostring(ET.parse(profile).getroot().find("General")) == general_before, \
                   "Canceled options changed saved general settings"
            print("PASS: typed options text and Cancel leave saved general settings unchanged", flush=True)

            display.keys(window, "Control_L", "q")
            assert process.wait(timeout=15) == 0, "Application did not close normally"
            assert digest(source) == original_hash, "Original profile changed"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            display.close()
            capture.close()
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: application navigation and dialogs; original profile unchanged\nEvidence: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check-replay-seek", action="store_true",
                        help="Exercise the System workspace's position control without a log")
    parser.add_argument("--expected-uavo-hash",
                        help="Check the telemetry definition hash displayed by About")
    parser.add_argument("--expected-revision",
                        help="Check the commit, dirty marker and date displayed by About")
    parser.add_argument("--inside-private-session", action="store_true", help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)
    if arguments.inside_private_session:
        run(arguments)
    else:
        # Isolate accessibility as well as input; never inspect or drive the user's desktop.
        session = Path(tempfile.mkdtemp(prefix="droning-ui-session-")).resolve()
        environment = dict(os.environ)
        for name in ("AT_SPI_BUS_ADDRESS", "NO_AT_BRIDGE", "WAYLAND_DISPLAY", "SWAYSOCK",
                     "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "XDG_SESSION_DESKTOP", "DESKTOP_SESSION"):
            environment.pop(name, None)
        environment["GSETTINGS_BACKEND"] = "memory"
        environment["QT_NO_XDG_DESKTOP_PORTAL"] = "1"
        # Fedora's broker launcher needs a systemd user session; this private bus uses dbus-daemon.
        environment["ATSPI_DBUS_IMPLEMENTATION"] = "dbus-daemon"
        for variable, directory in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                                    ("XDG_CACHE_HOME", "cache"), ("XDG_RUNTIME_DIR", "runtime")):
            path = session / directory
            path.mkdir(mode=0o700)
            environment[variable] = str(path)
        try:
            result = subprocess.run(["dbus-run-session", "--", "xvfb-run", "-d",
                                     "--server-args=-screen 0 1440x1000x24", sys.executable,
                                     str(Path(__file__).resolve()), *sys.argv[1:],
                                     "--inside-private-session"], env=environment, timeout=150)
        finally:
            shutil.rmtree(session)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
