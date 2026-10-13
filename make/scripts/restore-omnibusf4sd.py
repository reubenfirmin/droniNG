#!/usr/bin/env python3
"""Restore Omnibus F4 SD to Betaflight 3.4.0, with backups and logs built in.

Close DroniNG first. With no arguments, wait for hardware BOOT mode, restore
clean firmware, verify all flash and confirm the running version/board target.
--prepare validates the recovery image without flashing; --check checks startup.
--invalid restores the saved DroniNG two-blink IMU startup failure instead,
then confirms its USB reset loop. Open DroniNG to test recovery and reinstall.
Usage, board procedures and troubleshooting: make/scripts/restore-board.md
"""

import argparse
from datetime import datetime, timezone
import fcntl
from pathlib import Path
import shlex
import signal
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "build/firmware-restore/omnibusf4sd"
IMAGE = "betaflight_3.4.0_OMNIBUSF4SD.hex"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--prepare", action="store_true", help="validate/cache the image without flashing")
    mode.add_argument("--check", action="store_true", help="check startup of the selected state without flashing")
    parser.add_argument("--invalid", action="store_true",
                        help="restore the saved failing DroniNG image for recovery testing")
    parser.add_argument("--usb-path", help="select a USB topology if several controllers are attached")
    parser.add_argument("--wait", type=int, default=120, help="seconds to wait for BOOT mode (default: 120)")
    args = parser.parse_args(argv)
    if args.wait < 1:
        parser.error("--wait must be positive")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    # One recovery at a time, including checks, so concurrent commands cannot
    # compete for the same DFU/serial device or erase another attempt's evidence.
    with (OUTPUT / "restore.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.exit(1, "Another Omnibus recovery command is running. Wait for it to finish.\n")
        command = [sys.executable, "-u", str(ROOT / "make/scripts/restore-board.py"),
                   "--board", "omnibusf4sd", "--output", str(OUTPUT), "--wait", str(args.wait)]
        if args.invalid:
            command.append("--invalid")
        if args.check:
            command.append("--check")
        elif not args.prepare:
            command.append("--flash")
        if args.usb_path:
            command.extend(["--usb-path", args.usb_path])
        # Reuse the byte-verified release saved during the successful recovery.
        # Subsequent runs prefer the utility's cache; neither needs a download.
        evidence = ROOT / "build/qt612-release/omnibusf4sd-tests" / IMAGE
        if not args.invalid and not (OUTPUT / IMAGE).exists() and evidence.is_file():
            command.extend(["--image", str(evidence)])
        log_path = OUTPUT / "run-restore.log"
        with log_path.open("a", buffering=1) as log:
            def report(message):
                print(message, flush=True)
                log.write(message + "\n")

            report(f"\nRecovery command: {datetime.now(timezone.utc).isoformat()}")
            report(f"Log: {log_path}")
            report("$ " + shlex.join(command))
            try:
                with subprocess.Popen(command, cwd=ROOT, text=True, errors="replace", bufsize=1,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
                    try:
                        for line in process.stdout:
                            report(line.rstrip("\n"))
                        result = process.wait()
                    except KeyboardInterrupt:
                        process.send_signal(signal.SIGINT)
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                        report("Recovery interrupted. Reconnect in BOOT mode before retrying if writing had started.")
                        return 130
                report(f"Recovery command exited with status {result}.")
                return result
            except OSError as error:
                report(f"Could not run recovery: {error}")
                return 1


if __name__ == "__main__":
    raise SystemExit(main())
