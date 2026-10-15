#!/usr/bin/env python3
"""Prepare hash-checked Qt source archives, licences and SDK inventories.

This produces the Qt portion of a release's source materials. It does not
certify a complete application distribution or fetch files during packaging.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tarfile


FFMPEG_STUBS = {
    "libQt6FFmpegStub-crypto.so.3", "libQt6FFmpegStub-ssl.so.3",
    "libQt6FFmpegStub-va.so.2", "libQt6FFmpegStub-va-drm.so.2",
    "libQt6FFmpegStub-va-x11.so.2",
}
QML_OWNERS = {"QML": "qtdeclarative", "Qt": "qtdeclarative",
              "QtQml": "qtdeclarative", "QtQuick": "qtdeclarative",
              "Qt5Compat": "qt5compat", "QtCharts": "qtcharts",
              "QtMultimedia": "qtmultimedia"}


def digest(path, algorithm="sha256"):
    result = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def source_hashes(path):
    if not path.is_file():
        return {}
    text = path.read_text(encoding="utf-8")
    records = dict(re.findall(
        r"^FileName: \./(.+)\nSPDXID: [^\n]+\nFileChecksum: SHA1: ([0-9a-f]{40})$",
        text, re.MULTILINE))
    if not records or len(records) != len(re.findall(r"^FileName: ", text, re.MULTILINE)):
        raise RuntimeError(f"Incomplete or unsupported SDK source inventory: {path}")
    return records


def audit_archive(path, wanted, expected):
    if digest(path) != wanted:
        raise RuntimeError(f"Source archive checksum differs: {path}")
    found = {}
    licences = {}
    members = set()
    with tarfile.open(path) as archive:
        for member in archive:
            full = PurePosixPath(member.name)
            if full.is_absolute() or ".." in full.parts or len(full.parts) < 1:
                raise RuntimeError(f"Unsafe archive member: {member.name}")
            relative = str(PurePosixPath(*full.parts[1:]))
            if not member.isfile():
                continue
            if relative in members:
                raise RuntimeError(f"Duplicate archive member: {relative}")
            members.add(relative)
            if relative in expected or relative.startswith("LICENSES/"):
                contents = archive.extractfile(member).read()
                if relative in expected:
                    found[relative] = hashlib.sha1(contents).hexdigest()
                if relative.startswith("LICENSES/"):
                    licences[relative] = contents
    if not licences:
        raise RuntimeError(f"Source archive contains no LICENSES files: {path}")
    different = {name: {"sdk_sha1": expected[name], "archive_sha1": found[name]}
                 for name in sorted(expected.keys() & found.keys())
                 if expected[name] != found[name]}
    missing = sorted(expected.keys() - found.keys())
    # Release tarballs omit Git ignore/attribute files and may regenerate .tag.
    # Record every exception; no implementation or build file may be skipped.
    unexpected = [name for name in different
                  if name != ".tag"]
    unexpected += [name for name in missing
                   if PurePosixPath(name).name not in (".gitignore", ".gitattributes")]
    if unexpected:
        raise RuntimeError(f"Source archive differs from SDK source inventory: "
                           f"{path}: {', '.join(unexpected[:20])}")
    return {"source_inventory_available": bool(expected),
            "source_file_count": len(expected),
            "matching_source_files": len(found) - len(different),
            "different_metadata_files": different,
            "omitted_git_metadata_files": missing}, licences


def sdk_inventory(sdk, version):
    documents = {}
    files = {}
    for path in sorted((sdk / "sbom").glob(f"*-{version}.spdx.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        module = path.name.removesuffix(f"-{version}.spdx.json")
        documents[module] = (path, data)
        for record in data.get("files", []):
            source = (sdk / record["fileName"]).resolve()
            if not source.is_relative_to(sdk):
                raise RuntimeError(f"SDK inventory path escapes installation: {source}")
            if source.is_file():
                files.setdefault(source, []).append((module, record))
    if not documents:
        raise RuntimeError(f"SDK has no binary inventories for Qt {version}: {sdk}")
    return documents, files


def deployed_files(application, sdk, index):
    records = []
    runtime = application / "lib/droning"
    for path in sorted(runtime.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(runtime)
        family = relative.parts[0]
        if family == "qtplugins":
            source = sdk / "plugins" / Path(*relative.parts[1:])
        elif family in ("qml", "translations"):
            source = sdk / relative
        elif len(relative.parts) == 1 and path.name.startswith("libQt6"):
            source = sdk / "lib" / path.name
        else:
            continue
        source = source.resolve(strict=True)
        if not source.is_relative_to(sdk) or digest(path) != digest(source):
            raise RuntimeError(f"Deployed Qt file differs from selected SDK: {relative}")
        candidates = index.get(source, [])
        valid = [(module, record) for module, record in candidates
                 if any(value["algorithm"] == "SHA1"
                        and value["checksumValue"] == digest(path, "sha1")
                        for value in record.get("checksums", []))]
        if candidates and len(valid) != 1:
            raise RuntimeError(f"Deployed Qt file differs from SDK binary inventory: {relative}")
        entry = {"path": str(path.relative_to(application)),
                 "sdk_path": str(source.relative_to(sdk)), "sha256": digest(path)}
        if valid:
            module, record = valid[0]
            entry.update(module=module, verification="sdk_binary_inventory",
                         license_expression=record["licenseConcluded"])
        else:
            if family == "qml" and len(relative.parts) > 1:
                module = QML_OWNERS.get(relative.parts[1])
            elif path.name in FFMPEG_STUBS:
                module = "qtmultimedia"
            else:
                module = None
            if module is None:
                raise RuntimeError(f"Qt file has no inventory or explicit module owner: {relative}")
            entry.update(module=module, verification="identical_sdk_file",
                         license_expression="NOASSERTION")
        records.append(entry)
    if not records:
        raise RuntimeError("Package contains no deployed Qt files")
    return records


def build_tools(sdk, index, definitions):
    records = []
    for name, module in sorted(definitions.items()):
        path = PurePosixPath(name)
        if (path.is_absolute() or ".." in path.parts or str(path) != name
                or len(path.parts) != 2 or path.parts[0] not in ("bin", "libexec")):
            raise RuntimeError(f"Unsafe Qt build tool path: {name}")
        source = (sdk / path).resolve(strict=True)
        if not source.is_relative_to(sdk) or not source.is_file():
            raise RuntimeError(f"Qt build tool escapes selected SDK: {name}")
        sha1 = digest(source, "sha1")
        valid = [record for owner, record in index.get(source, []) if owner == module
                 and any(value["algorithm"] == "SHA1" and value["checksumValue"] == sha1
                         for value in record.get("checksums", []))]
        if len(valid) != 1:
            raise RuntimeError(f"Qt build tool differs from SDK binary inventory: {name}")
        records.append({"sdk_path": name, "module": module, "sha256": digest(source),
                        "sdk_sha1": sha1, "license_expression": valid[0]["licenseConcluded"]})
    return records


def prepare(application, sdk, archives, output, catalog):
    application = application.resolve(strict=True)
    sdk = sdk.resolve(strict=True)
    manifest = json.loads((application / "qt-runtime.json").read_text(encoding="utf-8"))
    version = manifest["qt_version"]
    if version != catalog["qt_version"]:
        raise RuntimeError(f"Source catalogue is for Qt {catalog['qt_version']}, "
                           f"package uses {version}")
    if output.exists():
        raise RuntimeError(f"Use a new output directory to preserve previous materials: {output}")
    documents, index = sdk_inventory(sdk, version)
    records = deployed_files(application, sdk, index)
    tools = build_tools(sdk, index, catalog.get("build_tools", {}))
    modules = sorted({entry["module"] for entry in records + tools})
    audited = {}
    for module in modules:
        if module not in catalog["modules"]:
            raise RuntimeError(f"Source catalogue omits deployed module: {module}")
        path = archives / f"{module}-everywhere-src-{version}.tar.xz"
        sbom = sdk / "sbom" / f"{module}-{version}.source.spdx"
        if not sbom.is_file() and module != "qttranslations":
            raise RuntimeError(f"SDK source inventory is missing: {sbom}")
        result, licences = audit_archive(path, catalog["modules"][module], source_hashes(sbom))
        result.update(archive=path.name, sha256=catalog["modules"][module],
                      source_url=catalog["source_url_prefix"] + path.name,
                      license_files=sorted(licences))
        audited[module] = (result, licences, path, sbom)

    output.mkdir(parents=True)
    for folder in ("archives", "sbom", "licenses"):
        (output / folder).mkdir()
    for module, (_, licences, path, sbom) in audited.items():
        shutil.copy2(path, output / "archives" / path.name)
        document = documents[module][0]
        shutil.copy2(document, output / "sbom" / document.name)
        if sbom.is_file():
            shutil.copy2(sbom, output / "sbom" / sbom.name)
        for relative, contents in licences.items():
            destination = output / "licenses" / module / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(contents)
    report = {"qt_version": version,
              "deployed_file_count": len(records),
              "verification_counts": dict(Counter(entry["verification"] for entry in records)),
              "modules": {module: value[0] for module, value in audited.items()},
              "deployed_files": records,
              "build_tools": tools,
              "material_files_sha256": {
                  str(path.relative_to(output)): digest(path) for path in sorted(output.rglob("*"))
                  if path.is_file()}}
    (output / "qt-source-materials.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared Qt {version}: {len(modules)} module source archives; "
          f"{len(records)} deployed SDK files verified; materials {output}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--archives", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        catalog = json.loads(Path(__file__).with_name("qt_runtime_sources.json").read_text())
        prepare(args.application, args.sdk, args.archives, args.output, catalog)
    except (OSError, RuntimeError, ValueError, KeyError, tarfile.TarError) as error:
        print(f"Qt source material preparation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
