#!/usr/bin/env python3
"""Check native GitHub Wiki links before publication."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("validate_wiki", ROOT / "docs/validate_wiki.py")
wiki = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wiki)


class WikiTests(unittest.TestCase):
    def test_current_pages_screenshots_and_application_links(self):
        pages = wiki.validate_pages(wiki.SOURCE)
        self.assertIn("OnlineHelp:-Attitude", pages)
        destinations = wiki.validate_application_links(ROOT / "ground/gcs", pages)
        self.assertGreaterEqual(len(destinations), 13)

    def test_missing_page_and_screenshot_are_rejected(self):
        for reference in ("[Missing](Unknown-topic)",
                          "![Missing](images/missing.png)",
                          "[Missing](OnlineHelp:-Missing)",
                          "[Missing](https://github.com/reubenfirmin/droniNG/wiki/Missing)"):
            with self.subTest(reference=reference), tempfile.TemporaryDirectory() as temporary:
                source = Path(temporary)
                (source / "Home.md").write_text("# Help\n\n" + reference)
                (source / "_Sidebar.md").write_text("[Home](Home)")
                with self.assertRaisesRegex(ValueError, "missing local link or image"):
                    wiki.validate_pages(source)

    def test_sidebar_links_are_validated(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / "Home.md").write_text("# Help")
            (source / "_Sidebar.md").write_text("[Missing](Missing)")
            with self.assertRaisesRegex(ValueError, "_Sidebar.md: missing local link"):
                wiki.validate_pages(source)

    def test_missing_and_obsolete_application_destinations_are_rejected(self):
        for target in (wiki.WIKI_URL + "/Missing", wiki.WIKI_URL + "/Home.html",
                       "https://github.com/d-ronin/dRonin/wiki/Home",
                       "https://reubenfirmin.github.io/droniNG/Home.html"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temporary:
                app = Path(temporary)
                (app / "help.qml").write_text('Qt.openUrlExternally("' + target + '")')
                with self.assertRaises(ValueError):
                    wiki.validate_application_links(app, {"Home"})

    def test_native_wiki_urls_with_encoded_colons_and_fragments(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary)
            (app / "help.cpp").write_text(wiki.WIKI_URL + "/OnlineHelp%3A-Attitude#filters")
            self.assertEqual(wiki.validate_application_links(app, {"OnlineHelp:-Attitude"}),
                             {"OnlineHelp:-Attitude"})


if __name__ == "__main__":
    unittest.main()
