# droniNG wiki migration

The Markdown in `docs/wiki/` is ready to become the droniNG GitHub wiki. It revises the GCS help topics fetched with:

```sh
gh repo clone https://github.com/d-ronin/dRonin.wiki.git /tmp/dronin-upstream-wiki-20261015 -- --depth=1
```

The revision replaces external legacy screenshots, documents the current Firmware workflow and configuration drafts, and replaces historical empty help stubs with usable instructions. Adapted pages credit their upstream sources. Screenshots are stored beside the pages in `docs/wiki/images/`; keep those relative image paths when publishing.

## Publication prerequisite

On 2026-10-15, authenticated `gh api repos/reubenfirmin/droniNG` reported `has_wiki: true` and repository administration/write access. Both authenticated `gh repo clone` and an SSH `git ls-remote` returned **Repository not found** for `droniNG.wiki.git`. Visiting `/wiki` redirected to the repository home. GitHub has not initialized the separate wiki repository.

Create the first page at [New wiki page](https://github.com/reubenfirmin/droniNG/wiki/_new) to initialize it. The wiki can then be cloned with `gh repo clone https://github.com/reubenfirmin/droniNG.wiki.git`. Publishing requires a commit and push to that separate repository; the workspace instructions require explicit authorization for each action.

The application now targets `https://github.com/reubenfirmin/droniNG/wiki/` from all existing help entry points:

- `ground/gcs/src/plugins/config/{airframe,attitude,camerastabilization,input,modules,output,stabilization,txpid}.ui`
- `ground/gcs/src/plugins/setupwizard/pages/outputpage.ui`
- `ground/gcs/src/plugins/uavsettingsimportexport/importsummary.cpp`
- `ground/gcs/src/plugins/importexport/importexportgadgetwidget.cpp`
- `ground/gcs/src/plugins/uploader/uploadergadgetwidget.cpp`
- Home's Documentation links in `ground/gcs/share/welcome/{main,SitesPanel}.qml`
- Matching source strings in all six translation catalogs

All referenced topic slugs have matching Markdown files in this migration. The setup wizard also names the droniNG wiki. **The destination pages are not live until the wiki is initialized and these files are published.** Verify the published pages and images and exercise the help buttons before considering publication complete.

## Screenshot sources

The full application is captured at 1920×1080 by `ground/tests/run_ui.py --layout-audit`. Native configuration captures also cover board-specific Hardware pages and module tabs that require controller capabilities. Those previews are identified as configuration examples rather than evidence of a flight-ready aircraft.

Verified on 2026-10-15:

- All 12 focused native checks passed, covering configuration layouts at compact and full-screen sizes, retained drafts, Discard, native Hardware pages, controller setup and Firmware layout.
- `ground/tests/run_ui.py --layout-audit` passed against the rebuilt package with primary actions aligned left. Its 50 screenshots and report are in `build/qt612-release/ui-checks/ui-check-rzqaoz3j/`.
- All 20 local Markdown pages and their 18 screenshot references resolve.

Do not publish the migration checklist itself into the wiki; publish the contents of `docs/wiki/`.
