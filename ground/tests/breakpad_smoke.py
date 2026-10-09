#!/usr/bin/env python3
"""Validate real Linux crash capture and symbolization without launching GCS."""
import pathlib
import resource
import subprocess
import sys
import tempfile


def disable_core_dump():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    binary, tools, output = map(lambda p: pathlib.Path(p).resolve(), sys.argv[1:])
    output.mkdir(parents=True, exist_ok=True)
    dumps = pathlib.Path(tempfile.mkdtemp(prefix="dumps-", dir=output))
    result = subprocess.run([str(binary), str(dumps)], capture_output=True,
                            text=True, timeout=20, preexec_fn=disable_core_dump)
    (output / "capture.log").write_text(result.stdout + result.stderr)
    if result.returncode != 0:
        raise RuntimeError("Crash capture failed; sandbox ptrace restrictions can "
                           "prevent minidumps. See capture.log (exit %s)." % result.returncode)
    dump = pathlib.Path(result.stdout.strip())
    if dump.parent != dumps or not dump.is_file() or dump.stat().st_size == 0:
        raise RuntimeError("Crash callback did not produce a nonempty minidump")

    syms = subprocess.run([str(tools / "dump_syms"), str(binary)],
                          capture_output=True, check=True, timeout=30)
    (output / "dump_syms.log").write_bytes(syms.stderr)
    (output / "smoke.sym").write_bytes(syms.stdout)
    module = syms.stdout.decode().splitlines()[0].split(" ", 4)
    if len(module) != 5 or module[:2] != ["MODULE", "Linux"]:
        raise RuntimeError("Invalid Linux symbol header")
    _, _, arch, identifier, name = module
    symbols = output / "symbols" / name / identifier
    symbols.mkdir(parents=True, exist_ok=True)
    (symbols / (name + ".sym")).write_bytes(syms.stdout)

    walk = subprocess.run([str(tools / "minidump_stackwalk"), str(dump),
                           str(output / "symbols")], capture_output=True,
                          text=True, timeout=30)
    (output / "stackwalk.log").write_text(walk.stdout + walk.stderr)
    if walk.returncode != 0 or "SIGSEGV" not in walk.stdout or "smoke!main" not in walk.stdout:
        raise RuntimeError("Minidump could not be read and symbolized; see stackwalk.log")
    print("Breakpad passed: child SIGSEGV, minidump, Linux %s symbols, resolved main" % arch)
    print("Evidence: %s" % output)


if __name__ == "__main__":
    main()
