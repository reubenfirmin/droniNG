#!/usr/bin/env python3
"""Download the explicitly pinned, unmodified sources for the offline survey."""

import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
SURVEY = ROOT / "flight/PiOS/Betaflight"


def git_blob_hash(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


def download(job):
    key, repository, commit, source_path, expected = job
    destination = SURVEY / "upstream" / key / source_path
    if destination.exists():
        data = destination.read_bytes()
        if git_blob_hash(data) != expected:
            raise RuntimeError(f"Local content differs from the pin: {destination}")
    else:
        url = f"https://raw.githubusercontent.com/{repository}/{commit}/{source_path}"
        request = urllib.request.Request(url, headers={"User-Agent": "droniNG-driver-survey"})
        with urllib.request.urlopen(request, timeout=40) as response:
            data = response.read()
        if git_blob_hash(data) != expected:
            raise RuntimeError(f"Upstream Git blob hash mismatch: {source_path}")
    return destination, data, {
        "repository": key,
        "path": source_path,
        "git_blob_sha1": expected,
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
    }


def main():
    lock = json.loads((SURVEY / "survey.lock.json").read_text())
    jobs = []
    for key, repo in lock["repositories"].items():
        result = subprocess.run(
            ["gh", "api", f"repos/{repo['repository']}/git/trees/{repo['commit']}?recursive=1"],
            check=True, capture_output=True,
        )
        tree = json.loads(result.stdout)
        if tree.get("truncated"):
            raise RuntimeError("GitHub tree is truncated; cannot validate selected inputs")
        blobs = {item["path"]: item["sha"] for item in tree["tree"] if item["type"] == "blob"}
        for source_path in repo["files"]:
            if source_path not in blobs:
                raise RuntimeError(f"File absent at selected pin: {key}/{source_path}")
            jobs.append((key, repo["repository"], repo["commit"], source_path, blobs[source_path]))

    records = []
    # Downloads are independent; perform writes sequentially after hash verification.
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for destination, data, record in pool.map(download, jobs):
            if not destination.exists():
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
            records.append(record)
            print(f"Verified {record['repository']}/{record['path']}", flush=True)

    manifest = {
        "schema_version": 1,
        "repositories": {
            key: {"repository": repo["repository"], "commit": repo["commit"]}
            for key, repo in lock["repositories"].items()
        },
        "files": records,
    }
    (SURVEY / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified {len(records)} files, {sum(r['bytes'] for r in records)} bytes")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Source download failed: {error}", file=sys.stderr)
        sys.exit(1)
