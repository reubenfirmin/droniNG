#!/usr/bin/env python3
"""Run packaged UI checks and save their output to ui-checks/run-ui.log."""

import argparse
from collections import deque
import os
from pathlib import Path
import shutil
import subprocess
import sys
import traceback


def report(message, log, error=False):
    print(message, file=sys.stderr if error else sys.stdout, flush=True)
    log.write(message + "\n")
    log.flush()


def run_logged(command, root, log, environment=None, native_log=None):
    """Capture subprocess output without requiring shell redirection."""
    recent = deque(maxlen=4)
    with subprocess.Popen(command, cwd=root, env=environment, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          errors="replace", bufsize=1) as process:
        for line in process.stdout:
            log.write(line)
            log.flush()
            if native_log is not None:
                native_log.write(line)
                native_log.flush()
            recent.append(line.rstrip())
        return process.wait(), recent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-native", action="store_true",
                        help="Run packaged UI checks without repeating the native diagnostic suites")
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = root / "build"
    binaries = [
        binary
        for pattern in ("package*/droning*/bin/droning-gcs", "*/package*/droning*/bin/droning-gcs")
        for binary in build.glob(pattern)
        if not binary.parents[1].name.endswith((".staging", ".previous"))
    ]
    if not binaries:
        parser.exit(1, "No packaged application found under build/. Build a ground package first.\n")
    application = max(binaries, key=lambda binary: binary.parents[1].stat().st_mtime).parents[1]
    output = application.parent.parent / "ui-checks"
    output.mkdir(parents=True, exist_ok=True)
    log_path = output / "run-ui.log"
    with log_path.open("w") as log:
        description = f"Testing: {application}\nEvidence: {output}\nLog: {log_path}"
        report(description, log)
        try:
            return check_application(root, application, output, log, arguments.skip_native)
        except Exception:
            traceback.print_exc(file=log)
            raise


def check_application(root, application, output, log, skip_native=False):
    report("Checking native diagnostic framing and object schemas…", log)
    result, recent = run_logged([
        sys.executable, str(root / "ground/tests/uavtalk_inspection.py"),
    ], root, log)
    if result:
        print("\n".join(recent), file=sys.stderr, flush=True)
        return result
    native = application.parent.parent / "connection-journey-tests/connection-diagnostics-tests"
    if native.exists() and not skip_native:
        bwrap = shutil.which("bwrap")
        if not bwrap:
            report("Native controller checks require bubblewrap for USB isolation.", log, error=True)
            return 1
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        environment["CONNECTION_DIALOG_CAPTURE_DIR"] = str(output / "native-captures")
        # Give the native tests the delivered helper and resources at their
        # normal application-relative paths, without changing the package.
        runtime = output / "native-runtime"
        (runtime / "bin").mkdir(parents=True, exist_ok=True)
        shutil.copy2(native, runtime / "bin" / native.name)
        shutil.copy2(application / "bin/dfu-util", runtime / "bin/dfu-util")
        for directory in ("lib", "share", "firmware"):
            link = runtime / directory
            if link.is_symlink():
                link.unlink()
            link.symlink_to(application / directory, target_is_directory=True)
        command = [bwrap, "--ro-bind", "/", "/", "--dev", "/dev",
                   "--tmpfs", "/tmp", "--tmpfs", "/sys", "--tmpfs", "/run/udev",
                   "--bind", str(output), str(output), str(runtime / "bin" / native.name)]
        native_log_path = output / "native-tests.log"
        report("Running controller and connection checks…", log)
        with native_log_path.open("w") as native_log:
            result, recent = run_logged(command, root, log, environment, native_log)
        print("\n".join(recent), flush=True)
        if result:
            report(f"Native checks failed; details: {native_log_path}", log, error=True)
            return result
    upgrade = application.parent.parent / "firmware-upgrade-tests/firmware-upgrade-tests"
    if upgrade.exists() and not skip_native:
        bwrap = shutil.which("bwrap")
        if not bwrap:
            report("Upgrade checks require bubblewrap for USB and network isolation.", log, error=True)
            return 1
        runtime = output / "upgrade-runtime"
        (runtime / "bin").mkdir(parents=True, exist_ok=True)
        shutil.copy2(upgrade, runtime / "bin" / upgrade.name)
        for directory in ("lib", "share", "firmware"):
            link = runtime / directory
            if link.is_symlink():
                link.unlink()
            link.symlink_to(application / directory, target_is_directory=True)
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        environment["LD_LIBRARY_PATH"] = os.pathsep.join((
            str(application / "lib"), str(application / "lib/droning"),
            str(application / "lib/droning/plugins/droniNG")))
        environment["QT_PLUGIN_PATH"] = str(application / "lib/droning/qtplugins")
        environment["FIRMWARE_UPGRADE_CAPTURE_DIR"] = str(output / "upgrade-captures")
        environment["FIRMWARE_UPGRADE_IMAGE_ROOT"] = str(application / "firmware")
        command = [bwrap, "--ro-bind", "/", "/", "--dev", "/dev", "--unshare-net",
                   "--tmpfs", "/tmp", "--tmpfs", "/sys", "--tmpfs", "/run/udev",
                   "--bind", str(output), str(output), str(runtime / "bin" / upgrade.name)]
        report("Running offline upgrade, backup and rendered card checks against packaged libraries…", log)
        upgrade_log_path = output / "upgrade-tests.log"
        with upgrade_log_path.open("w") as upgrade_log:
            result, recent = run_logged(command, root, log, environment, upgrade_log)
        print("\n".join(recent), flush=True)
        if result:
            report(f"Upgrade checks failed; details: {upgrade_log_path}", log, error=True)
            return result
    report("Running packaged application UI checks…", log)
    result, recent = run_logged([
        sys.executable, str(root / "ground/tests/application_ui.py"),
        "--application", str(application),
        "--profile", str(root / "ground/gcs/share/default_configurations/developer.xml"),
        "--output", str(output),
        "--check-firmware-installation", "--check-controller-setup",
        "--check-usb-access-startup", "--check-replay-seek",
    ], root, log)
    report("Packaged application UI checks " + ("failed." if result else "passed."), log)
    if result:
        print("\n".join(recent), file=sys.stderr)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
