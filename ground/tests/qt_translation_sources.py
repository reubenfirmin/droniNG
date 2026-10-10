#!/usr/bin/env python3
"""Rebuild every deployed Qt translation from the pinned official source."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile


def check(application, sdk, archive, output, lrelease=None):
    version = json.loads((application / "qt-runtime.json").read_text())["qt_version"]
    catalog = json.loads((Path(__file__).resolve().parents[2]
                          / "package/qt_runtime_sources.json").read_text())
    if version != catalog["qt_version"]:
        raise RuntimeError("Translation sources do not match the package's Qt version")
    if hashlib.sha256(archive.read_bytes()).hexdigest() != catalog["modules"]["qttranslations"]:
        raise RuntimeError("Translation source archive checksum differs")
    sdk = sdk.resolve(strict=True)
    compiler = (lrelease if lrelease is not None else sdk / "bin/lrelease").resolve(strict=True)
    inventory = json.loads((sdk / "sbom" / f"qttranslations-{version}.spdx.json").read_text())
    expected = {Path(record["fileName"]).name: record for record in inventory["files"]
                if record["fileName"].startswith("./translations/")
                and record["fileName"].endswith(".qm")}
    deployed = sorted((application / "lib/droning/translations").glob("*.qm"))
    if not deployed or {path.name for path in deployed} != expected.keys():
        raise RuntimeError("Deployed translation set differs from the selected SDK inventory")
    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, LD_LIBRARY_PATH=str(sdk / "lib"))
    for name in ("LD_PRELOAD", "LD_AUDIT"):
        environment.pop(name, None)
    records = []
    with tarfile.open(archive) as source:
        members = {}
        cmake = None
        for member in source:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise RuntimeError(f"Unsafe source archive member: {member.name}")
            if member.isfile() and path.suffix == ".ts":
                if path.name in members:
                    raise RuntimeError(f"Duplicate translation source: {path.name}")
                members[path.name] = member
            if member.isfile() and str(path).endswith("/translations/CMakeLists.txt"):
                cmake = source.extractfile(member).read().decode()
        if cmake is None:
            raise RuntimeError("Translation archive is missing its build recipe")
        for deployed_file in deployed:
            ts_name = deployed_file.with_suffix(".ts").name
            ts = output / ts_name
            if ts_name in members:
                contents = source.extractfile(members[ts_name]).read()
                origin = "archive_file"
            elif ts_name.endswith("_en.ts"):
                # Qt's pinned CMake recipe generates empty English catalogues.
                # Keep its exact XML input and verify the compiled result too.
                recipe = '"<TS language=\\"en_US\\"></TS>"'
                if recipe not in cmake:
                    raise RuntimeError("English translation generation recipe has changed")
                contents = b'<TS language="en_US"></TS>'
                origin = "qttranslations_cmake_generated"
            else:
                raise RuntimeError(f"Missing translation source: {ts_name}")
            ts.write_bytes(contents)
            qm = output / deployed_file.name
            result = subprocess.run([str(compiler), "-silent", str(ts), "-qm", str(qm)],
                                    env=environment, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=30)
            if result.returncode:
                raise RuntimeError(f"lrelease failed for {ts_name}: {result.stdout.strip()}")
            contents = qm.read_bytes()
            sha1 = hashlib.sha1(contents).hexdigest()
            if not any(checksum["algorithm"] == "SHA1" and checksum["checksumValue"] == sha1
                       for checksum in expected[deployed_file.name]["checksums"]):
                raise RuntimeError(f"Rebuilt translation differs from SDK inventory: {deployed_file.name}")
            if contents != deployed_file.read_bytes():
                raise RuntimeError(f"Rebuilt translation differs from deployment: {deployed_file.name}")
            records.append({"translation": deployed_file.name,
                            "source_origin": origin,
                            "source_sha256": hashlib.sha256(ts.read_bytes()).hexdigest(),
                            "deployed_sha256": hashlib.sha256(contents).hexdigest()})
    report = {"qt_version": version, "lrelease": str(compiler), "lrelease_sha256": hashlib.sha256(
                  compiler.read_bytes()).hexdigest(), "translations": records}
    (output / "translation-source-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: all {len(records)} deployed translations rebuild byte-identically "
          "from the pinned Qt source and match the SDK inventory.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lrelease", type=Path, help="Use a source-rebuilt translation compiler")
    args = parser.parse_args()
    try:
        check(args.application, args.sdk, args.archive, args.output, args.lrelease)
    except (OSError, RuntimeError, ValueError, KeyError, tarfile.TarError,
            subprocess.SubprocessError) as error:
        print(f"Qt translation source check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
