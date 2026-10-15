#!/usr/bin/env python3
"""Compile untouched upstream ICM426xx logic against a bounded host PiOS mock."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SURVEY = ROOT / "flight/Betaflight"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    cc = shlex.split(args.cc)
    spec = importlib.util.spec_from_file_location("desk_check", Path(__file__).with_name("betaflight-desk-check.py"))
    audit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(audit)
    _, manifest = audit.verify_sources()
    env = dict(os.environ, CCACHE_DISABLE="1")
    upstream = SURVEY / "upstream/betaflight/src/main"
    source = upstream / "drivers/accgyro/accgyro_spi_icm426xx.c"
    host = SURVEY / "survey/icm42688_host.c"
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-DUSE_GYRO_SPI_ICM42688P",
             "-I" + str(SURVEY / "survey/mock"), "-I" + str(upstream), "-I" + str(ROOT / "flight/PiOS/inc")]
    report = {"scope": "host mock only; no MCU binary, IRQ, RTOS, physical SPI or timing validation",
              "repositories": manifest["repositories"],
              "compiler": subprocess.run(cc + ["--version"], env=env, capture_output=True, text=True,
                                         check=True).stdout.splitlines()[0], "results": []}
    with tempfile.TemporaryDirectory(prefix="droning-driver-probe-") as temp:
        for name, extra in (("internal_clock", []), ("external_clock", ["-DUSE_GYRO_CLKIN"])):
            executable = Path(temp) / name
            compile_command = cc + flags + extra + ["-fsanitize=undefined", "-fsanitize-undefined-trap-on-error",
                                                    str(source), str(host), "-lm", "-o", str(executable)]
            subprocess.run(compile_command, env=env, check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
            print(result.stdout.strip())
            obj = Path(temp) / (name + ".o")
            subprocess.run(cc + flags + extra + ["-c", str(source), "-o", str(obj)], env=env, check=True)
            size = subprocess.run(["size", str(obj)], text=True, capture_output=True, check=True).stdout
            fields = size.splitlines()[1].split()
            report["results"].append({"mode": name, "status": "passed", "undefined_behavior_sanitizer": True,
                                      "message": result.stdout.strip(), "sanitizer_mode": "trap on undefined behavior",
                                      "host_upstream_object_bytes": dict(zip(("text", "data", "bss"), map(int, fields[:3]))),
                                      "object_size_scope": "host object only, excludes bridge; not ARM flash/RAM estimate"})
    destination = SURVEY / "survey/driver-probe.json"
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Wrote {destination.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
