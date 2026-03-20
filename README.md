# VLCModule (Open3DOLED)

This repository tracks the maintained VLC-side source, patch, build, and final
playback launch surface for Open3DOLED.

## Tracked Surface

Tracked here:
- VLC-side source under `renderer/`
- canonical VLC 3.0.23 patch under `patches/vlc-3.0.23/`
- maintained build helpers under `scripts/`
- root build and launch entry points:
  - `build.sh`
  - `launcher_sbs.sh`
  - `launcher_pf.sh`
- pinned vendor submodules:
  - `vendor/edge264`
  - `vendor/libbluray`

Local-only and ignored:
- `local/assets/`
- `local/docs/`
- `local/out/`
- `local/scripts/`

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
- `open3dmkv` demux
- `open3dbluraymvc` access plugin
- vendored `libbluray`
- vendored `edge264`

All generated runtime/build artifacts live under `local/out/`.

## Launch

SBS monitor mode:

```bash
./launcher_sbs.sh [media-path-or-url] [vlc args...]
```

Open3D page-flipping mode:

```bash
./launcher_pf.sh [media-path-or-url] [vlc args...]
```

Default launcher behavior:
- do not rebuild if the maintained runtime already exists
- rebuild automatically only if required runtime artifacts are missing
- force rebuild only when `--rebuild` is passed

## Supported Maintained Inputs

- `.iso`
  - routed to `open3dbluraymvc://`
- `.mkv`
  - played through native MKV demux with `edge264mvc`
- `.264`, `.h264`, `.mvc`
  - played with `--demux=open3dmkv`

Examples:

```bash
./launcher_sbs.sh "/path/to/disc.iso"
./launcher_pf.sh "/path/to/movie.mkv"
./launcher_sbs.sh "/path/to/stream.264"
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
