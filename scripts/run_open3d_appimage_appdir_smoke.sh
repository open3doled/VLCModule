#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_open3d_appimage_appdir_smoke.sh <media-path-or-url> [vlc args...]

Runs the latest exported Open3DOLED AppDir on the host in a bounded headless
smoke mode so packaging/runtime gaps can be identified without needing a full
interactive PF/SBS session.

Env overrides:
  OPEN3D_APPIMAGE_APPDIR=/path/to/AppDir
  OPEN3D_APPIMAGE_SMOKE_RUN_TIME=5
  OPEN3D_APPIMAGE_SMOKE_VERBOSE=2
  OPEN3D_APPIMAGE_SMOKE_CPU_BACKEND=taskset
  OPEN3D_APPIMAGE_SMOKE_TRACE_FILE=1
EOF
}

if [[ $# -lt 1 ]]; then
  usage >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

discover_latest_appdir() {
  find "${REPO_DIR}/local/out/appimage/appdir-builds" \
    -mindepth 2 -maxdepth 2 -type d -name AppDir \
    -printf '%T@ %p\n' 2>/dev/null | sort -nr | awk 'NR==1 {print $2}'
}

APPDIR_PATH="${OPEN3D_APPIMAGE_APPDIR:-}"
if [[ -z "${APPDIR_PATH}" ]]; then
  APPDIR_PATH="$(discover_latest_appdir || true)"
fi

if [[ -z "${APPDIR_PATH}" || ! -d "${APPDIR_PATH}" ]]; then
  echo "Error: AppDir not found. Build one first or set OPEN3D_APPIMAGE_APPDIR." >&2
  exit 2
fi

MEDIA_PATH="$1"
shift

RUN_TIME="${OPEN3D_APPIMAGE_SMOKE_RUN_TIME:-5}"
VERBOSE="${OPEN3D_APPIMAGE_SMOKE_VERBOSE:-2}"
SMOKE_CPU_BACKEND="${OPEN3D_APPIMAGE_SMOKE_CPU_BACKEND:-taskset}"
TRACE_FILE_ENABLED="${OPEN3D_APPIMAGE_SMOKE_TRACE_FILE:-1}"
RUN_STAMP="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="${REPO_DIR}/local/out/appimage/appdir-smoke/${RUN_STAMP}"
LOG_PATH="${RUN_DIR}/smoke.log"
TRACE_LOG_PATH="${RUN_DIR}/smoke.strace.log"

mkdir -p "${RUN_DIR}"

{
  echo "Open3DOLED AppDir host smoke"
  echo "appdir=${APPDIR_PATH}"
  echo "media=${MEDIA_PATH}"
  echo "run_time=${RUN_TIME}"
  echo "verbose=${VERBOSE}"
  echo "smoke_cpu_backend=${SMOKE_CPU_BACKEND}"
  echo "trace_file=${TRACE_FILE_ENABLED}"
  echo "command=${APPDIR_PATH}/AppRun --intf dummy --vout dummy --play-and-exit --run-time=${RUN_TIME} --verbose=${VERBOSE} ${MEDIA_PATH} $*"
} | tee "${LOG_PATH}"

smoke_env=(
  QT_QPA_PLATFORM=offscreen
  OPEN3D_PROCESS_CPU_BACKEND="${SMOKE_CPU_BACKEND}"
)

command_prefix=()
if [[ "${TRACE_FILE_ENABLED}" == "1" ]] && command -v strace >/dev/null 2>&1; then
  command_prefix+=(strace -f -e trace=file -o "${TRACE_LOG_PATH}")
fi

"${command_prefix[@]}" env "${smoke_env[@]}" \
  "${APPDIR_PATH}/AppRun" \
  --intf dummy \
  --vout dummy \
  --play-and-exit \
  --run-time="${RUN_TIME}" \
  --verbose="${VERBOSE}" \
  "${MEDIA_PATH}" \
  "$@" 2>&1 | tee -a "${LOG_PATH}"

echo "Smoke log: ${LOG_PATH}"
if [[ -f "${TRACE_LOG_PATH}" ]]; then
  echo "Smoke trace: ${TRACE_LOG_PATH}"
fi
