# Libbluray Vendor Layout

## Purpose
This directory now uses a single pinned libbluray fork as a submodule.

The maintained source of truth is:
- submodule path:
  - `vendor/libbluray`
- submodule remote:
  - `git@github-open3doled:open3doled/libbluray.git`
- development branch:
  - `open3doled_001`

The Open3DOLED fork was created by mirroring the official VideoLAN upstream and
then applying the maintained MVC changes on top of upstream tag `1.4.1`.

Verification note:
- latest official upstream tag was verified on `2026-03-19` from:
  - `https://code.videolan.org/videolan/libbluray.git`
- verified upstream tag:
  - `1.4.1`
- upstream tag commit:
  - `7d94f2660af5bfc16015291a03539329135c18f1`

## Layout

### Maintained Fork Submodule
- Path:
  - `vendor/libbluray`
- Meaning:
  - pinned Open3DOLED libbluray fork
  - active libbluray MVC development should happen in the fork and then be
    advanced in the parent repo by updating the submodule commit

### Build / Stage Roots
- Build root:
  - `local/out/vendor_build/libbluray`
- Stage root:
  - `local/out/vendor_stage/libbluray`
- Meaning:
  - libbluray builds happen out of tree
  - staged vendored libs should be preferred over the host system libbluray in
    maintained validation paths

## First Expected Touch Areas
These are the first libbluray subtrees most likely to matter:

- core public/internal control:
  - `src/libbluray/bluray.c`
  - `src/libbluray/bluray.h`
  - `src/libbluray/bluray_internal.h`
- navigation and playlist/clip metadata:
  - `src/libbluray/bdnav/navigation.c`
  - `src/libbluray/bdnav/navigation.h`
  - `src/libbluray/bdnav/mpls_parse.c`
  - `src/libbluray/bdnav/mpls_data.h`
  - `src/libbluray/bdnav/clpi_parse.c`
  - `src/libbluray/bdnav/clpi_data.h`
  - `src/libbluray/bdnav/index_parse.c`
- possible stream/demux helpers:
  - `src/libbluray/decoders/m2ts_demux.c`
  - `src/libbluray/decoders/m2ts_filter.c`

## Working Rule
- Treat `vendor/libbluray` as the only maintained libbluray tree in this repo.
- Upstream comparison should be done against the official VideoLAN repo and the
  pinned upstream base recorded in the fork history, not against a second
  tracked source snapshot in this parent repo.
- Do not silently mix staged vendored libbluray and system libbluray in the
  same validation path.
