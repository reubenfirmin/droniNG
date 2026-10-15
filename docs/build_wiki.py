#!/usr/bin/env python3
"""Build the help wiki for GitHub Pages and validate every local link."""

import argparse
from html import escape
from html.parser import HTMLParser
from pathlib import Path
import shutil
from urllib.parse import quote, unquote, urlsplit

import markdown


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs/wiki"


def page_name(stem):
    return "index.html" if stem == "Home" else stem + ".html"


class Links(HTMLParser):
    def __init__(self, source, pages, current):
        super().__init__(convert_charrefs=False)
        self.source, self.pages, self.current = source, pages, current
        self.parts = []

    def target(self, value):
        path, separator, fragment = value.partition("#")
        decoded = unquote(path)
        stem = decoded.removesuffix(".md")
        if stem in self.pages:
            return quote(page_name(stem)) + separator + fragment
        if not path or urlsplit(value).scheme in {"http", "https", "mailto"}:
            return value
        asset = (self.source / decoded).resolve()
        if not asset.is_relative_to(self.source.resolve()) or not asset.is_file():
            raise ValueError(f"{self.current}: missing local link or image: {value}")
        return quote(decoded, safe="/") + separator + fragment

    def start(self, tag, attrs, closing):
        attrs = dict(attrs)
        if tag == "a" and attrs.get("href"):
            attrs["href"] = self.target(attrs["href"])
            if attrs["href"] == quote(page_name(self.current)):
                attrs["aria-current"] = "page"
        if tag == "img" and attrs.get("src"):
            attrs["src"] = self.target(attrs["src"])
            attrs["loading"] = "lazy"
        text = "".join(" " + key if value is None else f' {key}="{escape(value, quote=True)}"'
                       for key, value in attrs.items())
        self.parts.append(f"<{tag}{text}{closing}>")

    def handle_starttag(self, tag, attrs):
        self.start(tag, attrs, "")

    def handle_startendtag(self, tag, attrs):
        self.start(tag, attrs, " /")

    def handle_endtag(self, tag):
        self.parts.append(f"</{tag}>")

    def handle_data(self, data):
        self.parts.append(data)

    def handle_entityref(self, name):
        self.parts.append(f"&{name};")

    def handle_charref(self, name):
        self.parts.append(f"&#{name};")


def render(text, source, pages, current):
    parser = Links(source, pages, current)
    parser.feed(markdown.markdown(text, extensions=["tables", "fenced_code", "toc"]))
    parser.close()
    return "".join(parser.parts)


def build(source, output):
    pages = {path.stem: path for path in sorted(source.glob("*.md"))
             if not path.name.startswith("_")}
    if "Home" not in pages or not (source / "_Sidebar.md").is_file():
        raise ValueError("The wiki requires Home.md and _Sidebar.md")
    # Validate everything before replacing the previous build.
    rendered = {}
    for stem, path in pages.items():
        text = path.read_text(encoding="utf-8")
        title = next((line[2:] for line in text.splitlines() if line.startswith("# ")), stem)
        body = render(text, source, pages, stem)
        sidebar = render((source / "_Sidebar.md").read_text(encoding="utf-8"), source, pages, stem)
        edit = "https://github.com/reubenfirmin/droniNG/blob/main/docs/wiki/" + quote(path.name)
        rendered[page_name(stem)] = f'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark light">
<title>{escape(title)} · droniNG</title>
<link rel="stylesheet" href="wiki.css">
</head>
<body>
<a class="skip" href="#content">Skip to content</a>
<header><a href="index.html">droniNG <span>Help</span></a><a href="https://github.com/reubenfirmin/droniNG">GitHub ↗</a></header>
<div class="page">
<nav aria-label="Help topics">{sidebar}</nav>
<main id="content">{body}<footer><a href="{escape(edit, quote=True)}">View page source on GitHub</a></footer></main>
</div>
</body>
</html>
'''
    resolved = output.resolve()
    if resolved == source.resolve() or resolved in source.resolve().parents or resolved.is_relative_to(source.resolve()):
        raise ValueError("The output directory must be separate from the Markdown source")
    marker = output / ".droning-wiki-build"
    if output.exists() and any(output.iterdir()) and not marker.is_file():
        raise ValueError(f"Refusing to replace a directory that is not a wiki build: {output}")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    marker.touch()
    (output / ".nojekyll").touch()
    for name, html in rendered.items():
        (output / name).write_text(html, encoding="utf-8")
    # Keep a direct Home.html URL available alongside the site's index.
    (output / "Home.html").write_text(rendered["index.html"], encoding="utf-8")
    shutil.copyfile(ROOT / "docs/wiki-site.css", output / "wiki.css")
    if (source / "images").exists():
        shutil.copytree(source / "images", output / "images")
    print(f"Built {len(pages)} help pages and {len(list((output / 'images').glob('*')))} images in {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/wiki-site")
    args = parser.parse_args()
    try:
        build(SOURCE, args.output)
    except ValueError as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
