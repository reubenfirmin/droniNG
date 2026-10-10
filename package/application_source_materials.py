#!/usr/bin/env python3
"""Preserve current application sources without repository metadata or build caches."""

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import shutil
import subprocess
import sys
import tarfile
import tempfile

from qt_source_materials import digest


ARCHIVE_ROOT = "droning-source"
NEW_SOURCE_ROOTS = ("ground/gcs/src", "ground/tests", "package")
NEW_SOURCE_FILES = ("QT_MODERNIZATION_PLAN.md",)
VERSION_METADATA_SOURCE = "make/gcs-version-metadata.json"
VERSION_METADATA_RUNTIME = "share/gcs-version-metadata.json"


def version_metadata(application, head=None):
    path = application / VERSION_METADATA_RUNTIME
    if path.is_symlink() or not path.resolve(strict=True).is_relative_to(application.resolve()):
        raise RuntimeError("GCS version metadata escapes the application")
    contents = path.read_bytes()
    metadata = json.loads(contents)
    if not isinstance(metadata, dict) or metadata.get("schema") != 1 or not isinstance(metadata.get("repository"), dict):
        raise RuntimeError("Missing or invalid preserved GCS version metadata")
    if head is not None and metadata["repository"].get("hash") != head:
        raise RuntimeError("Preserved GCS version metadata records a different repository revision")
    outputs = metadata.get("generated_files_sha256", {})
    if set(outputs) != {"gcsversioninfo.h", "gcsauthors.html", "gcsrelauthors.html"}:
        raise RuntimeError("Preserved GCS version metadata has incomplete generated output hashes")
    for name in ("gcsauthors.html", "gcsrelauthors.html"):
        output = (application / "share" / name).resolve(strict=True)
        if not output.is_relative_to(application.resolve()) or digest(output) != outputs[name]:
            raise RuntimeError(f"Preserved GCS contributors differ from the generated build: {name}")
    return contents


def safe_name(name):
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..", ".git") for part in name.split("/")):
        raise RuntimeError(f"Unsafe application source path: {name}")
    return path


def safe_link(name, target):
    if PurePosixPath(target).is_absolute():
        raise RuntimeError(f"Application source symlink escapes archive: {name}")
    result = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
    if result == ".." or result.startswith("../") or ".git" in PurePosixPath(result).parts:
        raise RuntimeError(f"Application source symlink escapes archive: {name}")


def source_names(repository):
    tracked = subprocess.check_output(["git", "-C", str(repository), "ls-files", "--stage", "-z"])
    names = set()
    for row in tracked.split(b"\0"):
        if not row:
            continue
        header, name = row.split(b"\t", 1)
        mode, _, stage = header.split()
        if stage != b"0" or mode == b"160000":
            raise RuntimeError("Resolve conflicts and submodule source inputs before snapshotting")
        names.add(name.decode("utf-8"))
    new = subprocess.check_output(["git", "-C", str(repository), "ls-files", "--others",
                                   "--exclude-standard", "-z", "--",
                                   *NEW_SOURCE_ROOTS, *NEW_SOURCE_FILES])
    names.update(name.decode("utf-8") for name in new.split(b"\0") if name)
    return sorted(names)


def source_record(repository, name):
    safe_name(name)
    path = repository / name
    if not path.parent.resolve().is_relative_to(repository):
        raise RuntimeError(f"Application source escapes repository: {name}")
    if path.is_symlink():
        target = str(path.readlink())
        safe_link(name, target)
        return {"type": "symlink", "target": target}
    if not path.exists():
        return {"type": "deleted"}
    if not path.is_file():
        raise RuntimeError(f"Application source is not a regular file: {name}")
    return {"type": "file", "size": path.stat().st_size, "sha256": digest(path),
            "mode": 0o755 if path.stat().st_mode & 0o111 else 0o644}


def write_snapshot(repository, names, archive_path, archive_root=ARCHIVE_ROOT, generated_files=None):
    generated_files = generated_files or {}
    if set(names) & set(generated_files):
        raise RuntimeError("Generated application source path collides with repository sources")
    records = {}
    with tarfile.open(archive_path, "w:xz", preset=3) as archive:
        for name in sorted([*names, *generated_files]):
            if name in records:
                raise RuntimeError(f"Duplicate application source path: {name}")
            safe_name(name)
            if name in generated_files:
                contents = generated_files[name]
                record = {"type": "file", "size": len(contents), "mode": 0o644,
                          "sha256": hashlib.sha256(contents).hexdigest()}
            else:
                record = source_record(repository, name)
            records[name] = record
            if record["type"] == "deleted":
                continue
            member = tarfile.TarInfo(archive_root + "/" + name)
            if record["type"] == "symlink":
                member.type = tarfile.SYMTYPE
                member.linkname = record["target"]
                member.mode = 0o777
                archive.addfile(member)
            else:
                contents = generated_files[name] if name in generated_files else (repository / name).read_bytes()
                if hashlib.sha256(contents).hexdigest() != record["sha256"]:
                    raise RuntimeError(f"Application source changed while snapshotting: {name}")
                member.size = len(contents)
                member.mode = record["mode"]
                archive.addfile(member, io.BytesIO(contents))
    return records


def verify_snapshot(archive_path, records, archive_root=ARCHIVE_ROOT):
    if any(record["type"] not in ("file", "symlink", "deleted", "gitlink")
           for record in records.values()):
        raise RuntimeError("Unsupported application source inventory type")
    expected = {name for name, record in records.items() if record["type"] in ("file", "symlink")}
    observed = set()
    with tarfile.open(archive_path) as archive:
        for member in archive:
            prefix = archive_root + "/"
            if not member.name.startswith(prefix):
                raise RuntimeError(f"Unsafe application archive path: {member.name}")
            name = member.name[len(prefix):]
            safe_name(name)
            if name in observed or name not in expected:
                raise RuntimeError(f"Unexpected or duplicate application archive member: {name}")
            observed.add(name)
            record = records[name]
            if record["type"] == "symlink":
                safe_link(name, member.linkname)
                if not member.issym() or member.linkname != record["target"]:
                    raise RuntimeError(f"Application source symlink differs: {name}")
            elif (not member.isfile() or member.size != record["size"]
                  or member.mode != record["mode"]
                  or hashlib.file_digest(archive.extractfile(member), "sha256").hexdigest()
                  != record["sha256"]):
                raise RuntimeError(f"Application source archive differs: {name}")
    if observed != expected:
        raise RuntimeError("Application source archive file set differs from its inventory")


def verify_materials(output, application):
    report = json.loads((output / "application-source-materials.json").read_text())
    archive = output / "droning-source.tar.xz"
    if digest(archive) != report["archive_sha256"]:
        raise RuntimeError("Application source archive checksum differs")
    verify_snapshot(archive, report["source_files"])
    if report.get("generated_source_files"):
        if report["generated_source_files"] != [VERSION_METADATA_SOURCE]:
            raise RuntimeError("Unsupported generated application source inputs")
        contents = version_metadata(application, report["repository_head"])
        record = report["source_files"][VERSION_METADATA_SOURCE]
        if hashlib.sha256(contents).hexdigest() != record["sha256"]:
            raise RuntimeError("Archived GCS version inputs differ from the packaged build")
    if report.get("external_dependencies"):
        from application_dependency_materials import verify
        if verify(output / "dependencies") != report["external_dependencies"]:
            raise RuntimeError("Application dependency inventory differs from source manifest")
    runtime = json.loads((application / "qt-runtime.json").read_text())["linux_abi"]["elf_files"]
    if {item["path"]: item["sha256"] for item in runtime} != report["runtime_elf_sha256"]:
        raise RuntimeError("Application source snapshot records a different runtime")
    for name, wanted in report["runtime_elf_sha256"].items():
        safe_name(name)
        path = (application / name).resolve(strict=True)
        if not path.is_relative_to(application.resolve()) or digest(path) != wanted:
            raise RuntimeError(f"Runtime differs from application source snapshot: {name}")
    return report


def prepare(application, repository, output):
    repository = repository.resolve(strict=True)
    application = application.resolve(strict=True)
    if output.exists() or output.is_symlink():
        raise RuntimeError("Use a fresh output to preserve existing application source materials")
    if subprocess.check_output(["git", "-C", str(repository), "rev-parse", "--show-toplevel"],
                               text=True).strip() != str(repository):
        raise RuntimeError("Application source root must be the repository root")
    names = source_names(repository)
    for required in ("Makefile", "LICENSE.md", "CREDITS.txt", "ground/gcs/gcs.pro"):
        if required not in names or not (repository / required).is_file():
            raise RuntimeError(f"Missing required application source: {required}")
    head = subprocess.check_output(["git", "-C", str(repository), "rev-parse", "HEAD"],
                                   text=True).strip()
    if (repository / VERSION_METADATA_SOURCE).exists() or (repository / VERSION_METADATA_SOURCE).is_symlink():
        raise RuntimeError("Reserved archive version metadata path already exists in the checkout")
    metadata_contents = version_metadata(application, head)
    generated_files = {VERSION_METADATA_SOURCE: metadata_contents}
    runtime = json.loads((application / "qt-runtime.json").read_text())["linux_abi"]["elf_files"]
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="application-sources-", dir=output.parent))
    try:
        archive = stage / "droning-source.tar.xz"
        records = write_snapshot(repository, names, archive, generated_files=generated_files)
        from application_dependency_materials import prepare as prepare_dependencies
        dependencies = prepare_dependencies(repository, stage / "dependencies")
        if (subprocess.check_output(["git", "-C", str(repository), "rev-parse", "HEAD"],
                                    text=True).strip() != head
                or source_names(repository) != names or any(source_record(repository, name) != records[name]
                for name in names) or version_metadata(application, head) != metadata_contents):
            raise RuntimeError("Application sources changed while preparing the snapshot")
        report = {"repository_head": head, "archive_sha256": digest(archive),
                  "source_files": records,
                  "generated_source_files": [VERSION_METADATA_SOURCE],
                  "untracked_source_roots": NEW_SOURCE_ROOTS,
                  "untracked_source_files": NEW_SOURCE_FILES,
                  "runtime_elf_sha256": {item["path"]: item["sha256"] for item in runtime},
                  "external_dependencies": dependencies,
                  "build_correspondence": "unverified; runtime association is not a rebuild proof",
                  "remaining_source_gates": [
                      "Verify a full application rebuild using the recorded source snapshot.",
                      "Complete the application library, asset and build-input notice audit."]}
        (stage / "application-source-materials.json").write_text(json.dumps(report, indent=2) + "\n")
        verify_materials(stage, application)
        stage.rename(output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print(f"Preserved {len(records)} current application source paths; "
          "full rebuild and notice audit remain open.")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--repository", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.repository:
            prepare(args.application, args.repository, args.output)
        else:
            report = verify_materials(args.output, args.application)
            print(f"Verified {len(report['source_files'])} archived source paths and "
                  f"{len(report['runtime_elf_sha256'])} associated runtime ELF hashes.")
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError,
            tarfile.TarError) as error:
        print(f"Application source material verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
