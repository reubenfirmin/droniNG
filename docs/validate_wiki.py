#!/usr/bin/env python3
"""Validate GitHub Wiki pages, screenshots and application Help destinations."""

import argparse
from html.parser import HTMLParser
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit

import markdown


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs/wiki"
WIKI_URL = "https://github.com/reubenfirmin/droniNG/wiki"
APP_SUFFIXES = {".cpp", ".h", ".ui", ".qml", ".ts"}


class Links(HTMLParser):
    def __init__(self, source, pages, current):
        super().__init__()
        self.source, self.pages, self.current = source.resolve(), pages, current

    def check(self, value, image=False):
        if value.startswith(WIKI_URL + "/"):
            value = value[len(WIKI_URL) + 1:]
        decoded = unquote(value.partition("#")[0])
        # Legacy topic names contain a colon, which URL parsers treat as a scheme.
        if not image and decoded.removesuffix(".md") in self.pages:
            return
        if not decoded or urlsplit(value).scheme in {"http", "https", "mailto"}:
            return
        asset = (self.source / decoded).resolve()
        if not asset.is_relative_to(self.source) or not asset.is_file():
            raise ValueError(f"{self.current}: missing local link or image: {value}")

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "a" and attrs.get("href"):
            self.check(attrs["href"])
        if tag == "img" and attrs.get("src"):
            self.check(attrs["src"], image=True)


def validate_pages(source):
    pages = {path.stem for path in source.glob("*.md") if not path.name.startswith("_")}
    if "Home" not in pages or not (source / "_Sidebar.md").is_file():
        raise ValueError("The wiki requires Home.md and _Sidebar.md")
    for path in sorted(source.glob("*.md")):
        parser = Links(source, pages, path.name)
        parser.feed(markdown.markdown(path.read_text(encoding="utf-8"),
                                      extensions=["tables", "fenced_code"]))
        parser.close()
    return pages


def validate_application_links(app_source, pages):
    destinations = set()
    pattern = re.compile(re.escape(WIKI_URL) + r'(?=[/\s"<>;&)]|$)([^\s"<>;&)]*)')
    obsolete = re.compile(r'https?://github\.com/d-ronin/dRonin/wiki'
                          r'|https?://reubenfirmin\.github\.io/droniNG')
    for path in app_source.rglob("*"):
        if path.suffix not in APP_SUFFIXES:
            continue
        # Some legacy C++ comments include non-UTF-8 bytes; URLs remain ASCII.
        text = path.read_text(encoding="utf-8", errors="replace")
        if obsolete.search(text):
            raise ValueError(f"{path}: obsolete Help URL; use {WIKI_URL}")
        for target in pattern.findall(text):
            destination = unquote(target.partition("#")[0].lstrip("/")) or "Home"
            if destination not in pages:
                raise ValueError(f"{path}: missing wiki Help destination: {destination}")
            destinations.add(destination)
    return destinations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    try:
        pages = validate_pages(SOURCE)
        destinations = validate_application_links(ROOT / "ground/gcs", pages)
    except ValueError as error:
        parser.exit(1, str(error) + "\n")
    images = len(list((SOURCE / "images").glob("*")))
    print(f"Validated {len(pages)} wiki pages, {images} screenshots and "
          f"{len(destinations)} application Help destinations")


if __name__ == "__main__":
    main()
