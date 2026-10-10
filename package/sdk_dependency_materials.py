#!/usr/bin/env python3
"""Prepare pinned FFmpeg/ICU source materials for the selected Qt SDK.

These are companion materials, not a complete application source distribution.
No downloads, vendor-script execution or changes to the runtime package occur.
The catalogue records observed provenance and unresolved source/build gates.
"""

import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
import tarfile

from qt_source_materials import digest


def relative_path(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or path == PurePosixPath("."):
        raise RuntimeError(f"Unsafe material path: {value}")
    return path


def archive_notices(archive_path, expected):
    notices = {}
    names = set()
    for name in expected:
        relative_path(name)
    with tarfile.open(archive_path) as archive:
        for member in archive:
            path = relative_path(member.name)
            if not member.isfile():
                continue
            relative = str(PurePosixPath(*path.parts[1:]))
            if relative in names:
                raise RuntimeError(f"Duplicate archive file: {member.name}")
            names.add(relative)
            if relative in expected:
                notices[relative] = archive.extractfile(member).read()
    missing = sorted(set(expected) - notices.keys())
    if missing:
        raise RuntimeError(f"Missing archive notices: {archive_path}: {', '.join(missing)}")
    return notices


def prepare(application, sdk, cache, output, catalog):
    application = application.resolve(strict=True)
    sdk = sdk.resolve(strict=True)
    cache = cache.resolve(strict=True)
    manifest = json.loads((application / "qt-runtime.json").read_text())
    if manifest["qt_version"] != catalog["qt_version"]:
        raise RuntimeError("Dependency catalogue does not match the package's Qt version")
    if output.exists():
        raise RuntimeError(f"Use a new output directory to preserve previous materials: {output}")
    materials = {}
    for name, record in catalog["materials"].items():
        path = (cache / relative_path(name)).resolve(strict=True)
        if not path.is_relative_to(cache) or digest(path) != record["sha256"]:
            raise RuntimeError(f"Dependency material checksum or cache path differs: {name}")
        materials[name] = path
    libraries = []
    notices = {}
    for component, record in catalog["components"].items():
        relative_path(component)
        for name, wanted in record["sdk_library_sha256"].items():
            if len(relative_path(name).parts) != 1:
                raise RuntimeError(f"Expected a library basename: {name}")
            source = (sdk / "lib" / name).resolve(strict=True)
            if not source.is_relative_to(sdk) or digest(source) != wanted:
                raise RuntimeError(f"SDK dependency differs from pinned library: {name}")
            deployed = (application / "lib/droning" / name).resolve(strict=True)
            if not deployed.is_relative_to(application) or digest(deployed) != wanted:
                raise RuntimeError(f"Deployed dependency differs from pinned library: {name}")
            libraries.append({"component": component,
                              "path": f"lib/droning/{name}", "sha256": wanted,
                              "verification": "matches_pinned_sdk_bytes"})
        notices[component] = archive_notices(materials[record["source_archive"]],
                                             record["notice_members"])

    # Validate every input first. Preserve earlier bundles rather than overwrite.
    output.mkdir(parents=True)
    (output / "materials").mkdir()
    for name, source in materials.items():
        destination = output / "materials" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    for component, files in notices.items():
        for name, contents in files.items():
            destination = output / "notices" / component / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(contents)
    (output / "sdk-dependency-sources.json").write_text(json.dumps(catalog, indent=2) + "\n")
    report = {"qt_version": catalog["qt_version"],
              "components": sorted(catalog["components"]),
              "deployed_libraries": libraries,
              "remaining_source_gates": catalog["remaining_source_gates"],
              "material_files_sha256": {
                  str(path.relative_to(output)): digest(path)
                  for path in sorted(output.rglob("*")) if path.is_file()}}
    (output / "sdk-dependency-materials.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Prepared {len(materials)} pinned materials and "
          f"{sum(map(len, notices.values()))} notice files; "
          f"{len(libraries)} deployed libraries match the pinned SDK. "
          "Remaining source gates are recorded in the report.")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        catalog = json.loads(Path(__file__).with_name("sdk_dependency_sources.json").read_text())
        prepare(args.application, args.sdk, args.cache, args.output, catalog)
    except (OSError, RuntimeError, ValueError, KeyError, tarfile.TarError) as error:
        print(f"SDK dependency preparation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
