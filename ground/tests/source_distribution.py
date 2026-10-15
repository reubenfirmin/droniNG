#!/usr/bin/env python3
"""Exercise source inclusion and final-package verification with real files."""

import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "package"))
from source_distribution import include_sources, verify_sources


def sha256(contents):
    return hashlib.sha256(contents).hexdigest()


class SourceDistributionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="source-distribution-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.application = self.root / "package with spaces"
        self.sdk = self.root / "SDK with spaces"
        self.archives = self.root / "source archives"
        self.dependency_cache = self.root / "dependency cache"
        for path in (self.application / "lib/droning", self.sdk / "lib", self.sdk / "sbom",
                     self.archives, self.dependency_cache):
            path.mkdir(parents=True)
        self.binaries = {"libQt6Core.so.6": b"Qt SDK fixture\n",
                         "libfixture.so.1": b"Dependency SDK fixture\n"}
        for name, contents in self.binaries.items():
            (self.sdk / "lib" / name).write_bytes(contents)
            (self.application / "lib/droning" / name).write_bytes(contents)
        self.manifest_path = self.application / "qt-runtime.json"
        self.manifest = {"qt_version": "6.12.0", "linux_abi": {"fixture": "preserve"}}
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.source = b"int fixture(void) { return 0; }\n"
        self.notice = b"Fixture copyright and permission notice\n"
        qt_archive = self.archives / "qtbase-everywhere-src-6.12.0.tar.xz"
        self.archive(qt_archive, [("src/fixture.cpp", self.source), ("LICENSES/fixture.txt", self.notice)])
        self.qt_catalog = {"qt_version": "6.12.0", "source_url_prefix": "https://example.invalid/",
                           "modules": {"qtbase": sha256(qt_archive.read_bytes())}}
        binary_inventory = {"files": [{"fileName": "./lib/libQt6Core.so.6",
                                      "checksums": [{"algorithm": "SHA1", "checksumValue":
                                                     hashlib.sha1(self.binaries['libQt6Core.so.6']).hexdigest()}],
                                      "licenseConcluded": "GPL-3.0-only"}]}
        (self.sdk / "sbom/qtbase-6.12.0.spdx.json").write_text(json.dumps(binary_inventory))
        (self.sdk / "sbom/qtbase-6.12.0.source.spdx").write_text(
            "FileName: ./src/fixture.cpp\nSPDXID: SPDXRef-fixture\nFileChecksum: SHA1: "
            + hashlib.sha1(self.source).hexdigest() + "\n")
        dependency_archive = self.dependency_cache / "fixture.tar.gz"
        self.archive(dependency_archive, [("src/code.c", self.source), ("LICENSE", self.notice)])
        self.dependency_catalog = {
            "qt_version": "6.12.0",
            "materials": {dependency_archive.name: {"sha256": sha256(dependency_archive.read_bytes())}},
            "components": {"fixture": {
                "source_archive": dependency_archive.name, "notice_members": ["LICENSE"],
                "sdk_library_sha256": {"libfixture.so.1": sha256(self.binaries['libfixture.so.1'])}}},
            "remaining_source_gates": ["Fixture's full build recipe remains unverified."]}

    def archive(self, path, records):
        with tarfile.open(path, "w:xz" if path.suffix == ".xz" else "w:gz") as archive:
            for name, contents in records:
                member = tarfile.TarInfo("source-root/" + name)
                member.size = len(contents)
                archive.addfile(member, io.BytesIO(contents))

    def include(self):
        return include_sources(self.application, self.sdk, self.archives, self.dependency_cache,
                               self.qt_catalog, self.dependency_catalog)

    def test_inclusion_preserves_runtime_abi_notices_and_open_gates(self):
        report = self.include()
        verified = verify_sources(self.application)
        self.assertEqual(verified, report)
        manifest = json.loads(self.manifest_path.read_text())
        self.assertEqual(manifest["linux_abi"], self.manifest["linux_abi"])
        self.assertFalse(report["source_distribution_complete"])
        self.assertIn(self.dependency_catalog["remaining_source_gates"][0], report["remaining_source_gates"])
        for name, contents in self.binaries.items():
            self.assertEqual((self.application / "lib/droning" / name).read_bytes(), contents)
        for path in ("source-materials/qt/licenses/qtbase/LICENSES/fixture.txt",
                     "source-materials/sdk-dependencies/notices/fixture/LICENSE"):
            self.assertEqual((self.application / path).read_bytes(), self.notice)

    def test_version_mismatch_fails_before_inclusion(self):
        self.dependency_catalog["qt_version"] = "5.15.18"
        with self.assertRaisesRegex(RuntimeError, "Both source catalogues"):
            self.include()
        self.assertEqual(json.loads(self.manifest_path.read_text()), self.manifest)
        self.assertFalse((self.application / "source-materials").exists())

    def test_build_tool_inventory_is_preserved_and_checked_in_manifest(self):
        contents = b"Qt build tool fixture\n"
        (self.sdk / "bin").mkdir()
        (self.sdk / "bin/qmake").write_bytes(contents)
        inventory_path = self.sdk / "sbom/qtbase-6.12.0.spdx.json"
        inventory = json.loads(inventory_path.read_text())
        inventory["files"].append({"fileName": "./bin/qmake", "checksums": [
            {"algorithm": "SHA1", "checksumValue": hashlib.sha1(contents).hexdigest()}],
            "licenseConcluded": "GPL-3.0-only WITH Qt-GPL-exception-1.0"})
        inventory_path.write_text(json.dumps(inventory))
        self.qt_catalog["build_tools"] = {"bin/qmake": "qtbase"}
        report = self.include()
        self.assertEqual(verify_sources(self.application), report)
        self.assertEqual(report["qt_build_tools"][0]["sha256"], sha256(contents))
        manifest = json.loads(self.manifest_path.read_text())
        manifest["source_materials"]["qt_build_tools"][0]["sha256"] = "0" * 64
        self.manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(RuntimeError, "module/build-tool inventory differs"):
            verify_sources(self.application)

    def test_modified_source_archive_fails_before_inclusion(self):
        (self.archives / "qtbase-everywhere-src-6.12.0.tar.xz").write_bytes(b"modified archive\n")
        with self.assertRaisesRegex(RuntimeError, "checksum differs"):
            self.include()
        self.assertEqual(json.loads(self.manifest_path.read_text()), self.manifest)
        self.assertFalse((self.application / "source-materials").exists())

    def test_dependency_failure_does_not_publish_a_source_manifest(self):
        (self.dependency_cache / "fixture.tar.gz").write_bytes(b"modified dependency source\n")
        with self.assertRaisesRegex(RuntimeError, "checksum or cache path differs"):
            self.include()
        self.assertEqual(json.loads(self.manifest_path.read_text()), self.manifest)
        self.assertFalse((self.application / "THIRD_PARTY_NOTICES.md").exists())

    def test_existing_source_bundle_is_preserved(self):
        self.include()
        before = self.manifest_path.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "fresh package stage"):
            self.include()
        self.assertEqual(self.manifest_path.read_bytes(), before)

    def test_deployment_rejects_existing_sources_before_querying_qmake(self):
        self.include()
        before = self.manifest_path.read_bytes()
        result = subprocess.run(
            [sys.executable, "-B", str(Path(__file__).resolve().parents[2]
                                      / "package/deploy_linux.py"),
             "--qmake", "/unavailable-qmake", "--application", str(self.application)],
            text=True, capture_output=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("fresh package stage", result.stderr)
        self.assertEqual(self.manifest_path.read_bytes(), before)
        for name, contents in self.binaries.items():
            self.assertEqual((self.application / "lib/droning" / name).read_bytes(), contents)

    def test_deployment_rejects_unpaired_source_options_before_changes(self):
        before = self.manifest_path.read_bytes()
        result = subprocess.run(
            [sys.executable, "-B", str(Path(__file__).resolve().parents[2]
                                      / "package/deploy_linux.py"),
             "--qmake", "/unavailable-qmake", "--application", str(self.application),
             "--qt-source-archives", str(self.archives)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Supply both", result.stderr)
        self.assertEqual(self.manifest_path.read_bytes(), before)
        self.assertFalse((self.application / "source-materials").exists())

    def test_application_source_option_requires_sdk_sources_before_changes(self):
        before = self.manifest_path.read_bytes()
        result = subprocess.run(
            [sys.executable, "-B", str(Path(__file__).resolve().parents[2]
                                      / "package/deploy_linux.py"),
             "--qmake", "/unavailable-qmake", "--application", str(self.application),
             "--application-source-root", str(self.root)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("requires the SDK source material options", result.stderr)
        self.assertEqual(self.manifest_path.read_bytes(), before)
        self.assertFalse((self.application / "source-materials").exists())

    def test_changed_notice_is_rejected_after_inclusion(self):
        self.include()
        path = self.application / "source-materials/qt/licenses/qtbase/LICENSES/fixture.txt"
        path.write_bytes(b"replacement notice\n")
        with self.assertRaisesRegex(RuntimeError, "Source material checksum differs"):
            verify_sources(self.application)

    def test_missing_material_is_rejected_after_inclusion(self):
        self.include()
        (self.application / "source-materials/sdk-dependencies/materials/fixture.tar.gz").unlink()
        with self.assertRaisesRegex(RuntimeError, "file set differs"):
            verify_sources(self.application)

    def test_added_material_is_rejected_after_inclusion(self):
        self.include()
        (self.application / "source-materials/unrecorded-file").write_bytes(b"unrecorded\n")
        with self.assertRaisesRegex(RuntimeError, "file set differs"):
            verify_sources(self.application)

    def test_changed_deployed_sdk_file_is_rejected_after_inclusion(self):
        self.include()
        (self.application / "lib/droning/libfixture.so.1").write_bytes(b"replacement library\n")
        with self.assertRaisesRegex(RuntimeError, "Deployed file differs"):
            verify_sources(self.application)

    def test_added_qt_runtime_asset_is_rejected_after_inclusion(self):
        self.include()
        directory = self.application / "lib/droning/qml/QtQuick"
        directory.mkdir(parents=True)
        (directory / "unrecorded.qml").write_bytes(b"Item {}\n")
        with self.assertRaisesRegex(RuntimeError, "Deployed Qt file set differs"):
            verify_sources(self.application)

    def test_external_material_symlink_is_rejected(self):
        self.include()
        path = self.application / "source-materials/sdk-dependencies/materials/fixture.tar.gz"
        external = self.root / "external.tar.gz"
        external.write_bytes(path.read_bytes())
        path.unlink()
        path.symlink_to(external)
        with self.assertRaisesRegex(RuntimeError, "escapes the package"):
            verify_sources(self.application)


if __name__ == "__main__":
    sys.exit(unittest.main())
