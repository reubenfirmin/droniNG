#!/usr/bin/env python3
"""Check wiki publication, including legacy topic names and broken-link recovery."""

import importlib.util
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("build_wiki", ROOT / "docs/build_wiki.py")
wiki = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wiki)


class WikiSiteTests(unittest.TestCase):
    def test_all_current_pages_and_images_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "site"
            wiki.build(wiki.SOURCE, output)
            self.assertTrue((output / "index.html").is_file())
            attitude = (output / "OnlineHelp:-Attitude.html").read_text()
            self.assertIn('href="OnlineHelp%3A-Attitude.html" aria-current="page"', attitude)
            self.assertIn('src="images/attitude-filters.png"', attitude)
            self.assertTrue((output / "images/attitude-filters.png").is_file())
            self.assertNotIn('href="OnlineHelp:-', attitude)
            self.assertIn('<table>', (output / "Saving-changes.html").read_text())
            # Every shipped Help destination must exist in the published artifact.
            prefix = "https://reubenfirmin.github.io/droniNG/"
            destinations = set()
            for path in (ROOT / "ground/gcs").rglob("*"):
                if path.suffix not in {".cpp", ".h", ".ui", ".qml", ".ts"}:
                    continue
                # Legacy C++ comments include non-UTF-8 bytes; URLs remain ASCII.
                source = path.read_text(encoding="utf-8", errors="replace")
                self.assertNotRegex(source, r'https?://github\.com/(?:d-ronin/dRonin|reubenfirmin/droniNG)/wiki', str(path))
                for target in re.findall(re.escape(prefix) + r'([^\s"<>;&]*)', source):
                    destination = target or "index.html"
                    self.assertTrue((output / destination).is_file(), f"{path}: {destination}")
                    destinations.add(destination)
            self.assertGreaterEqual(len(destinations), 13)
            # Rebuilding removes obsolete pages and screenshots.
            (output / "obsolete.html").touch()
            wiki.build(wiki.SOURCE, output)
            self.assertFalse((output / "obsolete.html").exists())

    def test_missing_page_or_screenshot_preserves_previous_build(self):
        for missing in ("Unknown-topic", "images/missing.png"):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as temporary:
                source = Path(temporary) / "source"
                source.mkdir()
                (source / "Home.md").write_text("# Help\n\n[Missing](" + missing + ")")
                (source / "_Sidebar.md").write_text("[Home](Home)")
                output = Path(temporary) / "site"
                output.mkdir()
                (output / ".droning-wiki-build").touch()
                (output / "index.html").write_text("Previous successful build")
                with self.assertRaisesRegex(ValueError, "missing local link or image"):
                    wiki.build(source, output)
                self.assertEqual((output / "index.html").read_text(), "Previous successful build")

    def test_unrelated_output_directory_is_not_removed(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / "keep.txt").write_text("Keep this file")
            with self.assertRaisesRegex(ValueError, "not a wiki build"):
                wiki.build(wiki.SOURCE, output)
            self.assertEqual((output / "keep.txt").read_text(), "Keep this file")


if __name__ == "__main__":
    unittest.main()
