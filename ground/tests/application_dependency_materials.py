#!/usr/bin/env python3
"""Exercise offline preservation and restoration using the cached pinned LSS sources."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "package"))
from application_dependency_materials import commit_sources, prepare, restore, verify


@unittest.skipUnless((ROOT / "downloads/breakpad-linux/src/third_party/lss/.git").exists(),
                     "Pinned linux-syscall-support source cache is required")
class ApplicationDependencyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="application-dependency-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / "materials with spaces"
        catalog = json.loads((ROOT / "package/application_dependency_sources.json").read_text())
        self.catalog = {"components": {"linux-syscall-support":
                                       catalog["components"]["linux-syscall-support"]}}
        self.report = prepare(ROOT, self.output, self.catalog)
        self.component = self.report["components"]["linux-syscall-support"]
        self.destination = self.root / "clean source tree"
        self.destination.mkdir()

    def test_sources_notices_and_existing_commit_restore_without_original_checkout(self):
        self.assertEqual(verify(self.output), self.report)
        restore(self.output, self.destination)
        checkout = self.destination / self.component["checkout"]
        self.assertEqual(commit_sources(checkout, self.component["revision"]),
                         self.component["source_files"])
        self.assertEqual((checkout / "LICENSE").read_bytes(),
                         (self.output / "notices/linux-syscall-support/LICENSE").read_bytes())
        remotes = subprocess.check_output(["git", "-C", str(checkout), "remote"], text=True)
        self.assertEqual(remotes, "")

    def test_existing_materials_are_preserved(self):
        before = (self.output / "application-dependency-materials.json").read_bytes()
        with self.assertRaisesRegex(RuntimeError, "fresh application dependency output"):
            prepare(ROOT, self.output, self.catalog)
        self.assertEqual((self.output / "application-dependency-materials.json").read_bytes(), before)

    def test_existing_checkout_is_preserved(self):
        checkout = self.destination / self.component["checkout"]
        checkout.mkdir(parents=True)
        (checkout / "keep").write_bytes(b"keep existing checkout\n")
        with self.assertRaisesRegex(RuntimeError, "Refusing to replace"):
            restore(self.output, self.destination)
        self.assertEqual((checkout / "keep").read_bytes(), b"keep existing checkout\n")

    def test_archive_tampering_is_rejected_before_restore(self):
        (self.output / self.component["archive"]).write_bytes(b"changed archive\n")
        with self.assertRaisesRegex(RuntimeError, "material checksum differs"):
            restore(self.output, self.destination)
        self.assertFalse((self.destination / self.component["checkout"]).exists())

    def test_object_pack_tampering_is_rejected_before_restore(self):
        (self.output / self.component["object_pack"]).write_bytes(b"changed object pack\n")
        with self.assertRaisesRegex(RuntimeError, "material checksum differs"):
            restore(self.output, self.destination)
        self.assertFalse((self.destination / self.component["checkout"]).exists())

    def test_notice_tampering_is_rejected(self):
        (self.output / "notices/linux-syscall-support/LICENSE").write_bytes(b"changed notice\n")
        with self.assertRaisesRegex(RuntimeError, "material checksum differs"):
            verify(self.output)

    def test_extra_material_is_rejected(self):
        (self.output / "unrecorded").write_bytes(b"unrecorded\n")
        with self.assertRaisesRegex(RuntimeError, "file set differs"):
            verify(self.output)

    def test_source_corruption_after_restore_is_rejected_by_commit_identity(self):
        restore(self.output, self.destination)
        checkout = self.destination / self.component["checkout"]
        (checkout / "LICENSE").write_bytes(b"changed source notice\n")
        with self.assertRaisesRegex(RuntimeError, "differs from pinned commit"):
            commit_sources(checkout, self.component["revision"])

    def test_wrong_pin_is_rejected_before_output(self):
        self.catalog["components"]["linux-syscall-support"]["revision"] = "0" * 40
        output = self.root / "wrong pin"
        with self.assertRaisesRegex(RuntimeError, "differs from build pin"):
            prepare(ROOT, output, self.catalog)
        self.assertFalse(output.exists())

    def test_breakpad_gitlink_references_are_retained(self):
        catalog = json.loads((ROOT / "package/application_dependency_sources.json").read_text())
        component = catalog["components"]["breakpad"]
        records = commit_sources(ROOT / component["checkout"], component["revision"])
        self.assertEqual({name: record["revision"] for name, record in records.items()
                          if record["type"] == "gitlink"},
                         {name: link["revision"] for name, link in component["gitlinks"].items()})

    def test_external_restore_parent_is_rejected(self):
        outside = self.root / "outside"
        outside.mkdir()
        (self.destination / "downloads").symlink_to(outside)
        with self.assertRaisesRegex(RuntimeError, "restore path escapes"):
            restore(self.output, self.destination)
        self.assertEqual(list(outside.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
