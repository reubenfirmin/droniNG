#!/usr/bin/env python3
"""Cache complete pinned repositories, then verify and import reviewed files."""

import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
SURVEY = ROOT / "flight/Betaflight"
CACHE = ROOT / "downloads/betaflight"


def git_blob_hash(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


def repository_snapshot(job):
    key, repository, commit = job
    destination = CACHE / f"{key}-{commit}"
    receipt_path = destination / ".droning-source.json"
    if receipt_path.exists():
        receipt = json.loads(receipt_path.read_text())
        if receipt["repository"] != repository or receipt["commit"] != commit:
            raise RuntimeError(f"Wrong cached repository: {destination}")
        print(f"Using complete local snapshot: {repository}@{commit[:12]}", flush=True)
        return destination
    CACHE.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"{key}-", dir=CACHE) as directory:
        temporary = Path(directory)
        archive = temporary / "source.tar.gz"
        url = f"https://codeload.github.com/{repository}/tar.gz/{commit}"
        request = urllib.request.Request(url, headers={"User-Agent": "droniNG-driver-survey"})
        print(f"Downloading complete snapshot: {repository}@{commit[:12]}", flush=True)
        with urllib.request.urlopen(request, timeout=120) as response, archive.open("wb") as output:
            shutil.copyfileobj(response, output)
        prefix = f"{repository.split('/')[-1]}-{commit}"
        with tarfile.open(archive, "r:gz") as source:
            if any(member.name != prefix and not member.name.startswith(prefix + "/") for member in source.getmembers()):
                raise RuntimeError("Repository archive does not match the pinned commit")
            source.extractall(temporary / "tree", filter="data")
        tree = temporary / "tree" / prefix
        with archive.open("rb") as data:
            archive_hash = hashlib.file_digest(data, "sha256").hexdigest()
        receipt = {"repository": repository, "commit": commit, "archiveSha256": archive_hash}
        (tree / ".droning-source.json").write_text(json.dumps(receipt, indent=2) + "\n")
        tree.rename(destination)
    return destination


def import_snapshot(lock_path, manifest_path, snapshots):
    lock = json.loads(lock_path.read_text())
    previous = json.loads(manifest_path.read_text()) if manifest_path.exists() else {"files": []}
    previous_repositories = previous.get("repositories", {})
    known = {(item["repository"], item["path"]): item for item in previous["files"]}
    records = []
    for key, repo in lock["repositories"].items():
        tree = snapshots[(key, repo["repository"], repo["commit"])]
        for source_path in repo["files"]:
            path = Path(source_path)
            if path.is_absolute() or ".." in path.parts:
                raise RuntimeError(f"Unsafe source path: {source_path}")
            data = (tree / path).read_bytes()
            blob = git_blob_hash(data)
            record = {"repository": key, "path": source_path, "git_blob_sha1": blob,
                      "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
            expected = known.get((key, source_path))
            same_pin = previous_repositories.get(key) == {"repository": repo["repository"], "commit": repo["commit"]}
            if expected and same_pin and record != expected:
                raise RuntimeError(f"Cached source differs from the verified pin: {key}/{source_path}")
            destination = SURVEY / "upstream" / key / path
            if destination.exists() and destination.read_bytes() != data:
                raise RuntimeError(f"Local content differs from the pin: {destination}")
            records.append((destination, data, record))

    # Verify every input before changing the reviewed source set.
    for destination, data, _ in records:
        if not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    manifest = {"schema_version": 1,
                "repositories": {key: {"repository": repo["repository"], "commit": repo["commit"]}
                                 for key, repo in lock["repositories"].items()},
                "files": [record for _, _, record in records]}
    contents = json.dumps(manifest, indent=2) + "\n"
    if not manifest_path.exists() or manifest_path.read_text() != contents:
        manifest_path.write_text(contents)
    print(f"Verified {len(records)} reviewed files for {lock_path.name}; complete sources cached in {CACHE}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=SURVEY / "survey.lock.json")
    parser.add_argument("--manifest", type=Path, default=SURVEY / "source-manifest.json")
    parser.add_argument("--all", action="store_true", help="Fetch all survey, Radix and Omnibus pins in one run")
    args = parser.parse_args()
    selections = [(args.lock, args.manifest)]
    if args.all:
        selections = [(SURVEY / "survey.lock.json", SURVEY / "source-manifest.json"),
                      (SURVEY / "radix.lock.json", SURVEY / "radix-source-manifest.json"),
                      (SURVEY / "omnibusf4sd.lock.json", SURVEY / "omnibusf4sd-source-manifest.json")]
    jobs = list(dict.fromkeys((key, repo["repository"], repo["commit"])
                             for lock_path, _ in selections
                             for key, repo in json.loads(lock_path.read_text())["repositories"].items()))
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        snapshots = dict(zip(jobs, pool.map(repository_snapshot, jobs)))
    for lock_path, manifest_path in selections:
        import_snapshot(lock_path, manifest_path, snapshots)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, tarfile.TarError) as error:
        print(f"Source download failed: {error}", file=sys.stderr)
        sys.exit(1)
