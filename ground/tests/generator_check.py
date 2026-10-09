#!/usr/bin/env python3
"""Compare every generator backend and check that failed writes fail the process."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


BACKENDS = ("flight", "gcs", "java", "matlab", "wireshark")


def generate(binary, repository, output, *options):
    result = subprocess.run(
        [str(binary), *options, str(repository / "shared/uavobjectdefinition"),
         str(repository)],
        cwd=output, env={**os.environ, "LC_ALL": "C.UTF-8"},
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120,
    )
    (output / "generator.log").write_bytes(result.stdout)
    match = re.search(rb"Id hash is ([0-9a-f]{16})\.", result.stdout)
    if not match:
        raise AssertionError(f"Missing object hash; see {output / 'generator.log'}")
    return result.returncode, match.group(1).decode("ascii")


def manifest(root):
    return {path.relative_to(root): path.read_bytes()
            for path in root.rglob("*") if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    repository = Path(__file__).resolve().parents[2]
    reference = arguments.reference.resolve(strict=True)
    candidate = arguments.candidate.resolve(strict=True)
    arguments.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="generator-check-", dir=arguments.output)).resolve()
    hashes = []
    manifests = []
    for name, binary in (("reference", reference), ("candidate", candidate)):
        directory = output / name
        directory.mkdir()
        code, object_hash = generate(binary, repository, directory)
        assert code == 0, f"{name} failed with status {code}; see {directory / 'generator.log'}"
        hashes.append(object_hash)
        for backend in BACKENDS:
            assert (directory / object_hash / backend).is_dir(), f"Missing {backend} output"
        manifests.append(manifest(directory / object_hash))
    assert hashes[0] == hashes[1], f"Object hashes differ: {hashes}"
    differences = sorted(str(path) for path in manifests[0].keys() | manifests[1].keys()
                         if manifests[0].get(path) != manifests[1].get(path))
    assert not differences, f"Generated files differ: {differences}"
    print(f"PASS: {len(manifests[0])} generated files match; hash {hashes[0]}")

    for backend in BACKENDS:
        directory = output / f"failed-{backend}"
        directory.mkdir()
        hash_directory = directory / hashes[1]
        hash_directory.mkdir()
        # A file in place of the output directory reliably prevents writes.
        (hash_directory / backend).write_text("blocked output directory\n")
        code, object_hash = generate(candidate, repository, directory, f"-{backend}")
        assert object_hash == hashes[1]
        assert code == 3, f"{backend} returned {code} after failed writes; see {directory / 'generator.log'}"
        print(f"PASS: {backend} write failure returns status 3")
    print(f"Evidence: {output}")


if __name__ == "__main__":
    main()
