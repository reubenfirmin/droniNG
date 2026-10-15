# droniNG branding

The flight mark combines four rotors and a heading arrow. Navy (`#142532`),
teal (`#20c8b5`), and amber (`#f4bd50`) are the primary colors. Write the
product name as **droniNG**; use `droning` for package names and paths.

- `droning_logo_only.svg`: application and Android icons.
- `droning_logo_full.svg`: horizontal wordmark and repository banner.
- `gcs_splash.svg`: ground station startup screen.
- `welcome-logo.svg`: ground station welcome screen.
- `droning_osd.svg`: black and white firmware overlay.

Run `make -C branding` from the repository root to export the PNG assets
with ImageMagick. Run `make -C branding osd` to regenerate the OSD image
header using the existing NumPy/Matplotlib encoder. The firmware images
retain their original dimensions and memory requirements.

Original dRonin author and copyright notices remain attributed to upstream.
Existing log signatures and external services retain their original names
where those names are required for compatibility.
