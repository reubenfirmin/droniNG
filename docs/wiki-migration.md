# Publishing the droniNG help wiki

The help wiki is built from `docs/wiki/` and published to
<https://reubenfirmin.github.io/droniNG/> by
[Publish wiki](../.github/workflows/publish-wiki.yml). The app's Help buttons,
setup wizard, Help menu and Home documentation links target this site.

## Publishing

The workflow validates and builds the wiki on pull requests. Changes to the wiki,
its builder or its publishing workflow on **main** deploy the site automatically.
It also supports **Actions → Publish wiki → Run workflow** on main.

The publisher uses GitHub Pages with the built-in workflow token and OIDC.
It needs no personal access token or separate wiki repository. GitHub Pages must
use **GitHub Actions** as its build source in the repository's Pages settings.
Repository administrators can enable that setting through the GitHub API:

```sh
gh api --method POST repos/reubenfirmin/droniNG/pages -f build_type=workflow
```

The deployment job reports the published URL in the `github-pages` environment.
A failed build leaves the previous deployment online. Pull requests cannot deploy.

## Editing and previewing

Edit the Markdown pages directly in `docs/wiki/`; keep topic filenames stable
because application Help buttons link to them. Link to other topics by filename
without the `.md` suffix, and keep screenshots under `docs/wiki/images/`.
`Home.md` becomes the site's landing page and `_Sidebar.md` defines navigation.

```sh
python3 -m pip install -r docs/wiki-requirements.txt
python3 ground/tests/wiki_site.py
python3 docs/build_wiki.py --output build/wiki-site
python3 -m http.server 8000 --directory build/wiki-site
```

Open <http://localhost:8000/> to review the result. The builder checks local page
and image references before replacing its previous output. The tests also check
that every application Help destination exists in the generated site.

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
remain in the documentation; application Help buttons use the droniNG site.
