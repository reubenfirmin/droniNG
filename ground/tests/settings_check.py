#!/usr/bin/env python3
"""Check Qt 5/6 settings migration without modifying any input profiles."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


class Runtime:
    def __init__(self, name, binary, libraries, output):
        self.name = name
        self.binary = binary.resolve(strict=True)
        self.output = output / name
        self.output.mkdir()
        self.calls = 0
        self.environment = dict(os.environ)
        for variable in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                         "QML_IMPORT_PATH", "QML2_IMPORT_PATH"):
            self.environment.pop(variable, None)
        self.environment.update(
            LD_LIBRARY_PATH=libraries, QT_QPA_PLATFORM="offscreen",
            QT_QPA_PLATFORMTHEME="", QT_FORCE_STDERR_LOGGING="1",
        )
        for variable, directory in (("XDG_RUNTIME_DIR", "runtime"),
                                    ("XDG_CONFIG_HOME", "config"),
                                    ("XDG_DATA_HOME", "data"),
                                    ("XDG_CACHE_HOME", "cache")):
            path = self.output / directory
            path.mkdir(mode=0o700)
            self.environment[variable] = str(path)

    def run(self, *arguments):
        self.calls += 1
        log = self.output / f"call-{self.calls:03}.log"
        result = subprocess.run(
            [str(self.binary), *map(str, arguments)],
            env=self.environment, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=15,
        )
        log.write_bytes(result.stdout)
        if result.returncode:
            raise AssertionError(f"{self.name} {arguments[0]} failed; see {log}")

    def normalize(self, source, destination):
        self.run("--normalize", source, destination)
        return json.loads(destination.read_text())


def compare(expected, actual, context):
    changed = sorted(key for key in expected.keys() | actual.keys()
                     if expected.get(key) != actual.get(key))
    if changed:
        # Profiles may contain credentials: report keys, never their values.
        raise AssertionError(f"{context}: changed keys: {changed}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--reference-libraries", required=True)
    parser.add_argument("--candidate-libraries", required=True)
    parser.add_argument("--profile", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    profiles = [(path.resolve(strict=True), False) for path in arguments.profile]
    original_hashes = {path: hashlib.sha256(path.read_bytes()).hexdigest()
                       for path, _ in profiles}
    arguments.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="settings-check-",
                                  dir=arguments.output)).resolve()
    reference = Runtime("qt5", arguments.reference,
                        arguments.reference_libraries, output)
    candidate = Runtime("qt6", arguments.candidate,
                        arguments.candidate_libraries, output)
    runtimes = (reference, candidate)
    for runtime in runtimes:
        path = output / f"{runtime.name}-model.xml"
        runtime.run("--write-model", path)
        profiles.append((path, True))

    for index, (source, model) in enumerate(profiles):
        directory = output / f"profile-{index:02}"
        directory.mkdir()
        expected = reference.normalize(source, directory / "original-qt5.json")
        actual = candidate.normalize(source, directory / "original-qt6.json")
        compare(expected, actual, f"{source.name} initial Qt 5/6 read")
        if model:
            for runtime in runtimes:
                runtime.run("--verify-model", source)

        for first, second in ((reference, candidate), (candidate, reference)):
            direction = f"{first.name}-to-{second.name}"
            current = source
            for step, writer in enumerate((first, second)):
                rewritten = directory / f"{direction}-{step}.xml"
                writer.run("--rewrite", current, rewritten)
                for reader in runtimes:
                    normalized = reader.normalize(
                        rewritten, directory / f"{direction}-{step}-{reader.name}.json")
                    compare(expected, normalized,
                            f"{source.name} {direction} step {step}, {reader.name} read")
                    if model:
                        reader.run("--verify-model", rewritten)
                current = rewritten
        digest = hashlib.sha256(json.dumps(expected, sort_keys=True,
                                          separators=(",", ":")).encode()).hexdigest()
        print(f"PASS: {source.name}: {len(expected)} settings; both migration directions; "
              f"canonical SHA-256 {digest}")

    for source, digest in original_hashes.items():
        if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise AssertionError(f"Input profile was modified: {source.name}")
    print("PASS: input profiles unchanged; model values, arrays, dock layouts, geometry "
          "and splitter state restore in both Qt versions")
    print(f"Evidence: {output}")


if __name__ == "__main__":
    main()
