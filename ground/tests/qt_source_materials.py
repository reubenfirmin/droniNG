#!/usr/bin/env python3
"""Check source/archive and deployment evidence before preparing Qt materials."""

import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "package"))
from qt_source_materials import audit_archive, prepare


class QtSourceMaterialsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="qt-source-materials-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = b"int source_fixture(void) { return 0; }\n"
        self.licence = b"Fixture licence text and copyright notice\n"
        self.expected = {"src/fixture.cpp": hashlib.sha1(self.source).hexdigest()}
        self.archive = self.root / "qtbase-everywhere-src-6.12.0.tar.xz"
        self.make_archive()

    def make_archive(self, members=None):
        if members is None:
            members = [("src/fixture.cpp", self.source), ("LICENSES/fixture.txt", self.licence)]
        with tarfile.open(self.archive, "w:xz") as archive:
            for name, contents in members:
                info = tarfile.TarInfo("qtbase-source/" + name)
                info.size = len(contents)
                archive.addfile(info, io.BytesIO(contents))
        return hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def audit(self, expected=None):
        return audit_archive(self.archive, hashlib.sha256(self.archive.read_bytes()).hexdigest(),
                             self.expected if expected is None else expected)

    def test_matching_source_and_licence_contents(self):
        result, licences = self.audit()
        self.assertEqual(result["matching_source_files"], 1)
        self.assertTrue(result["source_inventory_available"])
        self.assertEqual(licences["LICENSES/fixture.txt"], self.licence)

    def test_modified_archive_fails_published_checksum(self):
        with self.assertRaisesRegex(RuntimeError, "checksum differs"):
            audit_archive(self.archive, "0" * 64, self.expected)

    def test_modified_source_is_rejected(self):
        self.make_archive([("src/fixture.cpp", b"changed implementation\n"),
                           ("LICENSES/fixture.txt", self.licence)])
        with self.assertRaisesRegex(RuntimeError, "differs from SDK source inventory"):
            self.audit()

    def test_missing_source_is_rejected(self):
        self.make_archive([("LICENSES/fixture.txt", self.licence)])
        with self.assertRaisesRegex(RuntimeError, "src/fixture.cpp"):
            self.audit()

    def test_git_metadata_exceptions_are_reported(self):
        self.make_archive([("src/fixture.cpp", self.source), (".tag", b"release revision\n"),
                           ("LICENSES/fixture.txt", self.licence)])
        expected = dict(self.expected, **{".tag": hashlib.sha1(b"SDK revision\n").hexdigest(),
                                         "src/.gitignore": hashlib.sha1(b"*.o\n").hexdigest()})
        result, _ = self.audit(expected)
        self.assertEqual(result["matching_source_files"], 1)
        self.assertEqual(list(result["different_metadata_files"]), [".tag"])
        self.assertEqual(result["omitted_git_metadata_files"], ["src/.gitignore"])

    def test_path_escape_is_rejected(self):
        self.make_archive([("../LICENSES/escaped.txt", self.licence)])
        with self.assertRaisesRegex(RuntimeError, "Unsafe archive member"):
            self.audit()

    def test_duplicate_archive_paths_are_rejected(self):
        self.make_archive([("LICENSES/fixture.txt", self.licence),
                           ("LICENSES/fixture.txt", b"replacement licence\n")])
        with self.assertRaisesRegex(RuntimeError, "Duplicate archive member"):
            self.audit()

    def test_absent_licence_texts_are_rejected(self):
        self.make_archive([("src/fixture.cpp", self.source)])
        with self.assertRaisesRegex(RuntimeError, "no LICENSES files"):
            self.audit()

    def fixture_deployment(self):
        application = self.root / "package with spaces"
        sdk = self.root / "SDK with spaces"
        (application / "lib/droning").mkdir(parents=True)
        (sdk / "lib").mkdir(parents=True)
        (sdk / "sbom").mkdir()
        contents = b"SDK library fixture\n"
        name = "libQt6Core.so.6"
        (sdk / "lib" / name).write_bytes(contents)
        (application / "lib/droning" / name).write_bytes(contents)
        (application / "qt-runtime.json").write_text(json.dumps({"qt_version": "6.12.0"}))
        binary = {"files": [{"fileName": "./lib/" + name,
                             "checksums": [{"algorithm": "SHA1",
                                            "checksumValue": hashlib.sha1(contents).hexdigest()}],
                             "licenseConcluded": "GPL-3.0-only"}]}
        (sdk / "sbom/qtbase-6.12.0.spdx.json").write_text(json.dumps(binary))
        (sdk / "sbom/qtbase-6.12.0.source.spdx").write_text(
            "FileName: ./src/fixture.cpp\nSPDXID: SPDXRef-fixture\nFileChecksum: SHA1: "
            + self.expected["src/fixture.cpp"] + "\n")
        catalog = {"qt_version": "6.12.0", "source_url_prefix": "https://example.invalid/",
                   "modules": {"qtbase": hashlib.sha256(self.archive.read_bytes()).hexdigest()}}
        return application, sdk, catalog

    def test_materials_preserve_licences_archives_and_inventory(self):
        application, sdk, catalog = self.fixture_deployment()
        output = self.root / "prepared materials"
        report = prepare(application, sdk, self.root, output, catalog)
        self.assertEqual(report["deployed_file_count"], 1)
        self.assertEqual(report["deployed_files"][0]["license_expression"], "GPL-3.0-only")
        self.assertEqual((output / "licenses/qtbase/LICENSES/fixture.txt").read_bytes(), self.licence)
        self.assertEqual((output / "archives" / self.archive.name).read_bytes(), self.archive.read_bytes())
        for name, wanted in report["material_files_sha256"].items():
            self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), wanted)
        with self.assertRaisesRegex(RuntimeError, "preserve previous materials"):
            prepare(application, sdk, self.root, output, catalog)

    def test_changed_sdk_binary_inventory_cannot_validate_deployment(self):
        application, sdk, catalog = self.fixture_deployment()
        path = sdk / "sbom/qtbase-6.12.0.spdx.json"
        data = json.loads(path.read_text())
        data["files"][0]["checksums"][0]["checksumValue"] = "0" * 40
        path.write_text(json.dumps(data))
        output = self.root / "invalid materials"
        with self.assertRaisesRegex(RuntimeError, "differs from SDK binary inventory"):
            prepare(application, sdk, self.root, output, catalog)
        self.assertFalse(output.exists())

    def test_changed_deployed_binary_is_rejected_before_output(self):
        application, sdk, catalog = self.fixture_deployment()
        (application / "lib/droning/libQt6Core.so.6").write_bytes(b"modified library\n")
        output = self.root / "invalid materials"
        with self.assertRaisesRegex(RuntimeError, "differs from selected SDK"):
            prepare(application, sdk, self.root, output, catalog)
        self.assertFalse(output.exists())

    def test_malformed_source_inventory_is_rejected_before_output(self):
        application, sdk, catalog = self.fixture_deployment()
        (sdk / "sbom/qtbase-6.12.0.source.spdx").write_text(
            "FileName: ./src/fixture.cpp\nSPDXID: SPDXRef-fixture\n")
        output = self.root / "invalid materials"
        with self.assertRaisesRegex(RuntimeError, "Incomplete or unsupported SDK source inventory"):
            prepare(application, sdk, self.root, output, catalog)
        self.assertFalse(output.exists())


    def fixture_build_tool(self):
        application, sdk, catalog = self.fixture_deployment()
        (sdk / "bin").mkdir()
        contents = b"SDK translation compiler fixture\n"
        tool = sdk / "bin/lrelease"
        tool.write_bytes(contents)
        binary = {"files": [{"fileName": "./bin/lrelease",
                             "checksums": [{"algorithm": "SHA1", "checksumValue":
                                            hashlib.sha1(contents).hexdigest()}],
                             "licenseConcluded": "GPL-3.0-only WITH Qt-GPL-exception-1.0"}]}
        (sdk / "sbom/qttools-6.12.0.spdx.json").write_text(json.dumps(binary))
        shutil.copyfile(sdk / "sbom/qtbase-6.12.0.source.spdx",
                        sdk / "sbom/qttools-6.12.0.source.spdx")
        archive = self.root / "qttools-everywhere-src-6.12.0.tar.xz"
        shutil.copyfile(self.archive, archive)
        catalog["modules"]["qttools"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        catalog["build_tools"] = {"bin/lrelease": "qttools"}
        return application, sdk, catalog, tool

    def test_build_only_module_preserves_sources_notices_and_tool_inventory(self):
        application, sdk, catalog, tool = self.fixture_build_tool()
        output = self.root / "build tool materials"
        report = prepare(application, sdk, self.root, output, catalog)
        self.assertEqual(set(report["modules"]), {"qtbase", "qttools"})
        self.assertEqual(report["deployed_file_count"], 1)
        self.assertEqual(report["build_tools"][0]["sha256"],
                         hashlib.sha256(tool.read_bytes()).hexdigest())
        self.assertEqual(report["build_tools"][0]["license_expression"],
                         "GPL-3.0-only WITH Qt-GPL-exception-1.0")
        self.assertTrue((output / "archives/qttools-everywhere-src-6.12.0.tar.xz").is_file())
        self.assertEqual((output / "licenses/qttools/LICENSES/fixture.txt").read_bytes(), self.licence)
        self.assertTrue((output / "sbom/qttools-6.12.0.spdx.json").is_file())
        self.assertTrue((output / "sbom/qttools-6.12.0.source.spdx").is_file())

    def test_modified_build_tool_fails_before_output(self):
        application, sdk, catalog, tool = self.fixture_build_tool()
        tool.write_bytes(b"different translation compiler\n")
        output = self.root / "invalid tool materials"
        with self.assertRaisesRegex(RuntimeError, "build tool differs from SDK binary inventory"):
            prepare(application, sdk, self.root, output, catalog)
        self.assertFalse(output.exists())

    def test_build_tool_requires_matching_module_inventory(self):
        application, sdk, catalog, _ = self.fixture_build_tool()
        catalog["build_tools"]["bin/lrelease"] = "qtbase"
        output = self.root / "invalid tool materials"
        with self.assertRaisesRegex(RuntimeError, "build tool differs from SDK binary inventory"):
            prepare(application, sdk, self.root, output, catalog)
        self.assertFalse(output.exists())

    def test_build_tool_path_and_symlink_cannot_escape_sdk(self):
        application, sdk, catalog, tool = self.fixture_build_tool()
        for name in ("../lrelease", "/bin/lrelease", "bin/./lrelease"):
            with self.subTest(name=name):
                catalog["build_tools"] = {name: "qttools"}
                output = self.root / "invalid tool materials"
                with self.assertRaisesRegex(RuntimeError, "Unsafe Qt build tool path"):
                    prepare(application, sdk, self.root, output, catalog)
                self.assertFalse(output.exists())
        external = self.root / "external tool"
        external.write_bytes(tool.read_bytes())
        tool.unlink()
        tool.symlink_to(external)
        catalog["build_tools"] = {"bin/lrelease": "qttools"}
        with self.assertRaisesRegex(RuntimeError, "escapes"):
            prepare(application, sdk, self.root, output, catalog)
        self.assertFalse(output.exists())

    def test_build_only_module_requires_source_archive(self):
        application, sdk, catalog, _ = self.fixture_build_tool()
        (self.root / "qttools-everywhere-src-6.12.0.tar.xz").unlink()
        output = self.root / "invalid tool materials"
        with self.assertRaises(FileNotFoundError):
            prepare(application, sdk, self.root, output, catalog)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
