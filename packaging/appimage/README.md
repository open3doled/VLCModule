# AppImage Docker Prototype

This directory defines the first Docker-based build environment for the
Open3DOLED AppImage investigation.

Current intent:

- build a matched VLC fullstack inside Docker
- package that runtime as an AppImage
- export the AppImage to the host
- run and validate the artifact outside the container

This is not yet the final release pipeline. It is the first controlled
prototype environment.

## Current Layout Decision

Container paths:

- `/work`
  - bind-mounted repo root
- `/out`
  - bind-mounted host output directory for exported artifacts
- `/opt/open3doled-appimage/src`
  - container-local source/cache area
- `/opt/open3doled-appimage/build`
  - container-local build root
- `/opt/open3doled-appimage/AppDir`
  - container-local AppDir assembly root
- `/opt/open3doled-appimage/tools`
  - container-local packaging tools

Why this shape:

- repo source stays mounted from the host for iteration
- build products and AppDir assembly stay container-local
- exported AppImage artifacts land in one host-visible output directory

## Current Base Image Choice

- `debian:11`

Reasoning for the first prototype:

- old enough to be useful for AppImage portability
- still aligned with the current Debian/Ubuntu-oriented build assumptions in the
  repo
- conservative first step before revisiting the exact base image later

## Wrapper Script

Use the helper:

```bash
scripts/open3d_appimage_docker.sh build-image
scripts/open3d_appimage_docker.sh exec -- meson --version
scripts/open3d_appimage_docker.sh exec -- ./scripts/build_open3d_appimage_fullstack.sh
scripts/open3d_appimage_docker.sh exec -- ./scripts/build_open3d_appimage_appdir.sh
scripts/open3d_appimage_docker.sh exec -- ./scripts/build_open3d_appimage_artifact.sh
scripts/open3d_appimage_docker.sh shell
./scripts/run_open3d_appimage_appdir_smoke.sh /path/to/media.mkv --sub-track=0
./scripts/run_open3d_appimage_smoke.sh /path/to/media.mkv --sub-track=0
```

The current fullstack helper does not yet emit a finished AppImage. It builds
the first matched VLC/Open3DOLED runtime shape inside Docker and records logs
under `/out/fullstack-builds/` for later AppDir and AppImage slices.

The AppDir helper is the next bounded step. It runs the matched fullstack build
inside Docker, assembles a first exported AppDir under `/out/appdir-builds/`,
and verifies the bundled `AppRun` with `--version`. This first AppDir is
intentionally minimal: it proves the relocatable launcher/lib/plugin/data shape
before later slices broaden the bundled plugin set and wrap it into a final
AppImage artifact.

The host smoke helper is the next diagnostic layer on top of that AppDir. It
runs the latest exported AppDir on the host in a bounded headless mode
(`--intf dummy --vout dummy --play-and-exit`) so missing bundled plugins or
launcher-behavior gaps can be identified before attempting full PF/SBS visual
validation.

For packaging diagnostics the smoke helper currently forces:

- `OPEN3D_PROCESS_CPU_BACKEND=taskset`

This keeps the host-side child process output visible. When available, it also
records a file-access trace so missing bundled shared libraries can be
identified before broader AppImage wrapping work.

The next packaging step is the real AppImage wrapper. The artifact helper wraps
the latest AppDir into a `.AppImage` inside Docker using `appimagetool`. On
this current Debian 11 base, the downloaded `appimagetool` AppImage is not
directly executable, so the helper extracts its inner tool with `unsquashfs`
and drives that extracted `AppRun` instead. The same extract-and-run fallback
is also used for the bounded in-container verification when needed.

The AppImage smoke helper mirrors the AppDir smoke path on the host. It
defaults to `APPIMAGE_EXTRACT_AND_RUN=1` so the bounded host validation does
not depend on FUSE availability while the packaging work is still in progress.
It also forces `APPIMAGELAUNCHER_DISABLE=1` so the smoke result reflects the
artifact itself rather than host AppImage manager integration.
