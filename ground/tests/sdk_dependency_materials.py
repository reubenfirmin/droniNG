#!/usr/bin/env python3
"""Check pinned SDK dependency/source boundaries before creating companions."""

import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "package"))
from sdk_dependency_materials import prepare


def sha256(contents):
    return hashlib.sha256(contents).hexdigest()


class DependencyMaterialsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sdk-dependency-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.application = self.root / "application with spaces"
        self.sdk = self.root / "SDK with spaces"
        self.cache = self.root / "source cache"
        self.output = self.root / "prepared materials"
        (self.application / "lib/droning").mkdir(parents=True)
        (self.sdk / "lib").mkdir(parents=True)
        self.cache.mkdir()
        self.binary = b"SDK dependency fixture\n"
        for base in (self.application / "lib/droning", self.sdk / "lib"):
            (base / "libfixture.so.1").write_bytes(self.binary)
        (self.application / "qt-runtime.json").write_text(json.dumps({"qt_version": "6.12.0"}))
        self.notice = b"Copyright and permission notice\n"
        self.source = self.cache / "fixture.tar.gz"
        self.make_archive([("LICENSE", self.notice), ("src/code.c", b"source\n")])
        self.catalog = {
            "qt_version": "6.12.0",
            "materials": {self.source.name: {"sha256": sha256(self.source.read_bytes())}},
            "components": {"fixture": {
                "source_archive": self.source.name, "notice_members": ["LICENSE"],
                "sdk_library_sha256": {"libfixture.so.1": sha256(self.binary)}}},
            "remaining_source_gates": ["Fixture rebuild remains unverified."]}

    def make_archive(self, records):
        with tarfile.open(self.source, "w:gz") as archive:
            for name, contents in records:
                member = tarfile.TarInfo("fixture-source/" + name)
                member.size = len(contents)
                archive.addfile(member, io.BytesIO(contents))

    def run_prepare(self):
        return prepare(self.application, self.sdk, self.cache, self.output, self.catalog)

    def replace_archive(self, records):
        self.make_archive(records)
        self.catalog["materials"][self.source.name]["sha256"] = sha256(self.source.read_bytes())

    def test_preserves_sources_notices_hashes_and_remaining_gates(self):
        report = self.run_prepare()
        self.assertEqual((self.output / "notices/fixture/LICENSE").read_bytes(), self.notice)
        self.assertEqual((self.output / "materials" / self.source.name).read_bytes(),
                         self.source.read_bytes())
        for name, wanted in report["material_files_sha256"].items():
            self.assertEqual(sha256((self.output / name).read_bytes()), wanted)
        self.assertEqual(report["remaining_source_gates"], self.catalog["remaining_source_gates"])
        self.assertEqual(report["deployed_libraries"][0]["sha256"], sha256(self.binary))

    def test_changed_material_fails_before_output(self):
        self.source.write_bytes(b"replaced source archive\n")
        with self.assertRaisesRegex(RuntimeError, "checksum or cache path differs"):
            self.run_prepare()
        self.assertFalse(self.output.exists())

    def test_changed_sdk_dependency_fails_before_output(self):
        (self.sdk / "lib/libfixture.so.1").write_bytes(b"changed SDK library\n")
        with self.assertRaisesRegex(RuntimeError, "SDK dependency differs"):
            self.run_prepare()
        self.assertFalse(self.output.exists())

    def test_changed_deployed_dependency_fails_before_output(self):
        (self.application / "lib/droning/libfixture.so.1").write_bytes(b"changed deployed library\n")
        with self.assertRaisesRegex(RuntimeError, "Deployed dependency differs"):
            self.run_prepare()
        self.assertFalse(self.output.exists())

    def test_missing_notice_fails_before_output(self):
        self.replace_archive([("src/code.c", b"source\n")])
        with self.assertRaisesRegex(RuntimeError, "Missing archive notices"):
            self.run_prepare()
        self.assertFalse(self.output.exists())

    def test_unsafe_archive_path_fails_before_output(self):
        self.replace_archive([("../LICENSE", self.notice)])
        with self.assertRaisesRegex(RuntimeError, "Unsafe material path"):
            self.run_prepare()
        self.assertFalse(self.output.exists())

    def test_duplicate_archive_file_fails_before_output(self):
        self.replace_archive([("LICENSE", self.notice), ("LICENSE", b"replacement\n")])
        with self.assertRaisesRegex(RuntimeError, "Duplicate archive file"):
            self.run_prepare()
        self.assertFalse(self.output.exists())

    def test_symlink_to_external_deployed_library_is_rejected(self):
        external = self.root / "external.so"
        external.write_bytes(self.binary)
        path = self.application / "lib/droning/libfixture.so.1"
        path.unlink()
        path.symlink_to(external)
        with self.assertRaisesRegex(RuntimeError, "Deployed dependency differs"):
            self.run_prepare()
        self.assertFalse(self.output.exists())

    def test_existing_bundle_is_preserved(self):
        self.output.mkdir()
        sentinel = self.output / "previous-material"
        sentinel.write_bytes(b"preserve\n")
        with self.assertRaisesRegex(RuntimeError, "preserve previous materials"):
            self.run_prepare()
        self.assertEqual(sentinel.read_bytes(), b"preserve\n")


if __name__ == "__main__":
    unittest.main()
