#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 0 && ( "$1" == "-h" || "$1" == "--help" ) ]]; then
  cat <<'EOF'
Usage: launcher_sbs.sh [--rebuild] [media-path-or-url] [vlc args...]

Env overrides:
  OPEN3D_VLC_SRC=/path/to/vlc-3.0.23-source
  OPEN3D_LAUNCHER_SKIP_REBUILD=1
  EDGE264MVC_LIB=/path/to/libedge264.so.1
  OPEN3D_X11_BYPASS_COMPOSITOR=1
  OPEN3D_PROCESS_CPUSET=4-7
  OPEN3D_PROCESS_CPU_BACKEND=auto|systemd|taskset
  OPEN3D_PRESENTER_PREFERRED_CPU=auto|N
  OPEN3D_PROCESS_SCHED_POLICY=fifo|rr
  OPEN3D_PROCESS_RT_PRIORITY=10
  OPEN3D_PROCESS_IO_CLASS=realtime|best-effort|idle
  OPEN3D_PROCESS_IO_PRIORITY=0

When no media path is provided, VLC starts with the maintained Open3D SBS
runtime/profile so you can open content from the UI.
EOF
  exit 0
fi

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${REPO_DIR}/scripts/open3d_process_isolation.sh"
FORCE_REBUILD=0
MEDIA=""
isolation_prefix=()
isolation_vlc_args=()

prepare_launcher_log() {
  local log_tag="$1"
  local log_root="${OPEN3D_VLC_LOG_DIR:-${REPO_DIR}/local/out/runtime_logs/launcher}"
  local timestamp
  timestamp="$(date +%Y%m%d_%H%M%S)"
  mkdir -p "${log_root}"

  if [[ -n "${OPEN3D_VLC_LOG_PATH:-}" ]]; then
    printf '%s\n' "${OPEN3D_VLC_LOG_PATH}"
  else
    printf '%s/%s_%s_pid%s.log\n' "${log_root}" "${log_tag}" "${timestamp}" "$$"
  fi
}

start_x11_window_tuner() {
  if [[ "${OPEN3D_X11_BYPASS_COMPOSITOR:-0}" != "1" ]]; then
    return 0
  fi
  if [[ -z "${DISPLAY:-}" ]]; then
    echo "open3d x11 tuner: DISPLAY is not set, skipping"
    return 0
  fi
  "${REPO_DIR}/scripts/open3d_x11_window_tuner.sh" "$$" bypass_compositor &
}

default_vlc_src() {
  local latest=""

  if [[ -n "${OPEN3D_VLC_SRC:-}" ]]; then
    printf '%s\n' "${OPEN3D_VLC_SRC}"
    return 0
  fi

  while IFS= read -r latest; do
    [[ -n "${latest}" ]] && break
  done < <(find /tmp -maxdepth 1 -mindepth 1 -type d -name 'vlc-3.0.23-src-*' -printf '%T@ %p\n' 2>/dev/null | sort -nr | awk 'NR==1 {print $2}')

  if [[ -n "${latest}" ]]; then
    printf '%s\n' "${latest}"
  else
    printf '%s\n' "/tmp/vlc-3.0.23-src-20260310"
  fi
}

VLC_SRC="$(default_vlc_src)"
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

if [[ "${FORCE_REBUILD}" == "1" ]]; then
  if [[ ! -d "${VLC_SRC}" ]]; then
    echo "Error: VLC source tree not found: ${VLC_SRC}" >&2
    exit 2
  fi
  "${REPO_DIR}/build.sh" "${VLC_SRC}"
elif ! "${REPO_DIR}/build.sh" --check >/dev/null 2>&1; then
  if [[ "${OPEN3D_LAUNCHER_SKIP_REBUILD:-0}" == "1" ]]; then
    echo "Error: required Open3D runtime artifacts are missing and auto-build is disabled." >&2
    echo "Run ./build.sh or relaunch without OPEN3D_LAUNCHER_SKIP_REBUILD=1." >&2
    exit 3
  fi
  if [[ ! -d "${VLC_SRC}" ]]; then
    echo "Error: VLC source tree not found: ${VLC_SRC}" >&2
    exit 2
  fi
  "${REPO_DIR}/build.sh" "${VLC_SRC}"
fi

export OPEN3D_MVC_RUNTIME_PROFILE=stable
export EDGE264MVC_LIB="${REPO_DIR}/local/out/stable_runtime/latest/runtime-lib/libedge264.so.1"
export OPEN3D_STAGE_SKIP_VOUT="${OPEN3D_STAGE_SKIP_VOUT:-1}"
export OPEN3D_MVC_ENABLE_VOUT="${OPEN3D_MVC_ENABLE_VOUT:-0}"
export OPEN3D_MVC_NO_PLUGINS_CACHE="${OPEN3D_MVC_NO_PLUGINS_CACHE:-1}"
export OPEN3D_MVC_IGNORE_CONFIG="${OPEN3D_MVC_IGNORE_CONFIG:-1}"
export OPEN3D_PROCESS_CPUSET="${OPEN3D_PROCESS_CPUSET:-4-7}"
export OPEN3D_PROCESS_CPU_BACKEND="${OPEN3D_PROCESS_CPU_BACKEND:-auto}"
export OPEN3D_PRESENTER_PREFERRED_CPU="${OPEN3D_PRESENTER_PREFERRED_CPU:-auto}"
export OPEN3D_PROCESS_IO_CLASS="${OPEN3D_PROCESS_IO_CLASS:-best-effort}"
export OPEN3D_PROCESS_IO_PRIORITY="${OPEN3D_PROCESS_IO_PRIORITY:-0}"
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
export LD_LIBRARY_PATH="${REPO_DIR}/local/out/stable_runtime/latest/runtime-lib:${LD_LIBRARY_PATH:-}"
export VLC_BIN="${VLC_BIN:-/usr/bin/vlc}"

log_path="$(prepare_launcher_log sbs_ui)"
mkdir -p "$(dirname "${log_path}")"
echo "Playback log: ${log_path}"
exec > >(tee -a "${log_path}") 2>&1

start_x11_window_tuner

open3d_isolation_build_exec_prefix isolation_prefix
open3d_isolation_append_presenter_args isolation_vlc_args "$@"
if (( ${#isolation_prefix[@]} > 0 )); then
  echo "Playback isolation: ${isolation_prefix[*]}"
fi
if (( ${#isolation_vlc_args[@]} > 0 )); then
  echo "Playback isolation args: ${isolation_vlc_args[*]}"
fi

exec "${isolation_prefix[@]}" "${VLC_BIN}" \
  --vout=gl \
  --codec=edge264mvc,avcodec \
  --no-qt-privacy-ask \
  --no-qt-error-dialogs \
  --no-edge264mvc-advertise-multiview \
  --no-plugins-cache \
  --ignore-config \
  "${isolation_vlc_args[@]}" \
  "$@"
