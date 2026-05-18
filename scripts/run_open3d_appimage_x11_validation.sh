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
  OPEN3D_APPIMAGE_X11_CAPTURE_TOPLEVEL=0
  OPEN3D_APPIMAGE_X11_CAPTURE_INTERVAL=1.0
  OPEN3D_APPIMAGE_X11_CAPTURE_LIMIT=6
  OPEN3D_APPIMAGE_X11_POST_LAUNCH_HOOK=/path/to/hook.sh
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
CAPTURE_TOPLEVEL_ENABLED="${OPEN3D_APPIMAGE_X11_CAPTURE_TOPLEVEL:-0}"
CAPTURE_INTERVAL_SECONDS="${OPEN3D_APPIMAGE_X11_CAPTURE_INTERVAL:-1.0}"
CAPTURE_LIMIT="${OPEN3D_APPIMAGE_X11_CAPTURE_LIMIT:-6}"
POST_LAUNCH_HOOK="${OPEN3D_APPIMAGE_X11_POST_LAUNCH_HOOK:-}"
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
CAPTURE_DIR_PATH="${RUN_DIR}/window_captures"
CAPTURE_INDEX_PATH="${CAPTURE_DIR_PATH}/index.tsv"
HOOK_LOG_PATH="${RUN_DIR}/post_launch_hook.log"

CAPTURE_INTERVAL_MS="$(
  python3 - "${CAPTURE_INTERVAL_SECONDS}" <<'PY'
import sys
value = float(sys.argv[1])
if value < 0:
    value = 0.0
print(int(round(value * 1000.0)))
PY
)"

capture_window_snapshot() {
  if command -v xwininfo >/dev/null 2>&1; then
    xwininfo -root -tree 2>/dev/null || true
  fi
}

select_vlc_toplevel_from_snapshot() {
  python3 - "$1" <<'PY'
import re
import sys

best = None
pattern = re.compile(r'^\s*(0x[0-9A-Fa-f]+).*?([0-9]+)x([0-9]+)\+(-?[0-9]+)\+(-?[0-9]+)')

for line in sys.argv[1].splitlines():
    if 'VLC media player' not in line:
        continue
    match = pattern.search(line)
    if not match:
        continue
    window_id, width, height, rel_x, rel_y = match.groups()
    width = int(width)
    height = int(height)
    area = width * height
    candidate = (area, window_id, width, height, int(rel_x), int(rel_y), line)
    if best is None or candidate[0] > best[0]:
        best = candidate

if best is None:
    sys.exit(0)

_, window_id, width, height, rel_x, rel_y, line = best
print(f"{window_id}\t{width}\t{height}\t{rel_x}\t{rel_y}\t{line}")
PY
}

capture_toplevel_window() {
  local snapshot_text="$1"
  local elapsed_ms="$2"
  local capture_number="$3"
  local info window_id width height rel_x rel_y line
  local stem xwd_path png_path status detail err_path

  info="$(select_vlc_toplevel_from_snapshot "${snapshot_text}")"
  if [[ -z "${info}" ]]; then
    printf '%s\t%s\t-\t-\t-\t-\tselector_empty\t-\t-\n' \
      "${capture_number}" \
      "${elapsed_ms}" >>"${CAPTURE_INDEX_PATH}"
    return 1
  fi

  IFS=$'\t' read -r window_id width height rel_x rel_y line <<<"${info}"
  stem="$(printf 'capture_%03d' "${capture_number}")"
  xwd_path="${CAPTURE_DIR_PATH}/${stem}.xwd"
  png_path="${CAPTURE_DIR_PATH}/${stem}.png"
  err_path="$(mktemp)"

  if ! xwd -silent -id "${window_id}" -out "${xwd_path}" > /dev/null 2>"${err_path}"; then
    status="xwd_failed"
    detail="$(tr '\n' ' ' <"${err_path}" | sed -e 's/[[:space:]]\\+/ /g' -e 's/^ //; s/ $//')"
    printf '%s\t%s\t%s\t%s\t%s\t-\t%s\t%s\t%s\n' \
      "${capture_number}" \
      "${elapsed_ms}" \
      "${window_id}" \
      "${width}" \
      "${height}" \
      "${status}" \
      "${detail:-xwd_failed}" \
      "${line}" >>"${CAPTURE_INDEX_PATH}"
    rm -f "${err_path}" "${xwd_path}"
    return 1
  fi
  if ! convert "xwd:${xwd_path}" "${png_path}" > /dev/null 2>"${err_path}"; then
    status="convert_failed"
    detail="$(tr '\n' ' ' <"${err_path}" | sed -e 's/[[:space:]]\\+/ /g' -e 's/^ //; s/ $//')"
    printf '%s\t%s\t%s\t%s\t%s\t-\t%s\t%s\t%s\n' \
      "${capture_number}" \
      "${elapsed_ms}" \
      "${window_id}" \
      "${width}" \
      "${height}" \
      "${status}" \
      "${detail:-convert_failed}" \
      "${line}" >>"${CAPTURE_INDEX_PATH}"
    rm -f "${err_path}" "${xwd_path}" "${png_path}"
    return 1
  fi
  rm -f "${xwd_path}" "${err_path}"

  printf '%s\t%s\t%s\t%s\t%s\t%s\tok\t-\t%s\n' \
    "${capture_number}" \
    "${elapsed_ms}" \
    "${window_id}" \
    "${width}" \
    "${height}" \
    "${png_path}" \
    "${line}" >>"${CAPTURE_INDEX_PATH}"
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
  echo "capture_toplevel=${CAPTURE_TOPLEVEL_ENABLED}"
  echo "capture_interval_seconds=${CAPTURE_INTERVAL_SECONDS}"
  echo "capture_limit=${CAPTURE_LIMIT}"
  echo "post_launch_hook=${POST_LAUNCH_HOOK}"
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
capture_count=0
last_capture_ms=-1
run_start_ms="$(date +%s%3N)"
if [[ "${CAPTURE_TOPLEVEL_ENABLED}" == "1" ]]; then
  mkdir -p "${CAPTURE_DIR_PATH}"
  printf 'capture\telapsed_ms\twindow_id\twidth\theight\tpng_path\tstatus\tdetail\tsnapshot_line\n' >"${CAPTURE_INDEX_PATH}"
fi

"${command_prefix[@]}" env "${run_env[@]}" \
  "${APPIMAGE_PATH}" \
  "${vlc_args[@]}" > >(tee "${LAUNCHER_LOG_PATH}") 2>&1 &
app_pid=$!
timer_pid=""
hook_pid=""
helper_terminated=0
raw_exit_code=0

if [[ -n "${POST_LAUNCH_HOOK}" ]]; then
  env \
    OPEN3D_APPIMAGE_X11_RUN_DIR="${RUN_DIR}" \
    OPEN3D_APPIMAGE_X11_LAUNCHER_LOG="${LAUNCHER_LOG_PATH}" \
    OPEN3D_APPIMAGE_X11_WINDOWS_BEFORE="${WINDOWS_BEFORE_PATH}" \
    OPEN3D_APPIMAGE_X11_WINDOWS_DETECTED="${WINDOWS_DETECTED_PATH}" \
    OPEN3D_APPIMAGE_X11_CAPTURE_DIR="${CAPTURE_DIR_PATH}" \
    OPEN3D_APPIMAGE_X11_CAPTURE_INDEX="${CAPTURE_INDEX_PATH}" \
    OPEN3D_APPIMAGE_X11_APP_PID="${app_pid}" \
    OPEN3D_APPIMAGE_X11_APPIMAGE_PATH="${APPIMAGE_PATH}" \
    OPEN3D_APPIMAGE_X11_MEDIA_PATH="${MEDIA_PATH}" \
    OPEN3D_APPIMAGE_X11_COMMAND_PATH="${COMMAND_PATH}" \
    "${POST_LAUNCH_HOOK}" >"${HOOK_LOG_PATH}" 2>&1 &
  hook_pid=$!
fi

if [[ "${USE_VLC_BOUNDED_EXIT}" != "1" ]]; then
  sleep "${RUN_TIME}" &
  timer_pid=$!
fi

if [[ "${WINDOW_PROBE_ENABLED}" == "1" ]] && command -v xwininfo >/dev/null 2>&1; then
  while kill -0 "${app_pid}" 2>/dev/null; do
    current_windows="$(capture_window_snapshot)"
    record_detected_windows "${current_windows}"
    if [[ "${CAPTURE_TOPLEVEL_ENABLED}" == "1" && "${capture_count}" -lt "${CAPTURE_LIMIT}" ]]; then
      now_ms="$(date +%s%3N)"
      if [[ "${last_capture_ms}" -lt 0 || $((now_ms - last_capture_ms)) -ge "${CAPTURE_INTERVAL_MS}" ]]; then
        capture_toplevel_window "${current_windows}" "$((now_ms - run_start_ms))" "${capture_count}" || true
        capture_count=$((capture_count + 1))
        last_capture_ms="${now_ms}"
      fi
    fi
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
if [[ -n "${hook_pid}" ]]; then
  kill "${hook_pid}" 2>/dev/null || true
  wait "${hook_pid}" 2>/dev/null || true
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
if [[ -f "${CAPTURE_INDEX_PATH}" ]]; then
  echo "X11 validation top-level captures: ${CAPTURE_INDEX_PATH}"
fi
if [[ -f "${HOOK_LOG_PATH}" ]]; then
  echo "X11 validation post-launch hook log: ${HOOK_LOG_PATH}"
fi
if [[ -f "${TRACE_LOG_PATH}" ]]; then
  echo "X11 validation trace: ${TRACE_LOG_PATH}"
fi

exit "${exit_code}"
