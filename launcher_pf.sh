#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 0 && ( "$1" == "-h" || "$1" == "--help" ) ]]; then
  cat <<'EOF'
Usage: launcher_pf.sh [--rebuild] [media-path-or-url] [vlc args...]

Env overrides:
  OPEN3D_VLC_SRC=/path/to/vlc-3.0.23-source
  OPEN3D_LAUNCHER_SKIP_REBUILD=1
  EDGE264MVC_LIB=/path/to/libedge264.so.1

When no media path is provided, VLC starts with the maintained Open3D page
flipping runtime/profile so you can open content from the UI.
EOF
  exit 0
fi

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VLC_SRC="${OPEN3D_VLC_SRC:-/tmp/vlc-3.0.23-src-20260310}"
FORCE_REBUILD=0
MEDIA=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --rebuild)
      FORCE_REBUILD=1
      shift
      ;;
    --)
      shift
      break
      ;;
    *)
      break
      ;;
  esac
done

if [[ $# -gt 0 && "$1" != -* ]]; then
  MEDIA="$1"
  shift
fi

if [[ ! -d "${VLC_SRC}" ]]; then
  echo "Error: VLC source tree not found: ${VLC_SRC}" >&2
  exit 2
fi

if [[ "${FORCE_REBUILD}" == "1" ]]; then
  "${REPO_DIR}/build.sh" "${VLC_SRC}"
elif ! "${REPO_DIR}/build.sh" --check >/dev/null 2>&1; then
  if [[ "${OPEN3D_LAUNCHER_SKIP_REBUILD:-0}" == "1" ]]; then
    echo "Error: required Open3D runtime artifacts are missing and auto-build is disabled." >&2
    echo "Run ./build.sh or relaunch without OPEN3D_LAUNCHER_SKIP_REBUILD=1." >&2
    exit 3
  fi
  "${REPO_DIR}/build.sh" "${VLC_SRC}"
fi

export OPEN3D_MVC_RUNTIME_PROFILE=stable
export OPEN3D_STAGE_SKIP_VOUT=0
export OPEN3D_MVC_ENABLE_VOUT=1
export OPEN3D_MVC_NO_PLUGINS_CACHE=1
export OPEN3D_MVC_IGNORE_CONFIG=1
export EDGE264MVC_LIB="${REPO_DIR}/local/out/stable_runtime/latest/lib/libedge264.so.1"
export OPEN3D_MVC_FORCE_SEPARATE_INSTANCE="${OPEN3D_MVC_FORCE_SEPARATE_INSTANCE:-1}"
export OPEN3DBLURAYMVC_HOOK_LOG="${OPEN3DBLURAYMVC_HOOK_LOG:-1}"
export OPEN3D_MVC_VLC_VERBOSE="${OPEN3D_MVC_VLC_VERBOSE:-3}"
export EDGE264MVC_THREADS="${EDGE264MVC_THREADS:--1}"
if [[ -z "${EDGE264MVC_EXTRA_ARGS:-}" ]]; then
  export EDGE264MVC_EXTRA_ARGS="--no-edge264mvc-advertise-multiview"
elif ! grep -Eq -- '(^|[[:space:]])--(no-)?edge264mvc-advertise-multiview([[:space:]]|$)' <<<"${EDGE264MVC_EXTRA_ARGS}"; then
  export EDGE264MVC_EXTRA_ARGS="${EDGE264MVC_EXTRA_ARGS} --no-edge264mvc-advertise-multiview"
fi

if [[ -n "${MEDIA}" ]]; then
  exec "${REPO_DIR}/scripts/run_open3d_playback_vlc.sh" "${VLC_SRC}" "${MEDIA}" "$@"
fi

SYSTEM_PLUGIN_PATH="/usr/lib/x86_64-linux-gnu/vlc/plugins"
if [[ ! -d "${SYSTEM_PLUGIN_PATH}" ]]; then
  SYSTEM_PLUGIN_PATH="/usr/lib/vlc/plugins"
fi

export VLC_PLUGIN_PATH="${REPO_DIR}/local/out/stable_runtime/latest/plugins:${SYSTEM_PLUGIN_PATH}"
export LD_LIBRARY_PATH="${REPO_DIR}/local/out/stable_runtime/latest/lib:${LD_LIBRARY_PATH:-}"

exec /usr/bin/vlc \
  --vout=open3d \
  --codec=edge264mvc,avcodec \
  --open3d-enable \
  --open3d-emitter-enable \
  --open3d-emitter-tty=auto \
  --no-edge264mvc-advertise-multiview \
  --no-plugins-cache \
  --ignore-config \
  --no-qt-privacy-ask \
  "$@"
