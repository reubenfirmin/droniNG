# Publishing the droniNG help wiki

The help wiki is maintained in `docs/wiki/` and published to
<https://github.com/reubenfirmin/droniNG/wiki> by
[Publish wiki](../.github/workflows/publish-wiki.yml). The app's Help buttons,
setup wizard, Help menu and Home documentation links target this GitHub Wiki.

## Publishing

The workflow validates the wiki on pull requests. Changes to the wiki,
its validator, application Help links or publishing workflow on **main** publish automatically.
It also supports **Actions → Publish wiki → Run workflow** on main.

The publisher clones `reubenfirmin/droniNG.wiki.git`, synchronizes the Markdown
and screenshots, and pushes only when the content changes. It authenticates
through `gh auth git-credential` with the built-in Actions token and
`contents: write`. It does not use GitHub Pages or an HTML build.

For a new repository, enable **Settings → General → Features → Wikis** and save
one page named **Home** in the Wiki tab. This initializes the separate wiki Git
repository; `gh` has no API for creating that first page. Once initialized, all
subsequent updates are automatic. Check the repository with:

```sh
git -c credential.helper= -c 'credential.helper=!gh auth git-credential' \
  ls-remote https://github.com/reubenfirmin/droniNG.wiki.git
```

The publishing job reports the wiki URL in its Actions summary. A validation
failure leaves the published wiki unchanged. Pull requests cannot publish, and
the publisher does not force-push. If a concurrent wiki edit causes a push to
fail, review that edit before rerunning publication.

`docs/wiki/` is the source of truth. Make lasting edits there: direct edits in
the Wiki tab are replaced by the next publication. The sync removes obsolete
pages and images while preserving the wiki repository's Git history.

## Editing and previewing

Edit the Markdown pages directly in `docs/wiki/`; keep topic filenames stable
because application Help buttons link to them. Link to other topics by filename
without the `.md` suffix, and keep screenshots under `docs/wiki/images/`.
`Home.md` is the wiki landing page and `_Sidebar.md` defines navigation.

```sh
python3 -m pip install -r docs/wiki-requirements.txt
python3 ground/tests/wiki_site.py
python3 docs/validate_wiki.py
```

Preview Markdown in your editor and inspect the published Wiki after merging.
Validation checks all local page and image references, including the sidebar,
and verifies that every application Help destination has a corresponding guide.

## Screenshots and source material

The guides use 1920×1080 full-screen captures of the updated application, plus
native configuration previews. Packaged application captures come from
`ground/tests/run_ui.py --layout-audit`; native configuration
captures cover Hardware pages and module tabs requiring controller capabilities.
Those examples are identified as configuration previews rather than a connected,
flight-ready aircraft. Replace screenshots when the corresponding screen changes.

The full-screen capture set used for this update is
`build/qt612-release/ui-checks/ui-check-4c_ofpaw/`, with capability previews in
`build/qt612-release/ui-checks/page-layout-20261015/fullscreen/`.

Adapted guides credit their original dRonin sources. Those attribution links
remain in the documentation; application Help buttons use the droniNG Wiki.
