#!/usr/bin/env python3
"""Preserve pinned application dependencies and restore them for an offline build."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

from application_source_materials import safe_name, source_record, verify_snapshot, write_snapshot
from qt_source_materials import digest


def git(checkout, *arguments, **kwargs):
    return subprocess.run(["git", "-C", str(checkout), *arguments], check=True, **kwargs)


def commit_sources(checkout, revision):
    head = git(checkout, "rev-parse", "HEAD", capture_output=True, text=True).stdout.strip()
    if head != revision:
        raise RuntimeError(f"Application dependency revision differs: {checkout}")
    tree = git(checkout, "ls-tree", "-r", "-z", "HEAD", capture_output=True).stdout
    records = {}
    for row in tree.split(b"\0"):
        if not row:
            continue
        header, raw_name = row.split(b"\t", 1)
        mode, kind, blob = header.split()
        name = raw_name.decode("utf-8")
        safe_name(name)
        if mode == b"160000" and kind == b"commit":
            records[name] = {"type": "gitlink", "revision": blob.decode()}
            continue
        if kind != b"blob" or mode not in (b"100644", b"100755", b"120000"):
            raise RuntimeError("Dependency has unsupported source inputs")
        record = source_record(checkout, name)
        if record["type"] == "deleted":
            raise RuntimeError(f"Missing application dependency source: {name}")
        contents = (record["target"].encode() if record["type"] == "symlink"
                    else (checkout / name).read_bytes())
        wanted_mode = b"120000" if record["type"] == "symlink" else (
            b"100755" if record["mode"] == 0o755 else b"100644")
        actual = hashlib.sha1(b"blob " + str(len(contents)).encode() + b"\0" + contents).hexdigest()
        if actual != blob.decode() or wanted_mode != mode:
            raise RuntimeError(f"Dependency working-tree source differs from pinned commit: {name}")
        records[name] = record
    return records


def verify(output):
    output = output.resolve(strict=True)
    report = json.loads((output / "application-dependency-materials.json").read_text())
    for name, wanted in report["files_sha256"].items():
        safe_name(name)
        path = (output / name).resolve(strict=True)
        if not path.is_relative_to(output) or digest(path) != wanted:
            raise RuntimeError(f"Application dependency material checksum differs: {name}")
    actual = {str(path.relative_to(output)) for path in output.rglob("*") if path.is_file()}
    if actual != set(report["files_sha256"]) | {"application-dependency-materials.json"}:
        raise RuntimeError("Application dependency material file set differs")
    catalog = json.loads((output / "application-dependency-sources.json").read_text())
    if report["components"].keys() != catalog["components"].keys():
        raise RuntimeError("Application dependency catalogue differs from inventory")
    for name, component in report["components"].items():
        if any(component.get(key) != value for key, value in catalog["components"][name].items()):
            raise RuntimeError("Application dependency catalogue differs from inventory")
        verify_snapshot(output / component["archive"], component["source_files"], name + "-source")
        for notice in component["notices"]:
            if digest(output / "notices" / name / notice) != component["source_files"][notice]["sha256"]:
                raise RuntimeError(f"Application dependency notice differs from source: {name}/{notice}")
    return report


def prepare(repository, output, catalog=None):
    repository = repository.resolve(strict=True)
    if output.exists() or output.is_symlink():
        raise RuntimeError("Use a fresh application dependency output")
    if catalog is None:
        catalog = json.loads(Path(__file__).with_name("application_dependency_sources.json").read_text())
    tools = (repository / "make/tools.mk").read_text()
    source_records = {}
    for name, component in catalog["components"].items():
        safe_name(name)
        safe_name(component["checkout"])
        revision = component["revision"]
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise RuntimeError("Invalid application dependency revision")
        pattern = rf"^{re.escape(component['pin_variable'])}\s*:=\s*{revision}\s*$"
        if not re.search(pattern, tools, re.MULTILINE):
            raise RuntimeError(f"Dependency catalogue differs from build pin: {name}")
        checkout = (repository / component["checkout"]).resolve(strict=True)
        if not checkout.is_relative_to(repository):
            raise RuntimeError("Application dependency checkout escapes repository")
        source_records[name] = commit_sources(checkout, revision)
        links = {path: record["revision"] for path, record in source_records[name].items()
                 if record["type"] == "gitlink"}
        declared = component.get("gitlinks", {})
        if links != {path: link["revision"] for path, link in declared.items()}:
            raise RuntimeError(f"Dependency submodule references differ from catalogue: {name}")
        for link in declared.values():
            if ("component" in link
                    and catalog["components"][link["component"]]["revision"] != link["revision"]):
                raise RuntimeError("Required dependency submodule pin differs")
        if name == "breakpad" and "--disable-selftest" not in tools:
            raise RuntimeError("The selected Breakpad recipe must disable optional self-tests")
        for notice in component["notices"]:
            if notice not in source_records[name] or source_records[name][notice]["type"] != "file":
                raise RuntimeError(f"Missing application dependency notice: {name}/{notice}")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="application-dependencies-", dir=output.parent))
    try:
        report = {"components": {}, "files_sha256": {}}
        for name, component in catalog["components"].items():
            checkout = repository / component["checkout"]
            archive_name, pack_name = name + ".tar.xz", name + ".git.pack"
            files = {path: record for path, record in source_records[name].items()
                     if record["type"] != "gitlink"}
            records = write_snapshot(checkout, sorted(files), stage / archive_name, name + "-source")
            if records != files:
                raise RuntimeError(f"Application dependency sources changed: {name}")
            with (stage / pack_name).open("wb") as pack:
                git(checkout, "pack-objects", "--revs", "--stdout",
                    input=(component["revision"] + "\n").encode(), stdout=pack,
                    stderr=subprocess.PIPE)
            # Packs contain existing commit/tree/blob objects, not local Git configuration.
            report["components"][name] = dict(component, archive=archive_name,
                                              object_pack=pack_name, source_files=source_records[name])
            for notice in component["notices"]:
                target = stage / "notices" / name / notice
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(checkout / notice, target)
            if commit_sources(checkout, component["revision"]) != source_records[name]:
                raise RuntimeError(f"Application dependency changed during preparation: {name}")
        (stage / "application-dependency-sources.json").write_text(json.dumps(catalog, indent=2) + "\n")
        report["files_sha256"] = {str(path.relative_to(stage)): digest(path)
                                  for path in sorted(stage.rglob("*")) if path.is_file()}
        (stage / "application-dependency-materials.json").write_text(json.dumps(report, indent=2) + "\n")
        verify(stage)
        stage.rename(output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return report


def restore(output, repository):
    report = verify(output)
    repository = repository.resolve(strict=True)
    for component in report["components"].values():
        safe_name(component["checkout"])
        target = repository / component["checkout"]
        if not target.parent.resolve().is_relative_to(repository):
            raise RuntimeError("Dependency restore path escapes repository")
        if target.exists() or target.is_symlink():
            raise RuntimeError(f"Refusing to replace existing dependency sources: {target}")
    for name, component in report["components"].items():
        target = repository / component["checkout"]
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="dependency-restore-", dir=target.parent) as directory:
            with tarfile.open(output / component["archive"]) as archive:
                archive.extractall(directory, filter="data")
            checkout = Path(directory) / (name + "-source")
            git(checkout, "init", "-q", stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            with (output / component["object_pack"]).open("rb") as pack:
                git(checkout, "index-pack", "--stdin", stdin=pack, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE)
            (checkout / ".git/HEAD").write_text(component["revision"] + "\n")
            # Treat this preserved commit as a shallow root; parent history is not a build input.
            (checkout / ".git/shallow").write_text(component["revision"] + "\n")
            git(checkout, "read-tree", "HEAD", stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if commit_sources(checkout, component["revision"]) != component["source_files"]:
                raise RuntimeError(f"Restored dependency does not match pinned commit: {name}")
            checkout.rename(target)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path)
    parser.add_argument("--materials", type=Path, required=True)
    parser.add_argument("--restore", type=Path)
    args = parser.parse_args()
    try:
        if args.repository and args.restore:
            raise RuntimeError("Select preparation or restore, not both")
        if args.repository:
            report = prepare(args.repository, args.materials)
        elif args.restore:
            report = restore(args.materials, args.restore)
        else:
            report = verify(args.materials)
        print(f"Verified {len(report['components'])} pinned application dependencies and "
              f"{sum(len(c['source_files']) for c in report['components'].values())} source paths.")
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError,
            tarfile.TarError) as error:
        print(f"Application dependency materials failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
