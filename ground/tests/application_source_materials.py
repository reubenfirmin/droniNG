#!/usr/bin/env python3
"""Check source archive boundaries, exact contents and runtime associations."""

import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "package"))
from application_source_materials import (digest, verify_materials, verify_snapshot, write_snapshot,
                                          version_metadata, VERSION_METADATA_SOURCE)


class ApplicationSourceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="application-source-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repository = self.root / "repository with spaces"
        self.repository.mkdir()
        (self.repository / "source.cpp").write_bytes(b"current edited source\n")
        (self.repository / "LICENSE.md").write_bytes(b"Original copyright and permission notice\n")
        (self.repository / "run.py").write_bytes(b"#!/usr/bin/env python3\n")
        (self.repository / "run.py").chmod(0o755)
        (self.repository / "alias.cpp").symlink_to("source.cpp")
        self.archive = self.root / "droning-source.tar.xz"
        self.names = ["LICENSE.md", "alias.cpp", "deleted.cpp", "run.py", "source.cpp"]

    def snapshot(self):
        return write_snapshot(self.repository, self.names, self.archive)

    def replace_archive(self, members):
        with tarfile.open(self.archive, "w:xz") as archive:
            for name, contents in members:
                member = tarfile.TarInfo(name)
                member.size = len(contents)
                member.mode = 0o644
                archive.addfile(member, io.BytesIO(contents))

    def test_exact_current_sources_notices_modes_links_and_deletions(self):
        records = self.snapshot()
        verify_snapshot(self.archive, records)
        with tarfile.open(self.archive) as archive:
            self.assertEqual(archive.extractfile("droning-source/source.cpp").read(),
                             b"current edited source\n")
            self.assertEqual(archive.extractfile("droning-source/LICENSE.md").read(),
                             (self.repository / "LICENSE.md").read_bytes())
            self.assertEqual(archive.getmember("droning-source/run.py").mode, 0o755)
            self.assertEqual(archive.getmember("droning-source/alias.cpp").linkname, "source.cpp")
            self.assertNotIn("droning-source/deleted.cpp", archive.getnames())
        self.assertEqual(records["deleted.cpp"], {"type": "deleted"})

    def test_snapshot_is_repeatable(self):
        self.snapshot()
        first = digest(self.archive)
        self.snapshot()
        self.assertEqual(first, digest(self.archive))

    def metadata_application(self):
        application = self.root / "application"
        (application / "share").mkdir(parents=True)
        outputs = {}
        for name in ("gcsauthors.html", "gcsrelauthors.html"):
            path = application / "share" / name
            path.write_bytes(b"Original contributors\n")
            outputs[name] = digest(path)
        outputs["gcsversioninfo.h"] = "a" * 64
        metadata = {"schema": 1, "repository": {"hash": "b" * 40},
                    "uavo_sha1": "c" * 40, "generated_files_sha256": outputs}
        path = application / "share/gcs-version-metadata.json"
        path.write_text(json.dumps(metadata))
        return application, path

    def test_generated_archive_input_preserves_exact_build_metadata(self):
        application, path = self.metadata_application()
        contents = version_metadata(application, "b" * 40)
        records = write_snapshot(self.repository, self.names, self.archive,
                                 generated_files={VERSION_METADATA_SOURCE: contents})
        verify_snapshot(self.archive, records)
        with tarfile.open(self.archive) as archive:
            self.assertEqual(archive.extractfile("droning-source/" + VERSION_METADATA_SOURCE).read(),
                             path.read_bytes())
        self.assertFalse((self.repository / VERSION_METADATA_SOURCE).exists())

    def test_generated_source_collision_and_escape_are_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "collides with repository"):
            write_snapshot(self.repository, self.names, self.archive,
                           generated_files={"source.cpp": b"replacement"})
        with self.assertRaisesRegex(RuntimeError, "Unsafe application source path"):
            write_snapshot(self.repository, self.names, self.archive,
                           generated_files={"../private.json": b"replacement"})

    def test_contributor_metadata_changed_after_build_is_rejected(self):
        application, _ = self.metadata_application()
        (application / "share/gcsauthors.html").write_bytes(b"Changed contributors\n")
        with self.assertRaisesRegex(RuntimeError, "contributors differ"):
            version_metadata(application, "b" * 40)

    def test_different_build_revision_and_incomplete_output_hashes_are_rejected(self):
        application, path = self.metadata_application()
        with self.assertRaisesRegex(RuntimeError, "different repository revision"):
            version_metadata(application, "d" * 40)
        metadata = json.loads(path.read_text())
        del metadata["generated_files_sha256"]["gcsversioninfo.h"]
        path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(RuntimeError, "incomplete generated output hashes"):
            version_metadata(application)

    def test_version_metadata_symlink_is_rejected(self):
        application, path = self.metadata_application()
        path.unlink()
        path.symlink_to(self.repository / "source.cpp")
        with self.assertRaisesRegex(RuntimeError, "escapes the application"):
            version_metadata(application)

    def test_archived_version_inputs_must_match_the_packaged_build(self):
        application, path = self.metadata_application()
        records = write_snapshot(self.repository, self.names, self.archive,
                                 generated_files={VERSION_METADATA_SOURCE: path.read_bytes()})
        (application / "qt-runtime.json").write_text(json.dumps({"linux_abi": {"elf_files": []}}))
        report = {"archive_sha256": digest(self.archive), "source_files": records,
                  "runtime_elf_sha256": {}, "repository_head": "b" * 40,
                  "generated_source_files": [VERSION_METADATA_SOURCE]}
        (self.root / "application-source-materials.json").write_text(json.dumps(report))
        self.assertEqual(verify_materials(self.root, application), report)
        metadata = json.loads(path.read_text())
        metadata["repository"]["branch"] = "changed-after-build"
        path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(RuntimeError, "Archived GCS version inputs differ"):
            verify_materials(self.root, application)

    def test_escape_source_path_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Unsafe application source path"):
            write_snapshot(self.repository, ["../outside.cpp"], self.archive)

    def test_git_metadata_path_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Unsafe application source path"):
            write_snapshot(self.repository, [".git/config"], self.archive)

    def test_external_source_symlink_is_rejected(self):
        (self.repository / "alias.cpp").unlink()
        (self.repository / "alias.cpp").symlink_to("../private.cpp")
        with self.assertRaisesRegex(RuntimeError, "symlink escapes archive"):
            self.snapshot()

    def test_external_parent_directory_is_rejected(self):
        outside = self.root / "private"
        outside.mkdir()
        (outside / "source.cpp").write_bytes(b"private\n")
        (self.repository / "dir").symlink_to(outside)
        with self.assertRaisesRegex(RuntimeError, "escapes repository"):
            write_snapshot(self.repository, ["dir/source.cpp"], self.archive)

    def test_duplicate_source_names_are_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Duplicate application source path"):
            write_snapshot(self.repository, ["source.cpp", "source.cpp"], self.archive)

    def test_unsafe_archive_member_is_rejected(self):
        records = self.snapshot()
        self.replace_archive([("droning-source/../private.cpp", b"private\n")])
        with self.assertRaisesRegex(RuntimeError, "Unsafe application source path"):
            verify_snapshot(self.archive, records)

    def test_modified_archive_source_is_rejected(self):
        records = self.snapshot()
        self.replace_archive([("droning-source/source.cpp", b"replacement source\n")])
        with self.assertRaisesRegex(RuntimeError, "source archive differs"):
            verify_snapshot(self.archive, records)

    def test_missing_archive_sources_are_rejected(self):
        records = self.snapshot()
        self.replace_archive([("droning-source/source.cpp", b"current edited source\n")])
        with self.assertRaisesRegex(RuntimeError, "file set differs"):
            verify_snapshot(self.archive, records)

    def test_archive_hardlink_is_rejected(self):
        records = self.snapshot()
        with tarfile.open(self.archive, "w:xz") as archive:
            member = tarfile.TarInfo("droning-source/source.cpp")
            member.type = tarfile.LNKTYPE
            member.linkname = "droning-source/LICENSE.md"
            archive.addfile(member)
        with self.assertRaisesRegex(RuntimeError, "source archive differs"):
            verify_snapshot(self.archive, records)

    def test_changed_runtime_and_unproven_build_correspondence(self):
        records = self.snapshot()
        application = self.root / "application"
        application.mkdir()
        binary = application / "app.bin"
        binary.write_bytes(b"recorded runtime\n")
        runtime_hash = digest(binary)
        (application / "qt-runtime.json").write_text(json.dumps({"linux_abi": {
            "elf_files": [{"path": "app.bin", "sha256": runtime_hash}]}}))
        report = {"archive_sha256": digest(self.archive), "source_files": records,
                  "runtime_elf_sha256": {"app.bin": runtime_hash},
                  "build_correspondence": "unverified"}
        (self.root / "application-source-materials.json").write_text(json.dumps(report))
        self.assertEqual(verify_materials(self.root, application), report)
        binary.write_bytes(b"different runtime\n")
        with self.assertRaisesRegex(RuntimeError, "Runtime differs"):
            verify_materials(self.root, application)


if __name__ == "__main__":
    unittest.main()
