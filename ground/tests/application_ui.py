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
    if arguments.check_firmware_installation:
        assert not Path("/dev/bus/usb").exists(), "Firmware UI checks require isolated USB device access"
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
    from gi.repository import GLib
    from PIL import Image
    from Xlib import X, display as xdisplay, error as xerror

    desktop = pyatspi.Registry.getDesktop(0)
    display = Display()
    display.test.XTestFakeMotionEvent.argtypes = [c.c_void_p, c.c_int, c.c_int, c.c_int, c.c_ulong]
    display.test.XTestFakeMotionEvent.restype = c.c_int
    display.test.XTestFakeButtonEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]
    display.test.XTestFakeButtonEvent.restype = c.c_int
    capture = xdisplay.Display()
    disabled = ("RawHID", "Serial", "IPconnection", "UsageStats", "SetupWizard")
    if arguments.check_firmware_installation:
        disabled = tuple(name for name in disabled if name != "RawHID")
    else:
        disabled += ("Uploader",)
    if arguments.check_controller_setup:
        disabled = tuple(name for name in disabled if name != "SetupWizard")
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
            popup_roots = []
            popup_events = []
            def observe_popup(event):
                try:
                    node = event.source
                    if "menu" not in node.getRoleName() or node.get_process_id() != process.pid:
                        return
                    popup_events.append((event.type, node.getRoleName(), node.name))
                    for _ in range(20):
                        if not node or node.getRoleName() == "application":
                            break
                        if node.getRoleName() == "popup menu" and node not in popup_roots:
                            popup_roots.append(node)
                        node = node.parent
                except GLib.GError:
                    pass # Popup events can arrive after their temporary Qt objects have been deleted.
            pyatspi.Registry.registerEventListener(observe_popup,
                "object:state-changed:focused", "object:state-changed:showing",
                "object:active-descendant-changed")

            def visible_nodes(node=accessible, depth=0):
                showing = node.getState().contains(pyatspi.STATE_SHOWING)
                if depth and not showing:
                    return
                yield node
                for child in node:
                    if child:
                        yield from visible_nodes(child, depth + 1)
                if depth == 0:
                    for popup in tuple(popup_roots):
                        try:
                            yield from visible_nodes(popup, 1)
                        except GLib.GError:
                            popup_roots.remove(popup)

            def record(label, target=window):
                from gi.repository import GLib
                context = GLib.MainContext.default()
                while context.pending():
                    context.iteration(False)
                nodes = [{"role": node.getRoleName(), "name": node.name,
                          "selected": node.getState().contains(pyatspi.STATE_SELECTED),
                          "checked": node.getState().contains(pyatspi.STATE_CHECKED),
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

            def navigation_item(title):
                navigation = next((node for node in visible_nodes()
                    if node.name == "Main navigation"), None)
                if not navigation:
                    return None
                return next((node for node in visible_nodes(navigation)
                    if node.name == title and node.getRoleName() in
                    ("button", "push button", "toggle button", "radio button")), None)

            def navigation_selected(title):
                item = navigation_item(title)
                return item and item.getState().contains(pyatspi.STATE_CHECKED)

            def leave_sidebar():
                toggle = navigation_item("Collapse sidebar")
                if toggle:
                    click(toggle)
                left, top, width, height = display.geometry(window)
                assert display.test.XTestFakeMotionEvent(display.display, -1,
                    left + width - 30, top + height // 2, 0)
                display.x.XSync(display.display, False)
                wait_for("collapsed sidebar", lambda: navigation_item("Welcome")
                    .queryComponent().getExtents(pyatspi.DESKTOP_COORDS).width == 48, process)

            def open_application_menu(title):
                click(navigation_item("Advanced"))
                def menu_item():
                    accessible.clearCache() # Qt popup windows can appear after the application tree was cached.
                    return next((node for node in visible_nodes()
                        if node.name.replace("&", "") == title
                        and node.getRoleName() in ("menu", "menu item")), None)
                try:
                    menu = wait_for(title + " in Advanced", menu_item, process)
                except AssertionError:
                    record("advanced-menu-failure")
                    print("Popup events:", popup_events[-30:], flush=True)
                    raise
                # Popups extend outside the main window when opened near its bottom.
                assert menu.queryAction().doAction(0), title + " submenu did not open"
                time.sleep(0.2)

            def named_window(title):
                for child in capture.screen().root.query_tree().children:
                    try:
                        name = child.get_full_property(
                            capture.intern_atom("_NET_WM_NAME"), capture.intern_atom("UTF8_STRING"))
                        name = name.value.decode("utf-8") if name else child.get_wm_name()
                        if name == title:
                            return child.id
                    except xerror.BadWindow:
                        continue
                return None

            # Keyboard navigation exercises the actual application mode shortcuts.
            try:
                wait_for("startup Welcome selection", lambda: navigation_selected("Welcome"), process)
            except AssertionError:
                record("startup-selection-failure")
                raise
            try:
                wait_for("Welcome introduction", lambda: any(node.name == "Welcome to DroniNG"
                         for node in visible_nodes()), process)
            except AssertionError:
                record("welcome-render-failure")
                raise
            record("initial")
            primary_titles = ("Welcome", "Map", "Configuration", "System", "Firmware")
            navigation = [navigation_item(title) for title in primary_titles]
            assert all(navigation), "The sidebar must include all five main sections"
            bounds = [item.queryComponent().getExtents(pyatspi.DESKTOP_COORDS) for item in navigation]
            assert len({rect.x for rect in bounds}) == 1, "Main sections must align in the sidebar"
            assert all(first.y + first.height <= second.y for first, second in zip(bounds, bounds[1:])), \
                "The main sections must follow the requested vertical order"
            assert not any(node.name in primary_titles and node.getRoleName() == "page tab"
                for node in visible_nodes()), "The old workspace tabs must be hidden"
            report["main_navigation"] = {"placement": "left sidebar", "sections": primary_titles}
            leave_sidebar()
            assert not any(node.getRoleName() == "menu bar" for node in visible_nodes()), \
                "Application menus belong in Advanced"
            assert navigation_item("Advanced") and navigation_item("Exit")
            exit_bounds = navigation_item("Exit").queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
            left, top, width, height = display.geometry(window)
            assert exit_bounds.y + exit_bounds.height >= top + height - 20, "Exit belongs at bottom left"
            click(navigation_item("Welcome"))
            time.sleep(0.2)
            assert navigation_item("Welcome").queryComponent().getExtents(pyatspi.DESKTOP_COORDS).width == 48, \
                "Hovering or selecting a section must not expand the sidebar"
            click(navigation_item("Expand sidebar"))
            wait_for("expanded sidebar by button", lambda: navigation_item("Welcome")
                .queryComponent().getExtents(pyatspi.DESKTOP_COORDS).width == 188, process)
            record("sidebar-expanded")
            left, top, width, height = display.geometry(window)
            assert display.test.XTestFakeMotionEvent(display.display, -1, left + width - 30, top + height // 2, 0)
            display.x.XSync(display.display, False)
            time.sleep(0.2)
            assert navigation_item("Welcome").queryComponent().getExtents(pyatspi.DESKTOP_COORDS).width == 188, \
                "An expanded sidebar stays open when the pointer leaves"
            leave_sidebar()
            record("sidebar-collapsed")
            open_application_menu("Tools")
            record("advanced-menu")
            send("Escape")
            time.sleep(0.2)
            send("Escape")
            wait_for("Advanced menu dismissed", lambda: not any(node.getRoleName() == "popup menu"
                for node in visible_nodes()), process)
            leave_sidebar()
            assert not any(node.name == "Connections:" for node in visible_nodes()), \
                "Welcome must use the controller CTA without the legacy connection footer"
            assert not any(node.getRoleName() == "combo box" for node in visible_nodes()), \
                "Welcome must not expose a second controller selector"
            if arguments.check_usb_access_startup:
                assert any(node.name == "Enable USB connections" for node in visible_nodes()), \
                    "Missing USB rules must be explained on startup"
                assert not any(node.name.startswith("sudo install -m 0644 ") for node in visible_nodes()), \
                    "Keep the command collapsed in the introduction"
                copy = next((node for node in visible_nodes()
                    if node.name == "Copy setup command"
                    and node.getRoleName() in ("button", "push button")), None)
                assert copy, "Startup setup needs a Copy command action"
                assert not any(node.getRoleName() == "dialog" for node in visible_nodes()), \
                    "Startup USB setup must be inline"
                click(copy)
                wait_for("setup command copied", lambda: any(node.name == "Command copied"
                    and node.getRoleName() in ("button", "push button") for node in visible_nodes()), process)
                assert navigation_selected("Welcome")
                record("startup-usb-access")
                view = next(node for node in visible_nodes() if node.name == "View command"
                            and node.getRoleName() in ("button", "push button"))
                click(view)
                command = wait_for("expanded setup command", lambda: next((node.name for node in visible_nodes()
                    if node.name.startswith("sudo install -m 0644 ")), None), process)
                rules = application / "rules.udev"
                if not rules.exists():
                    rules = application.parents[1] / "shared/usb_ids/droning.udev"
                assert rules.is_file() and str(rules) in command, command
                assert command.endswith("/etc/udev/rules.d/45-droning.rules && sudo udevadm control --reload-rules"), command
                record("startup-usb-command")
                # Change only the rules directory bound into this private mount
                # namespace. Never install rules into the host system.
                installed = Path(environment["DRONING_TEST_RULES_DIR"]) / "45-droning.rules"
                shutil.copyfile(rules, installed)
                wait_for("automatic USB setup completion", lambda: any(node.name == "Ready to connect"
                    for node in visible_nodes()), process)
                assert not any(node.name in ("Copy setup command", "Command copied", "View command", "Hide command")
                    for node in visible_nodes()), "Completed setup must clear its instructions and actions"
                record("startup-usb-ready")
                controller_cta = next(node for node in visible_nodes() if node.name == "Connect controller"
                    and node.getRoleName() in ("button", "push button"))
                click(controller_cta)
                wait_for("connection CTA navigation", lambda: any(node.name == "Connect your controller"
                    for node in visible_nodes()), process)
                display.keys(window, "Control_L", "1")
                wait_for("return to Welcome", lambda: any(node.name == "Welcome to DroniNG"
                    for node in visible_nodes()), process)
                installed.unlink()
                wait_for("USB setup needed again", lambda: any(node.name == "Copy setup command"
                    for node in visible_nodes()), process)
                view = next(node for node in visible_nodes() if node.name == "View command"
                            and node.getRoleName() in ("button", "push button"))
                click(view)
                wait_for("setup command redisclosure", lambda: any(node.name.startswith("sudo install -m 0644 ")
                    for node in visible_nodes()), process)
                hide = next(node for node in visible_nodes() if node.name == "Hide command"
                            and node.getRoleName() in ("button", "push button"))
                click(hide)
                wait_for("collapsed setup command", lambda: not any(node.name.startswith("sudo install -m 0644 ")
                    for node in visible_nodes()), process)
                display.x.XMoveResizeWindow(display.display, window, 120, 80, 800, 740)
                display.x.XSync(display.display, False)
                wait_for("narrow Welcome layout", lambda: display.geometry(window)[2] == 800, process)
                time.sleep(0.4)
                record("startup-usb-narrow")
                display.resize(window)
                community = next(node for node in visible_nodes()
                    if " ".join(node.name.replace("&", "").split()) == "Community updates"
                    and node.getRoleName() in ("button", "push button", "check box"))
                click(community)
                wait_for("expanded community toggle", lambda: any(
                    " ".join(node.name.replace("&", "").split()) == "Hide community updates"
                    for node in visible_nodes()), process)
                left, top, width, height = display.geometry(window)
                assert display.test.XTestFakeMotionEvent(display.display, -1, left + width - 40, top + height // 2, 0)
                for direction in (5, 4):
                    for _ in range(12):
                        for pressed in (True, False):
                            assert display.test.XTestFakeButtonEvent(display.display, direction, pressed, 0)
                    display.x.XSync(display.display, False)
                    time.sleep(0.4)
                    if direction == 5:
                        try:
                            wait_for("visible community section", lambda: any(node.name == "Community updates"
                                for node in visible_nodes()), process)
                        except AssertionError:
                            record("welcome-community-failure")
                            raise
                        record("welcome-community")
                community = next(node for node in visible_nodes()
                    if " ".join(node.name.replace("&", "").split()) == "Hide community updates"
                    and node.getRoleName() in ("button", "push button", "check box"))
                click(community)
                wait_for("collapsed community section", lambda: not any(node.name == "Community updates"
                    for node in visible_nodes()), process)
                click(navigation_item("Firmware"))
                wait_for("Firmware from Welcome action", lambda: navigation_selected("Firmware"), process)
                assert not any(node.name == "Enable USB connections" for node in visible_nodes()), \
                    "Welcome setup must not occupy other workspaces"
                print("PASS: Welcome setup card, copy, disclosure, automatic completion and responsive workspace actions", flush=True)
            for shortcut, label, title in ((1, "welcome", "Welcome"), (2, "map", "Map"),
                                           (3, "configuration", "Configuration"),
                                           (4, "system", "System"), (5, "firmware", "Firmware")):
                tab_started = time.monotonic()
                click(navigation_item(title))
                wait_for(title + " sidebar selection", lambda: navigation_selected(title), process)
                display.keys(window, "Control_L", str(shortcut))
                wait_for(title + " keyboard selection", lambda: navigation_selected(title), process)
                leave_sidebar()
                time.sleep(0.5)
                if title == "Map":
                    map_started = tab_started
                    wait_for("current map location resolved", lambda: any("30 × 30 km" in node.name
                        or "Location unavailable." in node.name for node in visible_nodes()), process, seconds=6)
                    def map_tiles_rendered():
                        node = next(node for node in visible_nodes() if node.name == "Local map")
                        rect = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
                        raw = capture.screen().root.get_image(rect.x + rect.width // 2 - 100,
                            rect.y + rect.height // 2 - 100, 200, 200, X.ZPixmap, 0xffffffff)
                        sample = Image.frombytes("RGB", (200, 200), raw.data, "raw", "BGRX")
                        return len(sample.getcolors(40000) or []) > 500
                    def map_view_filled():
                        node = next(node for node in visible_nodes() if node.name == "Local map")
                        rect = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
                        # Check the whole view, including its edges. First imagery
                        # alone misses serialized downloads and partly empty maps.
                        raw = capture.screen().root.get_image(rect.x + 4, rect.y + 4,
                            rect.width - 8, rect.height - 8, X.ZPixmap, 0xffffffff)
                        sample = Image.frombytes("RGB", (rect.width - 8, rect.height - 8), raw.data, "raw", "BGRX")
                        colors = dict((color, count) for count, color in sample.getcolors(sample.width * sample.height))
                        empty = colors.get((20, 22, 24), 0) + colors.get((69, 75, 82), 0)
                        return empty / (sample.width * sample.height) < 0.01
                    try:
                        wait_for("map imagery rendered", map_tiles_rendered, process, seconds=8)
                    except AssertionError:
                        record("map-render-failure")
                        raise
                    report["map_startup_seconds"] = round(time.monotonic() - map_started, 2)
                    try:
                        wait_for("entire map view rendered", map_view_filled, process, seconds=12)
                    except AssertionError:
                        record("map-full-view-failure")
                        raise
                    report["map_full_view_seconds"] = round(time.monotonic() - map_started, 2)
                for status in ("Flight time", "Arm status", "Flight mode and failsafe", "Telemetry TX/RX and alerts"):
                    node = next((node for node in visible_nodes() if node.name == status), None)
                    assert node, status + " must be visible on " + title
                    bounds = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
                    assert bounds.y >= display.geometry(window)[1] + display.geometry(window)[3] - 65, \
                        status + " belongs in the global footer"
                if title == "Firmware" and arguments.check_firmware_installation:
                    cards = {name: next(node for node in visible_nodes()
                        if node.name == name and node.getRoleName() == "panel")
                        .queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
                        for name in ("Attitude", "Aircraft", "System health")}
                    assert cards["Attitude"].x == cards["System health"].x
                    assert cards["Aircraft"].y == cards["System health"].y
                    assert cards["Aircraft"].x < cards["Attitude"].x
                    assert cards["Attitude"].y < cards["Aircraft"].y
                    assert cards["Attitude"].width > 320, "Dashboard columns must fill the available width"
                assert not navigation_item("Flight Data"), "Map replaces Flight Data"
                assert not any(node.name == "Connections:" for node in visible_nodes()), \
                    "Workspaces must not restore the legacy connection footer"
                if arguments.check_usb_access_startup and title != "Welcome":
                    assert not any(node.name == "Enable USB connections" for node in visible_nodes()), \
                        "Setup belongs to the Welcome page"
                record(label)

            click(navigation_item("System"))
            for shortcut, title in ((6, "Scopes"), (7, "Advanced")):
                tab = wait_for(title + " System tool", lambda: next((node for node in visible_nodes()
                    if node.name == title and node.getRoleName() == "page tab"), None), process)
                click(tab)
                wait_for(title + " selection", lambda: tab.getState().contains(pyatspi.STATE_SELECTED), process)
                assert navigation_selected("System"), "System tools must keep System active in the sidebar"
                display.keys(window, "Control_L", str(shortcut))
                wait_for(title + " keyboard selection", lambda: tab.getState().contains(pyatspi.STATE_SELECTED), process)
                time.sleep(0.5)
                record(title.lower())

            if arguments.check_firmware_installation:
                tab = wait_for("Firmware workspace", lambda: navigation_item("Firmware"), process)
                click(tab)
                assert not named_window("Find dRonin firmware")
                wait_for("controller connection page", lambda: any(node.name == "Connect your controller"
                    for node in visible_nodes()), process)
                assert not any(node.name == "Connections:" for node in visible_nodes()), \
                    "Firmware owns its connection controls"
                for title in ("Attitude", "Aircraft", "System health"):
                    card = next((node for node in visible_nodes()
                        if node.name == title and node.getRoleName() in ("panel", "filler")), None)
                    assert card and not card.getState().contains(pyatspi.STATE_ENABLED), \
                        title + " stays unavailable until live firmware is detected"
                assert any(node.name == "Flight time" for node in visible_nodes()), \
                    "Firmware must retain the global flight status footer"
                controller_list = next(node for node in visible_nodes()
                    if node.name in ("Controller", "Waiting for a controller…") and node.getRoleName() == "combo box")
                connect_action = next(node for node in visible_nodes()
                    if node.name == "Connect controller" and node.getRoleName() in ("button", "push button"))
                assert not connect_action.getState().contains(pyatspi.STATE_ENABLED), \
                    "Connect stays disabled with no plausible USB controller"
                assert any(node.name == "Plug in your controller" for node in visible_nodes())
                assert not any(node.name == "Controller already in BOOT mode?" for node in visible_nodes())
                assert not any(node.name in ("1  Connect", "2  Identify", "3  Install", "4  Ready")
                    for node in visible_nodes()), "Connection must not assume installation intent"
                assert not any(node.name == "Detected firmware:" for node in visible_nodes()), \
                    "Disconnected state should guide connection rather than show empty identity fields"
                record("firmware-connect")
                def card_bounds(name):
                    return next(node for node in visible_nodes() if node.name == name
                        and node.getRoleName() in ("panel", "filler")) \
                        .queryComponent().getExtents(pyatspi.DESKTOP_COORDS)

                previous_width = card_bounds("Attitude").width
                display.x.XMoveResizeWindow(display.display, window, 80, 40, 1300, 900)
                display.x.XSync(display.display, False)
                wait_for("dashboard expands with window", lambda: card_bounds("Attitude").width > previous_width + 70, process)
                large = card_bounds("Controller setup")
                heading = next(node for node in visible_nodes() if node.name == "Connect your controller") \
                    .queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
                assert large.x <= heading.x and large.y <= heading.y \
                    and heading.x + heading.width <= large.x + large.width, "Controller heading belongs inside its card"
                record("firmware-wide")
                display.x.XMoveResizeWindow(display.display, window, 80, 40, 800, 740)
                display.x.XSync(display.display, False)
                wait_for("dashboard becomes one column", lambda: card_bounds("Attitude").x == card_bounds("Controller setup").x, process)
                narrow = card_bounds("Attitude")
                assert narrow.width > previous_width, "A narrow window uses one full-width card per row"
                assert narrow.y >= card_bounds("Controller setup").y + card_bounds("Controller setup").height
                record("firmware-narrow")
                display.resize(window)
                wait_for("dashboard returns to two columns", lambda: card_bounds("Attitude").x > card_bounds("Controller setup").x, process)
                def dashboard_scroll(bottom):
                    pane = next(node for node in visible_nodes() if node.name == "Firmware dashboard")
                    pane_bounds = pane.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
                    bars = [node for node in visible_nodes(pane) if node.getRoleName() == "scroll bar"
                        and node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS).height >
                            node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS).width
                        and node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS).x >=
                            pane_bounds.x + pane_bounds.width - 30]
                    if bars:
                        value = bars[0].queryValue()
                        value.currentValue = value.maximumValue if bottom else value.minimumValue
                    time.sleep(0.3)

                dashboard_scroll(True)
                recovery_card = card_bounds("Maintenance and recovery")
                log_card = card_bounds("Connection history")
                assert recovery_card.x < log_card.x and recovery_card.y == log_card.y, \
                    "Collapsed maintenance and log cards belong in the bottom grid row"
                maintenance = next(node for node in visible_nodes()
                    if node.name.replace("&", "").replace("  ", " ") == "Maintenance recovery")
                click(maintenance)
                wait_for("maintenance opens as a full-width grid card", lambda:
                    card_bounds("Maintenance and recovery").width > recovery_card.width * 1.8, process)
                record("firmware-maintenance-card")
                display.x.XMoveResizeWindow(display.display, window, 80, 40, 800, 740)
                display.x.XSync(display.display, False)
                wait_for("expanded maintenance follows the narrow grid", lambda:
                    card_bounds("Maintenance and recovery").width < 720, process)
                maintenance_bounds = card_bounds("Maintenance and recovery")
                maintenance_panel = next(node for node in visible_nodes()
                    if node.name == "Maintenance and recovery" and node.getRoleName() in ("panel", "filler"))
                for node in visible_nodes(maintenance_panel):
                    if node.getRoleName() in ("button", "push button", "combo box"):
                        bounds = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
                        assert maintenance_bounds.x <= bounds.x and bounds.x + bounds.width <= \
                            maintenance_bounds.x + maintenance_bounds.width, node.name + " overflows the maintenance card"
                record("firmware-maintenance-narrow")
                display.resize(window)
                wait_for("expanded maintenance restores its width", lambda:
                    card_bounds("Maintenance and recovery").width > 900, process)
                dashboard_scroll(True)
                log_toggle = next(node for node in visible_nodes() if node.name == "Connection log"
                    and node.getRoleName() in ("button", "push button", "toggle button", "check box"))
                click(log_toggle)
                dashboard_scroll(True)
                assert any(node.getRoleName() == "text" and node.name == "Connection log"
                    for node in visible_nodes()), "Connection log expands inside its own card"
                record("firmware-log-card")
                assert log_toggle.queryAction().doAction(0)
                assert maintenance.queryAction().doAction(0)
                dashboard_scroll(False)
                if arguments.check_controller_setup:
                    assert not named_window("droniNG Setup Wizard"), "Connecting must not launch setup"
                    assert not any(node.name == "Firmware maintenance" for node in visible_nodes()), \
                        "Maintenance stays collapsed until requested"
                    open_application_menu("Tools")
                    wizard_action = wait_for("setup wizard menu action", lambda: next((node for node in visible_nodes()
                        if node.name == "Vehicle Setup Wizard" and node.getRoleName() == "menu item"), None), process)
                    assert wizard_action.queryAction().doAction(0), "Setup wizard menu action did not run"
                    wait_for("explicitly launched setup wizard", lambda: named_window("droniNG Setup Wizard"), process)
                    record("controller-setup-wizard")
                    cancel = next(node for node in visible_nodes()
                        if node.name == "Cancel" and node.getRoleName() in ("button", "push button"))
                    click(cancel)
                    wait_for("setup wizard closed", lambda: not named_window("droniNG Setup Wizard"), process)
                    assert not named_window("Setup Wizard Cancelled"), "Cancellation must not prompt again"
                    assert navigation_selected("Firmware"), "Wizard preserves the Firmware workspace"
                    record("controller-setup-cancelled")
                maintenance = next(node for node in visible_nodes()
                    if node.name.replace("&", "").replace("  ", " ") == "Maintenance recovery")
                dashboard_scroll(True)
                click(maintenance)
                install_entry = wait_for("explicit installation choice", lambda: next((node for node in visible_nodes()
                    if node.name == "Install DroniNG…" and node.getRoleName() in ("button", "push button")), None), process)
                click(install_entry)
                wait_for("BOOT-mode board selection", lambda: any(node.name == "Board to install on"
                    for node in visible_nodes()), process)
                assert not any(node.name in ("Firmware", "Bootloader", "Not detected")
                    and node.getRoleName() == "label"
                    for node in visible_nodes()), "Manual installation should not show empty identity fields"
                record("firmware-detection")
                board = wait_for("board selector", lambda: next((node for node in visible_nodes()
                    if node.name in ("Board to install on", "Select board…")
                    and node.getRoleName() == "combo box"), None), process)
                assert board.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)[2] <= 360, \
                    "Board selector must stay compact in a wide workspace"
                assert board.queryAction().doAction(0), "Board selector did not open"
                wait_for("board selector popup", lambda: any(node.getRoleName() == "list item"
                         and node.name == "Select board…" for node in visible_nodes()), process)
                record("firmware-board-popup")
                send("Home")
                # Both board selectors show every model alphabetically by its display name.
                catalog_data = json.loads((Path(__file__).resolve().parents[2]
                    / "shared/firmware/board-catalog.json").read_text())
                ordered_boards = sorted(catalog_data["boards"], key=lambda item: item["name"].casefold())
                index = next(i for i, item in enumerate(ordered_boards)
                             if item["id"] == "hglrc-f4-v6pro") + 1
                for _ in range(index):
                    send("Down")
                send("Return")
                wait_for("selected board model", lambda: board.name == "HGLRC F4 V6PRO", process)
                assert not any(node.name.startswith("STM32 system bootloader")
                    or "Recommended firmware: Revolution." in node.name
                    or node.name == "Install the recommended firmware to continue."
                    for node in visible_nodes()), "Keep the overview focused on the board and installation action"
                assert sum(node.name == "HGLRC F4 V6PRO" for node in visible_nodes()) == 1, \
                    "Board model must appear once"
                assert any(node.name == "Currently installed" for node in visible_nodes())
                assert any(node.name == "Will install" for node in visible_nodes())
                product = json.loads((Path(__file__).resolve().parents[2]
                    / "shared/product-version.json").read_text())
                release = product["name"] + " " + product["version"]
                assert any(node.name == release + " · Revolution" for node in visible_nodes()), \
                    "Explain the firmware family that will be installed"
                assert any(node.name == "DroniNG" and node.getRoleName() == "label"
                    for node in visible_nodes()), "Show the bootloader that will be installed"
                assert any(node.name == "Board hardware settings will be applied automatically."
                    for node in visible_nodes())
                record("firmware-board-selected")
                install = wait_for("validated installation image", lambda: next((node for node in visible_nodes()
                    if node.name == "Install DroniNG" and node.getRoleName() in ("button", "push button")
                    and node.getState().contains(pyatspi.STATE_ENABLED)), None), process)
                record("firmware-install-available")
                assert any("Existing firmware and settings will be replaced." in node.name
                           for node in visible_nodes()), "Show the installation effect before authorizing it"
                click(install)
                title = "Install dRonin — HGLRC F4 V6PRO"
                assert not named_window(title), "Installation must stay in the Firmware tab"
                wait_for("hardware BOOT guidance", lambda: any("BOOT button" in node.name
                    for node in visible_nodes()), process)
                assert not any(node.name == "Install DroniNG"
                    and node.getRoleName() in ("button", "push button") for node in visible_nodes()), \
                    "Installation must continue automatically without a second Install button"
                wait_for("installation discovery state", lambda: any(
                    "Installation starts automatically" in node.name
                    or "Linux USB access is unavailable" in node.name for node in visible_nodes()), process)
                assert any("BOOT button beside the USB socket" in node.name for node in visible_nodes()) \
                    and any("flight battery" in node.name for node in visible_nodes()), \
                    "Show the selected board's boot procedure"
                assert any(node.name == "Board manual" for node in visible_nodes()), \
                    "Provide the selected board's manual during bootloader entry"
                discovery_failed = any("Linux USB access is unavailable" in node.name for node in visible_nodes())
                assert any(node.name == "Retry" for node in visible_nodes()) == discovery_failed, \
                    "Retry should appear only when an operation fails"
                assert sum("BOOT button" in node.name for node in visible_nodes()) == 1, \
                    "BOOT instructions must appear once"
                assert not any(node.name == "Installation log" for node in visible_nodes()), \
                    "Installation log must start collapsed"
                progress_bars = [node for node in visible_nodes() if node.getRoleName() == "progress bar"]
                assert len(progress_bars) == (0 if discovery_failed else 1), \
                    "Show one progress bar for the active installation task"
                if progress_bars:
                    assert progress_bars[0].name == "Detecting BOOT mode…"
                    assert any(node.name == "Detecting BOOT mode…" and node.getRoleName() == "label"
                               for node in visible_nodes()), "Name the task beside its progress bar"
                record("firmware-install-started")
                details_toggle = next(node for node in visible_nodes() if node.name == "View log")
                click(details_toggle)
                wait_for("expanded installation log", lambda: any(node.name == "Installation log"
                    for node in visible_nodes()), process)
                assert sum("BOOT button" in node.name for node in visible_nodes()) == 1
                record("firmware-install-details")
                click(details_toggle)
                wait_for("collapsed installation log", lambda: not any(node.name == "Installation log"
                    for node in visible_nodes()), process)
                cancel = next(node for node in visible_nodes()
                    if node.name == "Cancel" and node.getRoleName() in ("button", "push button"))
                click(cancel)
                wait_for("firmware installation cancellation", lambda: not any("BOOT button" in node.name
                    for node in visible_nodes()), process)
                assert not named_window("Find dRonin firmware")
                assert any(node.name == "Board to install on" for node in visible_nodes())
                record("firmware-install-cancelled")
                report["firmware_installation_checked"] = {
                    "board": "hglrc-f4-v6pro", "target": "revolution",
                    "image": "validated packaged image", "entry": "hardware BOOT",
                    "authorization": "single Install click; no second Install button",
                    "usb": "isolated /dev; no physical USB device nodes",
                    "write": "cancelled with physical USB devices isolated"}
                print("PASS: Firmware tab detection, validated image, inline BOOT guidance and cancellation", flush=True)

            if arguments.check_replay_seek:
                display.keys(window, "Control_L", "4")
                wait_for("System workspace before replay input", lambda: navigation_selected("System"), process)
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
                try:
                    wait_for("idle replay seek to ten seconds",
                             lambda: position.queryValue().currentValue == 10, process)
                except AssertionError:
                    print("Replay control bounds:", position.queryComponent().getExtents(pyatspi.DESKTOP_COORDS), flush=True)
                    print("Focused controls:", [(node.getRoleName(), node.name) for node in visible_nodes()
                        if node.getState().contains(pyatspi.STATE_FOCUSED)], flush=True)
                    parent = position
                    while parent and parent.getRoleName() != "application":
                        print("Replay ancestor:", parent.getRoleName(), parent.name,
                              parent.queryComponent().getExtents(pyatspi.DESKTOP_COORDS), flush=True)
                        parent = parent.parent
                    record("replay-seek-failure")
                    raise
                record("replay-seek-idle")
                click(position)
                send("Control_L", "a")
                send("0")
                send("Return")
                wait_for("idle replay seek back to zero",
                         lambda: position.queryValue().currentValue == 0, process)
                report["idle_replay_seek_checked"] = True
                print("PASS: actual replay position control works with no log loaded", flush=True)

            # Keyboard selection keeps the selected configuration page visible
            # when the navigation list needs to scroll.
            display.keys(window, "Control_L", "3")
            for index, title in enumerate(("Vehicle", "Input", "Output", "Attitude", "Stabilization", "Modules", "Camera Stab")):
                tab = wait_for(title + " tab", lambda: next((node for node in visible_nodes()
                               if node.name == title and node.getRoleName() == "list item"), None), process)
                if index:
                    send("Down")
                else:
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
                open_application_menu("Tools" if menu == "t" else "Help")
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

            click(navigation_item("Exit"))
            exit_code = process.wait(timeout=15)
            assert exit_code == 0, f"Application did not close normally (exit {exit_code})"
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
    parser.add_argument("--check-firmware-installation", action="store_true",
                        help="Check the Firmware tab and installation flow with physical USB access isolated")
    parser.add_argument("--check-controller-setup", action="store_true",
                        help="Check explicit wizard launch and cancellation from the Firmware workspace")
    parser.add_argument("--check-usb-access-startup", action="store_true",
                        help="Check Welcome setup and recovery with USB rules isolated")
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
            command = ["dbus-run-session", "--", "xvfb-run", "-d",
                       "--server-args=-screen 0 1440x1000x24", sys.executable,
                       str(Path(__file__).resolve()), *sys.argv[1:], "--inside-private-session"]
            if arguments.check_firmware_installation or arguments.check_usb_access_startup:
                # The Install click authorizes automatic flashing. Hide physical USB
                # devices from the entire test session before launching the real GCS.
                bwrap = shutil.which("bwrap")
                if not bwrap:
                    raise RuntimeError("Firmware UI checks require bubblewrap for USB isolation")
                output = arguments.output.resolve()
                rule_isolation = []
                if arguments.check_usb_access_startup:
                    private_rules = session / "usb-rules"
                    private_rules.mkdir()
                    environment["DRONING_TEST_RULES_DIR"] = str(private_rules)
                    for directory in ("/etc/udev/rules.d", "/run/udev/rules.d",
                                      "/usr/lib/udev/rules.d", "/usr/local/lib/udev/rules.d"):
                        if Path(directory).is_dir():
                            if directory == "/etc/udev/rules.d":
                                rule_isolation.extend(("--bind", str(private_rules), directory))
                            else:
                                rule_isolation.extend(("--tmpfs", directory))
                command = [bwrap, "--ro-bind", "/", "/", "--dev", "/dev",
                           "--tmpfs", "/sys", "--tmpfs", "/run/udev",
                           "--bind", "/tmp", "/tmp", "--bind", str(output), str(output),
                           *rule_isolation, *command]
            result = subprocess.run(command, env=environment, timeout=150)
        finally:
            shutil.rmtree(session)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
