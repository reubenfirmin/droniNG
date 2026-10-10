#!/usr/bin/env python3
"""Audit a Linux package's ELF architectures and required symbol versions."""

import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys


def elf_identity(path):
    with path.open("rb") as stream:
        header = stream.read(20)
    if header[:4] != b"\x7fELF":
        return None
    if len(header) != 20 or header[4] not in (1, 2) or header[5] not in (1, 2):
        raise RuntimeError(f"Invalid ELF header: {path}")
    order = "little" if header[5] == 1 else "big"
    return {"class": 32 if header[4] == 1 else 64,
            "byte_order": order,
            "machine": int.from_bytes(header[18:20], order)}


def command_output(arguments, environment):
    result = subprocess.run(arguments, env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=30)
    if result.returncode:
        raise RuntimeError(f"{arguments[0]} failed for {arguments[-1]}: "
                           f"{result.stdout.strip()} {result.stderr.strip()}")
    return result.stdout + result.stderr


def version_information(text):
    required = defaultdict(set)
    provided = set()
    section = None
    provider = None
    for line in text.splitlines():
        if line.startswith("Version needs section "):
            section = "needs"
        elif line.startswith("Version definition section "):
            section = "definitions"
        elif line.startswith("Version symbols section "):
            section = None
        if section == "needs":
            match = re.search(r"File: (\S+)\s+Cnt:", line)
            if match:
                provider = match[1]
            match = re.search(r"Name: (\S+)\s+Flags: (.*?)\s+Version:", line)
            if match:
                if not provider:
                    raise RuntimeError("ELF version requirement has no provider")
                # Missing weak version requirements do not prevent loading.
                if "WEAK" not in match[2].split():
                    required[provider].add(match[1])
        elif section == "definitions":
            match = re.search(r"\bName: (\S+)", line)
            if match:
                provided.add(match[1])
    return dict(required), provided


def dependency_paths(text):
    if "not found" in text:
        raise RuntimeError(f"Unresolved ELF dependency or symbol version:\n{text.strip()}")
    output = {}
    for line in text.splitlines():
        # Paths can contain spaces, including relocated application directories.
        match = re.match(r"\s*(\S+) => (/.+?) \(0x[0-9a-fA-F]+\)\s*$", line)
        if match:
            output[match[1]] = Path(match[2]).resolve()
            continue
        match = re.match(r"\s*(/.+?) \(0x[0-9a-fA-F]+\)\s*$", line)
        if match:
            path = Path(match[1]).resolve()
            output[Path(match[1]).name] = path
    return output


def collect_abi(application, qt_major=None):
    application = application.resolve(strict=True)
    environment = dict(os.environ, LC_ALL="C")
    for name in ("LD_PRELOAD", "LD_AUDIT"):
        environment.pop(name, None)
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(map(str, (
        application / "lib/droning", application / "lib/droning/plugins/droniNG")))
    executable = application / "bin/droning-gcs.bin"
    expected = elf_identity(executable)
    if expected is None:
        raise RuntimeError(f"Application is not ELF: {executable}")
    files = {}
    for directory in (application / "bin", application / "lib/droning"):
        for path in sorted(directory.rglob("*")):
            if not path.is_file() or path.name.endswith(".debug"):
                continue
            resolved = path.resolve()
            if not resolved.is_relative_to(application):
                raise RuntimeError(f"Runtime file resolves outside package: {path}")
            if resolved in files:
                continue
            identity = elf_identity(path)
            if identity is not None:
                if identity != expected:
                    raise RuntimeError(f"ELF architecture differs from application: {path}")
                files[resolved] = str(resolved.relative_to(application))

    versions = {}
    hashes = {}

    def inspect(path):
        if path not in versions:
            versions[path] = version_information(command_output(
                ["readelf", "--version-info", "--wide", str(path)], environment))
        return versions[path]

    def digest(path):
        if path not in hashes:
            hashes[path] = hashlib.sha256(path.read_bytes()).hexdigest()
        return hashes[path]

    records = []
    providers = {}
    all_requirements = set()
    for path, relative in sorted(files.items(), key=lambda item: item[1]):
        required, _ = inspect(path)
        dependencies = dependency_paths(command_output(["ldd", str(path)], environment))
        for name, provider in dependencies.items():
            qt = re.match(r"libQt([56])", name)
            if qt:
                if qt_major and qt[1] != str(qt_major):
                    raise RuntimeError(f"Mixed Qt major: {relative} loads {name}")
                if not provider.is_relative_to(application):
                    raise RuntimeError(f"Qt provider is outside package: {name}: {provider}")
        for name, wanted in required.items():
            provider = dependencies.get(name)
            if provider is None:
                raise RuntimeError(f"Missing version provider for {relative}: {name}")
            _, available = inspect(provider)
            missing = wanted - available
            if missing:
                raise RuntimeError(f"{relative} requires unavailable versions from {name}: "
                                   f"{', '.join(sorted(missing))}")
            bundled = provider.is_relative_to(application)
            provider_path = str(provider.relative_to(application)) if bundled else str(provider)
            entry = {"path": provider_path, "bundled": bundled, "sha256": digest(provider)}
            previous = providers.setdefault(name, entry)
            if previous != entry:
                raise RuntimeError(f"Inconsistent provider for {name}")
            all_requirements.update(wanted)
        records.append({"path": relative, "sha256": digest(path),
                        "required_symbol_versions": {
                            name: sorted(values) for name, values in sorted(required.items())}})

    highest = {}
    for prefix in ("GLIBC", "GLIBCXX", "CXXABI", "GCC"):
        values = [name for name in all_requirements
                  if re.fullmatch(prefix + r"_\d+(?:\.\d+)*", name)]
        if values:
            highest[prefix] = max(values, key=lambda name: tuple(
                map(int, name.split("_", 1)[1].split("."))))
    try:
        os_release = platform.freedesktop_os_release()
    except OSError:
        os_release = {}
    return {"elf_identity": expected, "elf_file_count": len(records),
            "highest_symbol_versions": highest,
            "validated_host": {"id": os_release.get("ID", "unknown"),
                               "version_id": os_release.get("VERSION_ID", "unknown"),
                               "machine": platform.machine()},
            "version_providers": dict(sorted(providers.items())), "elf_files": records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        manifest = json.loads((arguments.application / "qt-runtime.json").read_text())
        report = collect_abi(arguments.application, manifest["qt_version"].split(".")[0])
        arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"PASS: {report['elf_file_count']} ELF files; all required symbol versions "
              f"are provided on {report['validated_host']}")
        print("Required symbol floors: " + ", ".join(report["highest_symbol_versions"].values()))
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError, KeyError) as error:
        print(f"Linux ABI audit failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
