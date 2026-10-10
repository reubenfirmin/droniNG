#!/usr/bin/env python3
"""Check telemetry metadata hashes and qmake's failure handling."""

import importlib.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(os.environ.get("GCS_VERSION_INFO_SCRIPT", ROOT / "make/scripts/version-info.py"))
spec = importlib.util.spec_from_file_location("version_info", SCRIPT)
version_info = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version_info)


class VersionInfoTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="version-info-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.definitions = self.root / "definitions with spaces"
        self.definitions.mkdir()

    def run_cli(self, *arguments):
        return subprocess.run([sys.executable, "-B", str(SCRIPT), "--path", str(self.root),
                               *arguments], cwd=self.root, text=True, capture_output=True,
                              timeout=20)

    def test_utf8_and_universal_newlines_match_known_legacy_hash(self):
        path = self.definitions / "object.xml"
        for newline in (b"\n", b"\r\n", b"\r"):
            with self.subTest(newline=newline):
                path.write_bytes(b"<object>caf\xc3\xa9" + newline + b"</object>" + newline)
                self.assertEqual(version_info.GetHashofDirs(str(self.definitions), raw=1),
                                 "db38fe3368ce8ba68b393b2d01cab54e9786cd20")

    def test_long_definition_keeps_newline_normalization_across_chunks(self):
        path = self.definitions / "large.xml"
        path.write_bytes(b"a" * 4095 + b"\r\n" + b"b" * 4095 + b"\r")
        first = version_info.GetHashofDirs(str(self.definitions), raw=1)
        path.write_bytes(b"a" * 4095 + b"\n" + b"b" * 4095 + b"\n")
        self.assertEqual(version_info.GetHashofDirs(str(self.definitions), raw=1), first)

    def test_content_changes_affect_hash_and_other_files_do_not(self):
        path = self.definitions / "object.xml"
        path.write_bytes(b"alpha\nbeta\ngamma\n")
        first = version_info.GetHashofDirs(str(self.definitions), raw=1)
        self.assertEqual(first, "3606e66d1c3c8c823f84f89f1d4871a876ea7a0e")
        (self.definitions / "notes.txt").write_text("not an object definition")
        self.assertEqual(version_info.GetHashofDirs(str(self.definitions), raw=1), first)
        path.write_bytes(b"alpha\nbeta\ndelta\n")
        self.assertNotEqual(version_info.GetHashofDirs(str(self.definitions), raw=1), first)

    def test_directory_order_is_stable(self):
        for name in ("zdir", "adir"):
            (self.definitions / name).mkdir()
            (self.definitions / name / "object.xml").write_text(name + "\n")
        first = version_info.GetHashofDirs(str(self.definitions), raw=1)
        shutil.rmtree(self.definitions)
        self.definitions.mkdir()
        for name in ("adir", "zdir"):
            (self.definitions / name).mkdir()
            (self.definitions / name / "object.xml").write_text(name + "\n")
        self.assertEqual(version_info.GetHashofDirs(str(self.definitions), raw=1), first)

    def test_real_definitions_match_independent_sha1sum(self):
        definitions = ROOT / "shared/uavobjectdefinition"
        paths = sorted(definitions.rglob("*.xml"),
                       key=lambda path: (path.relative_to(definitions).parent.parts, path.name))
        self.assertTrue(paths)
        hashes = []
        for path in paths:
            contents = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
            result = subprocess.run(["sha1sum"], input=contents, capture_output=True, check=True)
            hashes.append(result.stdout.split()[0])
        result = subprocess.run(["sha1sum"], input=b"".join(hashes), capture_output=True, check=True)
        self.assertEqual(version_info.GetHashofDirs(str(definitions), raw=1),
                         result.stdout.split()[0].decode("ascii"))

    def test_cli_text_and_byte_hash_agree(self):
        (self.definitions / "object.xml").write_bytes(b"alpha\nbeta\ngamma\n")
        result = self.run_cli("--uavodir", str(self.definitions),
                              "--format", "${UAVOSHA1TXT}\n${UAVOSHA1}")
        self.assertEqual(result.returncode, 0, result.stderr)
        text, byte_values = result.stdout.splitlines()
        self.assertEqual(text, "3606e66d1c3c8c823f84f89f1d4871a876ea7a0e")
        self.assertEqual(bytes(int(value, 16) for value in byte_values.split(",")).hex(), text)

    def test_cli_without_definitions_preserves_optional_metadata(self):
        result = self.run_cli("--format", "${UAVOSHA1TXT}|${UAVOSHA1}|${TAG_OR_BRANCH}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "-1|-1|unreleased")

    def preserved_metadata(self):
        (self.definitions / "object.xml").write_bytes(b"alpha\nbeta\ngamma\n")
        metadata = {"schema": 1, "uavo_sha1": "3606e66d1c3c8c823f84f89f1d4871a876ea7a0e",
                    "repository": {"origin": "git@example.invalid:fixture/repo.git",
                                   "hash": "0123456789abcdef" * 2 + "01234567",
                                   "time": "1600000000", "tag": None, "branch": "archive-fixture",
                                   "dirty": True, "ancestor": "a" * 40,
                                   "authors": ["René Example", "Another Contributor"],
                                   "authors_since_release": ["René Example"]},
                    "generated_files_sha256": {}}
        path = self.root / "preserved metadata.json"
        path.write_text(json.dumps(metadata), encoding="utf-8")
        return path, metadata

    def test_archive_inputs_restore_revision_date_and_unicode_contributors_without_git(self):
        path, metadata = self.preserved_metadata()
        template = self.root / "template.h"
        template.write_text('${TAG_OR_BRANCH}:${HASH8}${DIRTY} ${DATE}\n'
                            '${ANCESTOR}\n${AUTHORS}\n${AUTHORS_SINCE_LAST_REL}\n')
        output = self.root / "generated.h"
        exported = self.root / "exported.json"
        result = self.run_cli("--uavodir", str(self.definitions), "--metadata", str(path),
                              "--write-metadata", str(exported), "--template", str(template),
                              "--outfile", str(output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_text(), "archive-fixture:01234567-dirty 20200913\n"
                         + "a" * 40 + "\nRené Example \nAnother Contributor\nRené Example\n")
        restored = json.loads(exported.read_text())
        self.assertEqual(restored["repository"], metadata["repository"])
        self.assertEqual(restored["uavo_sha1"], metadata["uavo_sha1"])
        self.assertEqual(restored["generated_files_sha256"], {
            "generated.h": hashlib.sha256(output.read_bytes()).hexdigest()})

    def test_changed_definitions_reject_preserved_inputs_before_replacing_outputs(self):
        path, _ = self.preserved_metadata()
        (self.definitions / "object.xml").write_bytes(b"changed definitions\n")
        template = self.root / "template.h"
        template.write_text("${HASH8}\n")
        output = self.root / "metadata.h"
        output.write_bytes(b"previous header\n")
        exported = self.root / "exported.json"
        exported.write_bytes(b"previous metadata\n")
        result = self.run_cli("--uavodir", str(self.definitions), "--metadata", str(path),
                              "--write-metadata", str(exported), "--template", str(template),
                              "--outfile", str(output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match UAVObject definitions", result.stderr)
        self.assertEqual(output.read_bytes(), b"previous header\n")
        self.assertEqual(exported.read_bytes(), b"previous metadata\n")

    def test_invalid_preserved_repository_fields_fail_before_output(self):
        path, metadata = self.preserved_metadata()
        for key, value in (("hash", "not-a-commit"), ("dirty", "false"),
                           ("time", "tomorrow"), ("authors", "not-a-list"),
                           ("branch", 'branch"with-quote'), ("ancestor", "a" * 39)):
            with self.subTest(key=key):
                changed = dict(metadata, repository=dict(metadata["repository"], **{key: value}))
                path.write_text(json.dumps(changed))
                result = self.run_cli("--uavodir", str(self.definitions), "--metadata", str(path),
                                      "--format", "${HASH8}")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")

    def test_unknown_metadata_schema_fails(self):
        path, metadata = self.preserved_metadata()
        metadata["schema"] = 2
        path.write_text(json.dumps(metadata))
        result = self.run_cli("--uavodir", str(self.definitions), "--metadata", str(path),
                              "--format", "${HASH8}")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported preserved version metadata schema", result.stderr)

    def test_invalid_definitions_do_not_replace_existing_metadata(self):
        template = self.root / "template.h"
        template.write_text("${UAVOSHA1TXT}\n")
        output = self.root / "metadata.h"
        for kind in ("missing", "empty", "unreadable", "invalid-utf8"):
            with self.subTest(kind=kind):
                definitions = self.root / kind
                if kind != "missing":
                    definitions.mkdir()
                if kind == "unreadable":
                    (definitions / "object.xml").symlink_to(definitions / "absent")
                if kind == "invalid-utf8":
                    (definitions / "object.xml").write_bytes(b"\xff")
                output.write_bytes(b"preserved previous metadata\n")
                result = self.run_cli("--uavodir", str(definitions), "--template", str(template),
                                      "--outfile", str(output))
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual(output.read_bytes(), b"preserved previous metadata\n")

    def test_qmake_stops_when_metadata_generation_fails(self):
        qmake6 = os.environ.get("GCS_VERSION_QMAKE6",
                                str(ROOT / "tools/Qt6.12.0/6.12.0/gcc_64/bin/qmake"))
        for index, qmake in enumerate(("/usr/bin/qmake-qt5", qmake6)):
            with self.subTest(qmake=qmake):
                project = self.root / str(index)
                source = project / "source"
                (source / "ground/gcs").mkdir(parents=True)
                scripts = source / "make/scripts"
                templates = source / "make/templates"
                scripts.mkdir(parents=True)
                templates.mkdir()
                shutil.copyfile(SCRIPT, scripts / "version-info.py")
                for name in ("gcsversioninfotemplate.h", "gcsauthorstemplate.html",
                             "gcsrelauthorstemplate.html"):
                    shutil.copyfile(ROOT / "make/templates" / name, templates / name)
                output = project / "output"
                output.mkdir()
                header = output / "gcsversioninfo.h"
                header.write_bytes(b"preserved previous metadata\n")
                pro = project / "metadata.pro"
                pro.write_text('TEMPLATE = aux\n'
                               'GCS_SOURCE_TREE = $$PWD/source/ground/gcs\n'
                               'GCS_BUILD_TREE = $$PWD/output\n'
                               'GCS_DATA_PATH = $$GCS_BUILD_TREE/share\n'
                               'defineReplace(targetPath) { return($$1) }\n'
                               'include(' + str(ROOT / 'ground/gcs/gcsversioninfo.pri') + ')\n')
                failed = subprocess.run([qmake, str(pro)], cwd=project, text=True,
                                        capture_output=True, timeout=20)
                self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
                self.assertIn("Could not generate GCS version metadata", failed.stderr)
                self.assertEqual(header.read_bytes(), b"preserved previous metadata\n")
                definitions = source / "shared/uavobjectdefinition"
                definitions.mkdir(parents=True)
                (definitions / "object.xml").write_bytes(b"alpha\nbeta\ngamma\n")
                passed = subprocess.run([qmake, str(pro)], cwd=project, text=True,
                                        capture_output=True, timeout=20)
                self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
                self.assertIn('0x36,0x06,0xe6,0x6d', header.read_text())
                exported = output / "share/gcs-version-metadata.json"
                metadata = json.loads(exported.read_text())
                self.assertEqual(metadata["generated_files_sha256"]["gcsversioninfo.h"],
                                 hashlib.sha256(header.read_bytes()).hexdigest())
                preserved, metadata = self.preserved_metadata()
                shutil.copyfile(preserved, source / "make/gcs-version-metadata.json")
                restored = subprocess.run([qmake, str(pro)], cwd=project, text=True,
                                          capture_output=True, timeout=20)
                self.assertEqual(restored.returncode, 0, restored.stdout + restored.stderr)
                self.assertIn('"archive-fixture:01234567-dirty 20200913"', header.read_text())
                self.assertEqual(json.loads(exported.read_text())["repository"], metadata["repository"])
                self.assertIn("René Example", (output / "share/gcsauthors.html").read_text())
                (definitions / "object.xml").write_bytes(b"changed definitions\n")
                before = header.read_bytes(), exported.read_bytes()
                stale = subprocess.run([qmake, str(pro)], cwd=project, text=True,
                                       capture_output=True, timeout=20)
                self.assertNotEqual(stale.returncode, 0)
                self.assertEqual((header.read_bytes(), exported.read_bytes()), before)
                (definitions / "object.xml").write_bytes(b"alpha\nbeta\ngamma\n")
                # The presence of a checkout prevents an archive input overriding Git.
                (source / ".git").mkdir()
                checkout = subprocess.run([qmake, str(pro)], cwd=project, text=True,
                                          capture_output=True, timeout=20)
                self.assertEqual(checkout.returncode, 0, checkout.stdout + checkout.stderr)
                self.assertNotIn("archive-fixture", header.read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
