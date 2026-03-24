#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_open3d_appimage_smoke.sh <media-path-or-url> [vlc args...]

Runs the latest exported Open3DOLED AppImage on the host in a bounded headless
smoke mode so the packaged artifact can be checked outside Docker.

Env overrides:
  OPEN3D_APPIMAGE_PATH=/path/to/Open3DOLED-VLC-x86_64.AppImage
  OPEN3D_APPIMAGE_SMOKE_RUN_TIME=5
  OPEN3D_APPIMAGE_SMOKE_VERBOSE=2
  OPEN3D_APPIMAGE_SMOKE_CPU_BACKEND=taskset
  OPEN3D_APPIMAGE_SMOKE_TRACE_FILE=1
  OPEN3D_APPIMAGE_SMOKE_EXTRACT_AND_RUN=1
EOF
}

if [[ $# -lt 1 ]]; then
  usage >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

discover_latest_appimage() {
  find "${REPO_DIR}/local/out/appimage/appimage-builds" \
    -mindepth 2 -maxdepth 2 -type f -name '*.AppImage' \
    -printf '%T@ %p\n' 2>/dev/null | sort -nr | awk 'NR==1 {print $2}'
}

APPIMAGE_PATH="${OPEN3D_APPIMAGE_PATH:-}"
if [[ -z "${APPIMAGE_PATH}" ]]; then
  APPIMAGE_PATH="$(discover_latest_appimage || true)"
fi

if [[ -z "${APPIMAGE_PATH}" || ! -f "${APPIMAGE_PATH}" ]]; then
  echo "Error: AppImage not found. Build one first or set OPEN3D_APPIMAGE_PATH." >&2
  exit 2
fi

MEDIA_PATH="$1"
shift

RUN_TIME="${OPEN3D_APPIMAGE_SMOKE_RUN_TIME:-5}"
VERBOSE="${OPEN3D_APPIMAGE_SMOKE_VERBOSE:-2}"
SMOKE_CPU_BACKEND="${OPEN3D_APPIMAGE_SMOKE_CPU_BACKEND:-taskset}"
TRACE_FILE_ENABLED="${OPEN3D_APPIMAGE_SMOKE_TRACE_FILE:-1}"
EXTRACT_AND_RUN="${OPEN3D_APPIMAGE_SMOKE_EXTRACT_AND_RUN:-1}"
RUN_STAMP="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="${REPO_DIR}/local/out/appimage/appimage-smoke/${RUN_STAMP}"
LOG_PATH="${RUN_DIR}/smoke.log"
TRACE_LOG_PATH="${RUN_DIR}/smoke.strace.log"

mkdir -p "${RUN_DIR}"

{
  echo "Open3DOLED AppImage host smoke"
  echo "appimage=${APPIMAGE_PATH}"
  echo "media=${MEDIA_PATH}"
  echo "run_time=${RUN_TIME}"
  echo "verbose=${VERBOSE}"
  echo "smoke_cpu_backend=${SMOKE_CPU_BACKEND}"
  echo "trace_file=${TRACE_FILE_ENABLED}"
  echo "extract_and_run=${EXTRACT_AND_RUN}"
  echo "command=${APPIMAGE_PATH} --intf dummy --vout dummy --play-and-exit --run-time=${RUN_TIME} --verbose=${VERBOSE} ${MEDIA_PATH} $*"
} | tee "${LOG_PATH}"

smoke_env=(
  APPIMAGELAUNCHER_DISABLE=1
  QT_QPA_PLATFORM=offscreen
  OPEN3D_PROCESS_CPU_BACKEND="${SMOKE_CPU_BACKEND}"
)
if [[ "${EXTRACT_AND_RUN}" == "1" ]]; then
  smoke_env+=(APPIMAGE_EXTRACT_AND_RUN=1)
fi

command_prefix=()
if [[ "${TRACE_FILE_ENABLED}" == "1" ]] && command -v strace >/dev/null 2>&1; then
  command_prefix+=(strace -f -e trace=file -o "${TRACE_LOG_PATH}")
fi

"${command_prefix[@]}" env "${smoke_env[@]}" \
  "${APPIMAGE_PATH}" \
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
