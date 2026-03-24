#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_open3d_appimage_x11_validation.sh <media-path-or-url> [vlc args...]

Runs the latest exported Open3DOLED AppImage on the current X11 desktop session
for a bounded real-display validation pass. This is intended for packaged
playback parity checks where the video output should use the live X server
instead of the offscreen smoke path.

Env overrides:
  OPEN3D_APPIMAGE_PATH=/path/to/Open3DOLED-VLC-x86_64.AppImage
  OPEN3D_APPIMAGE_X11_RUN_TIME=8
  OPEN3D_APPIMAGE_X11_VERBOSE=2
  OPEN3D_APPIMAGE_X11_CPU_BACKEND=taskset
  OPEN3D_APPIMAGE_X11_TRACE_FILE=0
  OPEN3D_APPIMAGE_X11_EXTRACT_AND_RUN=1
  OPEN3D_APPIMAGE_X11_DUMMY_INTF=1
  OPEN3D_APPIMAGE_X11_DEBUG_STATUS=1
  OPEN3D_APPIMAGE_X11_USE_VLC_BOUNDED_EXIT=0
  OPEN3D_APPIMAGE_X11_REQUIRE_X11=1
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

if [[ -z "${DISPLAY:-}" ]]; then
  echo "Error: DISPLAY is not set; X11 validation requires a live X display." >&2
  exit 3
fi

REQUIRE_X11="${OPEN3D_APPIMAGE_X11_REQUIRE_X11:-1}"
if [[ "${REQUIRE_X11}" == "1" && -n "${XDG_SESSION_TYPE:-}" && "${XDG_SESSION_TYPE}" != "x11" ]]; then
  echo "Error: XDG_SESSION_TYPE=${XDG_SESSION_TYPE}; this helper expects an X11 session." >&2
  exit 4
fi

MEDIA_PATH="$1"
shift

RUN_TIME="${OPEN3D_APPIMAGE_X11_RUN_TIME:-8}"
VERBOSE="${OPEN3D_APPIMAGE_X11_VERBOSE:-2}"
CPU_BACKEND="${OPEN3D_APPIMAGE_X11_CPU_BACKEND:-taskset}"
TRACE_FILE_ENABLED="${OPEN3D_APPIMAGE_X11_TRACE_FILE:-0}"
EXTRACT_AND_RUN="${OPEN3D_APPIMAGE_X11_EXTRACT_AND_RUN:-1}"
DUMMY_INTF="${OPEN3D_APPIMAGE_X11_DUMMY_INTF:-1}"
DEBUG_STATUS="${OPEN3D_APPIMAGE_X11_DEBUG_STATUS:-1}"
USE_VLC_BOUNDED_EXIT="${OPEN3D_APPIMAGE_X11_USE_VLC_BOUNDED_EXIT:-0}"
WINDOW_PROBE_ENABLED="${OPEN3D_APPIMAGE_X11_WINDOW_PROBE:-1}"
WINDOW_POLL_SECONDS="${OPEN3D_APPIMAGE_X11_WINDOW_POLL_SECONDS:-0.2}"
RUN_STAMP="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="${REPO_DIR}/local/out/appimage/appimage-x11-validation/${RUN_STAMP}"
LAUNCHER_LOG_PATH="${RUN_DIR}/launcher.log"
TRACE_LOG_PATH="${RUN_DIR}/playback.strace.log"
ENV_LOG_PATH="${RUN_DIR}/session_env.txt"
COMMAND_PATH="${RUN_DIR}/command.sh"
STATUS_PATH="${RUN_DIR}/status.txt"
WINDOWS_BEFORE_PATH="${RUN_DIR}/windows_before.txt"
WINDOWS_AFTER_PATH="${RUN_DIR}/windows_after.txt"
WINDOWS_DETECTED_PATH="${RUN_DIR}/windows_detected.txt"

capture_window_snapshot() {
  if command -v xwininfo >/dev/null 2>&1; then
    xwininfo -root -tree 2>/dev/null || true
  fi
}

record_detected_windows() {
  local snapshot_text="$1"
  local line window_id

  while IFS= read -r line; do
    [[ -n "${line}" ]] || continue
    window_id="$(sed -n 's/^[[:space:]]*\(0x[0-9a-fA-F]\+\).*/\1/p' <<<"${line}")"
    [[ -n "${window_id}" ]] || continue
    if ! grep -q "${window_id}" "${WINDOWS_BEFORE_PATH}" 2>/dev/null &&
       ! grep -q "${window_id}" "${WINDOWS_DETECTED_PATH}" 2>/dev/null; then
      printf '%s\n' "${line}" >>"${WINDOWS_DETECTED_PATH}"
    fi
  done <<<"${snapshot_text}"

  while IFS= read -r line; do
    [[ -n "${line}" ]] || continue
    if ! grep -F -q -- "${line}" "${WINDOWS_DETECTED_PATH}" 2>/dev/null; then
      printf '%s\n' "${line}" >>"${WINDOWS_DETECTED_PATH}"
    fi
  done < <(rg -e 'VLC media player' -e '\("vlc"' -e 'Qt Selection Owner for vlc' <<<"${snapshot_text}" || true)
}

terminate_app() {
  local pid="$1"
  local grace_loops=10

  kill "${pid}" 2>/dev/null || true
  for ((i = 0; i < grace_loops; ++i)); do
    if ! kill -0 "${pid}" 2>/dev/null; then
      return 0
    fi
    sleep 0.2
  done
  kill -9 "${pid}" 2>/dev/null || true
}

mkdir -p "${RUN_DIR}"

vlc_args=()
if [[ "${DUMMY_INTF}" == "1" ]]; then
  vlc_args+=(--intf dummy)
fi
if [[ "${USE_VLC_BOUNDED_EXIT}" == "1" ]]; then
  vlc_args+=(
    --play-and-exit
    "--run-time=${RUN_TIME}"
  )
fi
vlc_args+=("--verbose=${VERBOSE}")
if [[ "${DEBUG_STATUS}" == "1" ]]; then
  vlc_args+=(--open3d-debug-status)
fi
vlc_args+=("${MEDIA_PATH}")
vlc_args+=("$@")

{
  echo "Open3DOLED AppImage X11 validation"
  echo "appimage=${APPIMAGE_PATH}"
  echo "media=${MEDIA_PATH}"
  echo "run_time=${RUN_TIME}"
  echo "verbose=${VERBOSE}"
  echo "cpu_backend=${CPU_BACKEND}"
  echo "extract_and_run=${EXTRACT_AND_RUN}"
  echo "dummy_intf=${DUMMY_INTF}"
  echo "debug_status=${DEBUG_STATUS}"
  echo "use_vlc_bounded_exit=${USE_VLC_BOUNDED_EXIT}"
  echo "DISPLAY=${DISPLAY:-}"
  echo "XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-}"
  echo "WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}"
} | tee "${ENV_LOG_PATH}"

{
  printf '#!/usr/bin/env bash\n'
  printf 'set -euo pipefail\n'
  printf 'cd %q\n' "${REPO_DIR}"
  printf 'APPIMAGELAUNCHER_DISABLE=1 '
  if [[ "${EXTRACT_AND_RUN}" == "1" ]]; then
    printf 'APPIMAGE_EXTRACT_AND_RUN=1 '
  fi
  printf 'OPEN3D_PROCESS_CPU_BACKEND=%q ' "${CPU_BACKEND}"
  printf '%q' "${APPIMAGE_PATH}"
  printf ' '
  printf '%q ' "${vlc_args[@]}"
  printf '\n'
} >"${COMMAND_PATH}"
chmod +x "${COMMAND_PATH}"

command_prefix=()
if [[ "${TRACE_FILE_ENABLED}" == "1" ]] && command -v strace >/dev/null 2>&1; then
  command_prefix+=(strace -f -e trace=file,network,process -o "${TRACE_LOG_PATH}")
fi

run_env=(
  APPIMAGELAUNCHER_DISABLE=1
  OPEN3D_PROCESS_CPU_BACKEND="${CPU_BACKEND}"
)
if [[ "${EXTRACT_AND_RUN}" == "1" ]]; then
  run_env+=(APPIMAGE_EXTRACT_AND_RUN=1)
fi

{
  echo "command=${COMMAND_PATH}"
  printf 'argv='
  printf '%q ' "${APPIMAGE_PATH}" "${vlc_args[@]}"
  printf '\n'
} | tee -a "${ENV_LOG_PATH}"

if [[ "${WINDOW_PROBE_ENABLED}" == "1" ]] && command -v xwininfo >/dev/null 2>&1; then
  capture_window_snapshot >"${WINDOWS_BEFORE_PATH}"
fi
: >"${WINDOWS_DETECTED_PATH}"

"${command_prefix[@]}" env "${run_env[@]}" \
  "${APPIMAGE_PATH}" \
  "${vlc_args[@]}" > >(tee "${LAUNCHER_LOG_PATH}") 2>&1 &
app_pid=$!
timer_pid=""
helper_terminated=0
raw_exit_code=0

if [[ "${USE_VLC_BOUNDED_EXIT}" != "1" ]]; then
  sleep "${RUN_TIME}" &
  timer_pid=$!
fi

if [[ "${WINDOW_PROBE_ENABLED}" == "1" ]] && command -v xwininfo >/dev/null 2>&1; then
  while kill -0 "${app_pid}" 2>/dev/null; do
    current_windows="$(capture_window_snapshot)"
    record_detected_windows "${current_windows}"
    if [[ -n "${timer_pid}" ]] && ! kill -0 "${timer_pid}" 2>/dev/null; then
      helper_terminated=1
      terminate_app "${app_pid}"
      break
    fi
    sleep "${WINDOW_POLL_SECONDS}"
  done
elif [[ -n "${timer_pid}" ]]; then
  while kill -0 "${app_pid}" 2>/dev/null; do
    if ! kill -0 "${timer_pid}" 2>/dev/null; then
      helper_terminated=1
      terminate_app "${app_pid}"
      break
    fi
    sleep "${WINDOW_POLL_SECONDS}"
  done
fi

if [[ -n "${timer_pid}" ]]; then
  kill "${timer_pid}" 2>/dev/null || true
  wait "${timer_pid}" 2>/dev/null || true
fi

set +e
wait "${app_pid}"
exit_code=$?
set -e
raw_exit_code="${exit_code}"
if [[ "${helper_terminated}" == "1" ]]; then
  exit_code=0
fi

if [[ "${WINDOW_PROBE_ENABLED}" == "1" ]] && command -v xwininfo >/dev/null 2>&1; then
  capture_window_snapshot >"${WINDOWS_AFTER_PATH}"
fi

{
  printf 'exit_code=%s\n' "${exit_code}"
  printf 'raw_exit_code=%s\n' "${raw_exit_code}"
  printf 'helper_terminated=%s\n' "${helper_terminated}"
  printf 'use_vlc_bounded_exit=%s\n' "${USE_VLC_BOUNDED_EXIT}"
} >"${STATUS_PATH}"

echo "X11 validation env: ${ENV_LOG_PATH}"
echo "X11 validation launcher log: ${LAUNCHER_LOG_PATH}"
echo "X11 validation command: ${COMMAND_PATH}"
echo "X11 validation status: ${STATUS_PATH}"
if [[ -f "${WINDOWS_DETECTED_PATH}" ]]; then
  echo "X11 validation detected windows: ${WINDOWS_DETECTED_PATH}"
fi
if [[ -f "${TRACE_LOG_PATH}" ]]; then
  echo "X11 validation trace: ${TRACE_LOG_PATH}"
fi

exit "${exit_code}"
