# VLCModule (Open3DOLED)

This repository tracks the maintained VLC-side source, patch, build, and final
playback launch surface for Open3DOLED.

## Tracked Surface

Tracked here:
- VLC-side source under `renderer/`
- canonical VLC 3.0.23 patch under `patches/vlc-3.0.23/`
- maintained build helpers under `scripts/`
- root build entry point:
  - `build.sh`
- maintained AppImage packaging/launch surface:
  - `scripts/build_open3d_appimage_artifact.sh`
  - `packaging/appimage/AppRun`
- pinned vendor submodules:
  - `vendor/edge264`
  - `vendor/libbluray`

Local-only and ignored:
- `local/assets/`
- `local/docs/`
- `local/out/`
- `local/scripts/`

## Branch And Publish Policy

Use `open3doled_*` as the active feature-branch prefix across related repos.

Before creating merge/publish commits for the canonical remote branches:
- set `user.name` to `open3doled`
- set `user.email` to `159269811+open3doled@users.noreply.github.com`
- keep the `origin` SSH URL on the Open3DOLED namespace:
  - `VLCModule`: `git@github-open3doled:open3doled/VLCModule.git`
  - `vendor/edge264`: `git@github-open3doled:open3doled/edge264.git`
  - `vendor/libbluray`: `git@github-open3doled:open3doled/libbluray.git`

Merge targets:
- parent repo `VLCModule`
  - merge target: `main`
  - keep topic branches unsquashed
  - flatten/scrub only when updating `main`
  - only push `main` to remote
- local repo `local/`
  - merge target: `main`
  - do not squash/flatten this repo
  - only push `main` if a remote is ever added
- sub-repo `vendor/edge264`
  - merge target: `open3doled_001`
  - do not merge to `master` / `main`
  - keep topic branches unsquashed
  - flatten/scrub only when updating `open3doled_001`
  - only push `open3doled_001` to remote
- sub-repo `vendor/libbluray`
  - merge target: `open3doled_001`
  - do not merge to `master` / `main`
  - keep topic branches unsquashed
  - flatten/scrub only when updating `open3doled_001`
  - only push `open3doled_001` to remote

Do not push feature branches such as `open3doled_002_*` or `open3doled_003_*`.

After switching the parent repo branch, use the tracked helper below to align
the child repos to the expected working branches:

```bash
./scripts/sync_open3doled_repo_branches.sh
```

Expected branch map:
- parent `main`
  - `vendor/edge264` -> `open3doled_001`
  - `vendor/libbluray` -> `open3doled_001`
  - `local` -> `main`
- parent `open3doled_*`
  - each child repo uses the same branch name when it exists
  - otherwise it falls back to the canonical branch above

Use `--dry-run` to inspect the branch decisions without switching anything.

To create and switch a new project branch across the whole VLCModule hierarchy:

```bash
./scripts/sync_open3doled_repo_branches.sh --new-project ui_cleanup
```

If the name does not already start with `open3doled_`, the helper adds that
prefix automatically.

Historical test/oracle/assembly helpers were moved to `local/scripts/` and are
not part of the maintained tracked launch path anymore.

## Build

Build the maintained runtime:

```bash
./build.sh [/path/to/vlc-3.0.23-source]
```

Check whether the staged runtime is already present:

```bash
./build.sh --check
```

The maintained build produces and stages:
- `open3d` video output
- `edge264mvc` codec
- `open3dannexb` raw Annex-B demux
- Matroska `mkv` demux tagged for Open3DOLED MVC MKV playback
- `open3dbluraymvc` access plugin
- vendored `libbluray`
- vendored `edge264`

All generated runtime/build artifacts live under `local/out/`.

Build the maintained AppImage artifact:

```bash
./scripts/build_open3d_appimage_artifact.sh
```

The packaged artifact is written under the configured AppImage output root,
typically `local/out/appimage/appimage-builds/`.

## Launch

The maintained playback path is the packaged AppImage, not a host VLC launcher.

Page-flipping / `open3d` mode:

```bash
APPIMAGE=/path/to/Open3DOLED-VLC-3.0.23-x86_64.AppImage

APPIMAGELAUNCHER_DISABLE=1 \
APPIMAGE_EXTRACT_AND_RUN=1 \
"${APPIMAGE}" [media-path-or-url] [vlc args...]
```

SBS monitor mode:

```bash
APPIMAGE=/path/to/Open3DOLED-VLC-3.0.23-x86_64.AppImage

APPIMAGELAUNCHER_DISABLE=1 \
APPIMAGE_EXTRACT_AND_RUN=1 \
OPEN3D_MVC_ENABLE_VOUT=0 \
"${APPIMAGE}" [media-path-or-url] [vlc args...]
```

Default AppImage behavior:
- page-flipping remains the maintained default when `OPEN3D_MVC_ENABLE_VOUT` is
  unset or `1`
- set `OPEN3D_MVC_ENABLE_VOUT=0` only for SBS / `--vout=gl` monitor-style runs
- use `APPIMAGELAUNCHER_DISABLE=1` and `APPIMAGE_EXTRACT_AND_RUN=1` to keep the
  runtime surface reproducible and independent of desktop AppImage integration

Recommended max-performance runtime defaults:
- `OPEN3D_PROCESS_CPUSET=4-7`
- `OPEN3D_PROCESS_CPU_BACKEND=auto`
- `OPEN3D_PRESENTER_PREFERRED_CPU=auto`
- `OPEN3D_PROCESS_IO_CLASS=best-effort`
- `OPEN3D_PROCESS_IO_PRIORITY=0`

These values are now the default in both launchers. They mean:
- `OPEN3D_PROCESS_CPUSET=4-7`
  - isolate the VLC process tree onto CPUs `4-7`
- `OPEN3D_PROCESS_CPU_BACKEND=auto`
  - prefer `systemd-run --user --scope -p AllowedCPUs=...`, then fall back to `taskset`
- `OPEN3D_PRESENTER_PREFERRED_CPU=auto`
  - bind the presenter thread to the first isolated CPU
- `OPEN3D_PROCESS_IO_CLASS=best-effort`
  - use `ionice` best-effort scheduling for the process tree
- `OPEN3D_PROCESS_IO_PRIORITY=0`
  - use the highest best-effort I/O priority

Manual AppImage example with the recommended settings spelled out explicitly:

```bash
APPIMAGE=/path/to/Open3DOLED-VLC-3.0.23-x86_64.AppImage

APPIMAGELAUNCHER_DISABLE=1 \
APPIMAGE_EXTRACT_AND_RUN=1 \
OPEN3D_PROCESS_CPUSET=4-7 \
OPEN3D_PROCESS_CPU_BACKEND=auto \
OPEN3D_PRESENTER_PREFERRED_CPU=auto \
OPEN3D_PROCESS_IO_CLASS=best-effort \
OPEN3D_PROCESS_IO_PRIORITY=0 \
"${APPIMAGE}" "/path/to/disc.iso"
```

Remaining potential performance work from the tracked plan is now mainly the
direct-presentation feasibility track:
- investigate whether a simpler unmanaged/fullscreen X11 path can reduce the
  remaining jitter further
- if that is not enough, evaluate a separate DRM/KMS-style direct presentation
  path as a larger follow-on effort
- deferred KMS/direct-display notes are tracked in
  `docs/KMS_DIRECT_PLAYBACK_FUTURE.md`

## Supported Maintained Inputs

- `.iso`
  - routed to `open3dbluraymvc://`
- mounted Blu-ray roots, `BDMV`, and `index.bdmv`
  - routed to `open3dbluraymvc://`
- `.mkv`
  - played through native MKV demux with `edge264mvc`
  - normal Matroska AVC opens can now route to `edge264mvc` through the custom `mkv` module without requiring URL tricks
- `.264`, `.h264`, `.mvc`
  - played with `--demux=open3dannexb` raw Annex-B passthrough

## Blu-ray Menus

Current maintained Blu-ray menu policy:
- explicit Open3DOLED Blu-ray launches default to menus on
- the CLI opt-out is `--no-bluray-menu`
- the in-process `Open Disc` 3D UI opt-out is `Start without menus`
- stock `Blu-ray` remains the stock VLC/libbluray path; the Open3DOLED
  default-on menu policy applies to the explicit Open3DOLED 3D Blu-ray path

Current maintained AppImage runtime prerequisite:
- BD-J menu validation on the AppImage path currently assumes Java 8
- if the host default JVM is newer, BD-J can fail during startup with module
  access errors such as `IllegalAccessError`
- before menu-capable AppImage runs, prefer exporting a Java 8 runtime
  explicitly:

```bash
APPIMAGE=/path/to/Open3DOLED-VLC-3.0.23-x86_64.AppImage

export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64
export PATH="$JAVA_HOME/bin:$PATH"
APPIMAGELAUNCHER_DISABLE=1 APPIMAGE_EXTRACT_AND_RUN=1 "${APPIMAGE}" "/path/to/disc.iso"
```

Menu-capable vs no-menu AppImage examples:

```bash
APPIMAGE=/path/to/Open3DOLED-VLC-3.0.23-x86_64.AppImage

APPIMAGELAUNCHER_DISABLE=1 APPIMAGE_EXTRACT_AND_RUN=1 "${APPIMAGE}" "/path/to/disc.iso"
APPIMAGELAUNCHER_DISABLE=1 APPIMAGE_EXTRACT_AND_RUN=1 "${APPIMAGE}" "/path/to/disc.iso" --no-bluray-menu
```

Current support boundary and limitations:
- maintained real visible/navigable menu validation on the actual X11 launcher
  path has only been exercised against a small private reference set
- broader Blu-ray menu compatibility should still be treated as disc-by-disc
  until separately validated
- the explicit Open3DOLED 3D Blu-ray path is the maintained menu-capable path;
  stock `Blu-ray` remains the stock VLC/libbluray path
- if you need the older fallback behavior for a disc that has not yet been
  validated on the maintained menu path, use `--no-bluray-menu` or
  `Start without menus`

Developer note for future menu debugging:
- before deeper BD-J/menu flow tracing, first confirm runtime completeness on
  the tested path
- verify the maintained staged runtime or packaged AppDir is actually in use
- verify BD-J jar exposure is present:
  - AppRun `LIBBLURAY_CP`
  - packaged AppDir `usr/share/java`
- inspect logs for runtime-completeness failures before treating the issue as a
  disc-logic problem:
  - `libbluray-j2se-*.jar not found`
  - `BD-J check: Failed to load libbluray.jar`
  - `Java required`
- on the AppImage path, prefer Java 8 and treat Java 9+
  `IllegalAccessError` module-access faults as environment/runtime failures
  first

## MKV 3D Subtitles

Maintained MVC MKV subtitle behavior:
- MKV playback stays on the native Matroska demux path plus `edge264mvc`
- selected subtitle-track mapping is derived from Matroska metadata:
  - `3d-plane`
  - `SOURCE_ID`
- when recoverable MVC-side `OFMD` depth is present, MKV now uses the same
  shared subtitle-depth bridge path as Blu-ray
- active subtitle depth preference order is:
  - Blu-ray live depth latch
  - valid shared-bridge runtime depth
  - static MKV per-track fallback

What that means in practice:
- Blu-ray keeps the existing dynamic per-subtitle depth path
- MVC MKV can now use dynamic depth from the MVC-side metadata when available
- if dynamic MKV depth is missing or unverified, playback falls back cleanly to
  the static per-track MKV offset

Maintained AppRun behavior for MKV:
- AppRun auto-enables MKV timestamp normalization unless
  explicitly overridden
- for `--sub-track=N`, the launcher also resolves and passes:
  - `--open3d-mkv-subtitle-force`
  - `--open3d-mkv-subtitle-static-offset-units=...`
  - `--open3d-mkv-subtitle-plane=...`
  - `--open3d-mkv-subtitle-source-id=...`

Tracked validation helpers:
- `local/projects/subtitles/run_mkv_subtitle_validation.sh`
  - PF/SBS visual capture workflow
  - writes `dynamic_depth_summary.txt` comparing static launcher metadata vs
    active runtime subtitle depth

## Diagnostics And Validation

The current maintained runtime intentionally keeps the optional subtitle and
presenter diagnostics in place. They are not required for normal playback, but
they remain useful for:
- platform-specific pacing investigation
- subtitle-depth regression checks
- verifying MKV `OFMD` dynamic depth behavior
- checking PF/SBS subtitle placement after future changes

Useful optional flags:
- `--open3d-debug-status`
  - periodic Open3D state and subtitle-depth logging
- `--open3d-presenter-stage-profile`
  - detailed presenter stage timing on bad frames only
- `--edge264mvc-log-sei`
  - decoder-side MVC SEI diagnostics for subtitle-depth investigation

Known-good manual validation checks:
- Blu-ray PF subtitle depth
  ```bash
  APPIMAGE=/path/to/Open3DOLED-VLC-3.0.23-x86_64.AppImage
  APPIMAGELAUNCHER_DISABLE=1 APPIMAGE_EXTRACT_AND_RUN=1 "${APPIMAGE}" "/path/to/disc.iso" --sub-track=2 --open3d-debug-status --verbose=2
  ```
- MKV PF subtitle depth
  ```bash
  APPIMAGE=/path/to/Open3DOLED-VLC-3.0.23-x86_64.AppImage
  APPIMAGELAUNCHER_DISABLE=1 APPIMAGE_EXTRACT_AND_RUN=1 "${APPIMAGE}" "/path/to/movie.mkv" --sub-track=4 --open3d-debug-status --verbose=2
  ```
- OSD toggle regression check
  - while either title is playing, press `Ctrl+Shift+F9`
  - subtitle depth should remain stable when toggling the status OSD on/off
- SBS subtitle placement check
  - while playback is running, press `Ctrl+Shift+F8`
  - subtitles should remain correctly duplicated/aligned per eye

Current maintained subtitle expectations:
- Blu-ray uses dynamic per-subtitle depth through the shared subtitle-depth
  bridge
- MKV prefers dynamic MVC-side `OFMD` depth when available
- MKV falls back cleanly to static per-track subtitle depth when dynamic depth
  is unavailable
- OSD on/off should not change effective subtitle depth
- the optional diagnostics should remain available for future tuning and release
  validation

Examples:

```bash
APPIMAGE=/path/to/Open3DOLED-VLC-3.0.23-x86_64.AppImage

APPIMAGELAUNCHER_DISABLE=1 APPIMAGE_EXTRACT_AND_RUN=1 OPEN3D_MVC_ENABLE_VOUT=0 "${APPIMAGE}" "/path/to/disc.iso"
APPIMAGELAUNCHER_DISABLE=1 APPIMAGE_EXTRACT_AND_RUN=1 "${APPIMAGE}" "/path/to/movie.mkv"
APPIMAGELAUNCHER_DISABLE=1 APPIMAGE_EXTRACT_AND_RUN=1 OPEN3D_MVC_ENABLE_VOUT=0 "${APPIMAGE}" "/path/to/stream.264"
```

## VLC Patch Maintenance

Apply the canonical VLC patch to a pristine VLC 3.0.23 tree:

```bash
scripts/apply_vlc3_patch.sh /path/to/vlc-3.0.23
```

Regenerate the canonical patch from the maintained source in this repo:

```bash
scripts/update_vlc3_patch.sh [/path/to/pristine-vlc-source-or-tarball] [output-patch]
```
