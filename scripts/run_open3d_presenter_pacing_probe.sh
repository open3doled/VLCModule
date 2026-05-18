#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_open3d_presenter_pacing_probe.sh [media-path] [vlc args...]

Runs a bounded presenter pacing probe and summarizes Open3D page-flip burst
windows from the VLC log. X11 window automation is skipped for native Wayland.

Env overrides:
  OPEN3D_PACING_APPIMAGE=/path/to/Open3DOLED-VLC.AppImage
  OPEN3D_PACING_RUN_TIME=20
  OPEN3D_PACING_VERBOSE=2
  OPEN3D_PACING_LAYOUT=sbs-full
  OPEN3D_PACING_INTF=qt
  OPEN3D_PACING_PRESENTER_HZ=120
  OPEN3D_PACING_TARGET_FLIP_HZ=120
  OPEN3D_PACING_FULLSCREEN=1
  OPEN3D_PACING_PAUSE_AFTER=4
  OPEN3D_PACING_SETTLE_AFTER_PAUSE=4
  OPEN3D_PACING_ANALYZE_AFTER=8
  OPEN3D_PACING_CPU_BACKEND=taskset
  OPEN3D_PACING_EXTRACT_AND_RUN=1
  OPEN3D_PACING_ENABLE_WAYLAND=auto
  OPEN3D_PACING_QT_PLATFORM=wayland|xcb
  OPEN3D_PACING_NATIVE_WAYLAND=0|1|auto
  OPEN3D_PACING_XLIB=1
  OPEN3D_PACING_PREPARED_GPU_ENABLE=0|1|stored
  OPEN3D_PACING_BFI_ENABLE=0|1|stored
  OPEN3D_PACING_BFI_VISIBLE_FRAMES=1
  OPEN3D_PACING_BFI_BLACK_FRAMES=1
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_MEDIA="${REPO_DIR}/../3DPlayer/videos/ghosting_test_video_1080p_red_left_blue_right_side_by_side_full.mp4"

discover_latest_appimage() {
  find "${REPO_DIR}/local/out/appimage/appimage-builds" \
    -mindepth 2 -maxdepth 2 -type f -name '*.AppImage' \
    -printf '%T@ %p\n' 2>/dev/null | sort -nr | awk 'NR==1 {print $2}'
}

APPIMAGE_PATH="${OPEN3D_PACING_APPIMAGE:-}"
if [[ -z "${APPIMAGE_PATH}" ]]; then
  APPIMAGE_PATH="$(discover_latest_appimage || true)"
fi
if [[ -z "${APPIMAGE_PATH}" || ! -f "${APPIMAGE_PATH}" ]]; then
  echo "Error: AppImage not found. Build one first or set OPEN3D_PACING_APPIMAGE." >&2
  exit 2
fi

ENABLE_WAYLAND="${OPEN3D_PACING_ENABLE_WAYLAND:-auto}"
QT_PLATFORM="${OPEN3D_PACING_QT_PLATFORM:-}"
NATIVE_WAYLAND="${OPEN3D_PACING_NATIVE_WAYLAND:-auto}"
if [[ "${NATIVE_WAYLAND}" == "auto" ]]; then
  if [[ "${QT_PLATFORM}" == wayland* ||
        ( "${ENABLE_WAYLAND}" != "0" &&
          "${XDG_SESSION_TYPE:-}" == "wayland" &&
          -n "${WAYLAND_DISPLAY:-}" ) ]]; then
    NATIVE_WAYLAND=1
  else
    NATIVE_WAYLAND=0
  fi
fi

if [[ -z "${DISPLAY:-}" && "${NATIVE_WAYLAND}" != "1" ]]; then
  echo "Error: DISPLAY is not set; X11 presenter pacing probes need a live X display." >&2
  exit 3
fi

MEDIA_PATH="${1:-${DEFAULT_MEDIA}}"
if [[ $# -gt 0 ]]; then
  shift
fi
if [[ ! -e "${MEDIA_PATH}" && "${MEDIA_PATH}" != *"://"* ]]; then
  echo "Error: media path not found: ${MEDIA_PATH}" >&2
  exit 4
fi

RUN_TIME="${OPEN3D_PACING_RUN_TIME:-20}"
VERBOSE="${OPEN3D_PACING_VERBOSE:-2}"
LAYOUT="${OPEN3D_PACING_LAYOUT:-sbs-full}"
INTF="${OPEN3D_PACING_INTF:-qt}"
PRESENTER_HZ="${OPEN3D_PACING_PRESENTER_HZ:-120}"
TARGET_FLIP_HZ="${OPEN3D_PACING_TARGET_FLIP_HZ:-120}"
FULLSCREEN="${OPEN3D_PACING_FULLSCREEN:-1}"
PAUSE_AFTER="${OPEN3D_PACING_PAUSE_AFTER:-4}"
SETTLE_AFTER_PAUSE="${OPEN3D_PACING_SETTLE_AFTER_PAUSE:-4}"
ANALYZE_AFTER="${OPEN3D_PACING_ANALYZE_AFTER:-8}"
CPU_BACKEND="${OPEN3D_PACING_CPU_BACKEND:-taskset}"
EXTRACT_AND_RUN="${OPEN3D_PACING_EXTRACT_AND_RUN:-1}"
TELEMETRY_INTERVAL="${OPEN3D_PACING_TELEMETRY_INTERVAL:-120}"
XLIB_ENABLED="${OPEN3D_PACING_XLIB:-1}"
BFI_ENABLE="${OPEN3D_PACING_BFI_ENABLE:-0}"
PREPARED_GPU_ENABLE="${OPEN3D_PACING_PREPARED_GPU_ENABLE:-0}"
RUN_STAMP="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="${REPO_DIR}/local/out/appimage/presenter-pacing-probe/${RUN_STAMP}"
LOG_PATH="${RUN_DIR}/launcher.log"
SUMMARY_JSON="${RUN_DIR}/pacing_summary.json"
WINDOWS_TSV="${RUN_DIR}/pacing_windows.tsv"
STAGE_SUMMARY_JSON="${RUN_DIR}/stage_profile_summary.json"
STAGE_TSV="${RUN_DIR}/stage_profiles.tsv"
COMMAND_PATH="${RUN_DIR}/command.sh"
STATUS_PATH="${RUN_DIR}/status.txt"
ACTIONS_LOG="${RUN_DIR}/actions.log"

mkdir -p "${RUN_DIR}"

descendant_pids() {
  local root_pid="$1"
  local -a queue=("${root_pid}")
  local -a all=()
  local pid
  local -a kids=()

  while (( ${#queue[@]} > 0 )); do
    pid="${queue[0]}"
    queue=("${queue[@]:1}")
    all+=("${pid}")
    mapfile -t kids < <(pgrep -P "${pid}" 2>/dev/null || true)
    if (( ${#kids[@]} > 0 )); then
      queue+=("${kids[@]}")
    fi
  done

  printf '%s\n' "${all[@]}"
}

find_vlc_window() {
  local root_pid="$1"
  local -A wanted_pids=()
  local pid
  local line win_hex desktop line_pid x y w h host title

  while IFS= read -r pid; do
    [[ -n "${pid}" ]] && wanted_pids["${pid}"]=1
  done < <(descendant_pids "${root_pid}")

  while IFS= read -r line; do
    [[ -n "${line}" ]] || continue
    read -r win_hex desktop line_pid x y w h host title <<<"${line}"
    [[ "${win_hex}" == 0x* ]] || continue
    [[ -n "${wanted_pids[${line_pid}]+x}" ]] || continue
    case "${title}" in
      *"VLC media player"*|*"ghosting_test_video"*|*"Open3D"*)
        printf '%s\t%s\t%s\n' "${win_hex}" "$((win_hex))" "${line_pid}"
        return 0
        ;;
    esac
  done < <(wmctrl -lpG 2>/dev/null || true)

  return 1
}

wait_for_window() {
  local root_pid="$1"
  local attempts=0
  local window_info

  while (( attempts < 40 )); do
    if ! kill -0 "${root_pid}" 2>/dev/null; then
      return 1
    fi
    if window_info="$(find_vlc_window "${root_pid}")"; then
      printf '%s\n' "${window_info}"
      return 0
    fi
    sleep 0.25
    attempts=$((attempts + 1))
  done

  return 1
}

float_gt_zero() {
  python3 - "$1" <<'PY'
import sys
try:
    value = float(sys.argv[1])
except Exception:
    value = 0.0
raise SystemExit(0 if value > 0.0 else 1)
PY
}

remaining_sleep_seconds() {
  python3 - "$1" "$2" <<'PY'
import sys
run_time = float(sys.argv[1])
elapsed = float(sys.argv[2])
remaining = max(0.0, run_time - elapsed)
print(f"{remaining:.3f}")
PY
}

vlc_args=(
  "--intf=${INTF}"
  --extraintf=
  "--verbose=${VERBOSE}"
  --no-qt-privacy-ask
  --no-metadata-network-access
  --no-video-title-show
  --open3d-enable
  "--open3d-layout=${LAYOUT}"
  --open3d-presenter-telemetry
  "--open3d-presenter-telemetry-interval=${TELEMETRY_INTERVAL}"
)

if [[ "${FULLSCREEN}" == "1" ]]; then
  vlc_args+=(--fullscreen)
fi
if [[ "${XLIB_ENABLED}" != "1" ]]; then
  # Dummy-interface probes do not load Qt's normal XInitThreads path.
  vlc_args+=(--no-xlib)
fi

if [[ -n "${PRESENTER_HZ}" && "${PRESENTER_HZ}" != "stored" ]]; then
  vlc_args+=("--open3d-presenter-hz=${PRESENTER_HZ}")
fi
if [[ -n "${TARGET_FLIP_HZ}" && "${TARGET_FLIP_HZ}" != "stored" ]]; then
  vlc_args+=("--open3d-target-flip-hz=${TARGET_FLIP_HZ}")
fi
if [[ -n "${OPEN3D_PACING_PRESENTER_LEAD_US:-}" ]]; then
  vlc_args+=("--open3d-presenter-lead-us=${OPEN3D_PACING_PRESENTER_LEAD_US}")
fi
if [[ "${OPEN3D_PACING_DEBUG_STATUS:-0}" == "1" ]]; then
  vlc_args+=(--open3d-debug-status)
fi
if [[ "${OPEN3D_PACING_STAGE_PROFILE:-0}" == "1" ]]; then
  vlc_args+=(--open3d-presenter-stage-profile)
fi
if [[ "${PREPARED_GPU_ENABLE}" == "1" ]]; then
  vlc_args+=(--open3d-presenter-prepared-gpu-enable)
elif [[ "${PREPARED_GPU_ENABLE}" != "stored" ]]; then
  vlc_args+=(--no-open3d-presenter-prepared-gpu-enable)
fi
if [[ "${BFI_ENABLE}" == "1" ]]; then
  vlc_args+=(--open3d-bfi-enable)
elif [[ "${BFI_ENABLE}" != "stored" ]]; then
  vlc_args+=(--no-open3d-bfi-enable)
fi
if [[ -n "${OPEN3D_PACING_BFI_VISIBLE_FRAMES:-}" ]]; then
  vlc_args+=("--open3d-bfi-visible-frames=${OPEN3D_PACING_BFI_VISIBLE_FRAMES}")
fi
if [[ -n "${OPEN3D_PACING_BFI_BLACK_FRAMES:-}" ]]; then
  vlc_args+=("--open3d-bfi-black-frames=${OPEN3D_PACING_BFI_BLACK_FRAMES}")
fi

vlc_args+=("${MEDIA_PATH}")
vlc_args+=("$@")

run_env=(
  APPIMAGELAUNCHER_DISABLE=1
  OPEN3D_APPIMAGE_ENABLE_WAYLAND="${ENABLE_WAYLAND}"
  OPEN3D_PROCESS_CPU_BACKEND="${CPU_BACKEND}"
)
if [[ -n "${QT_PLATFORM}" ]]; then
  run_env+=(QT_QPA_PLATFORM="${QT_PLATFORM}")
fi
if [[ "${EXTRACT_AND_RUN}" == "1" ]]; then
  run_env+=(APPIMAGE_EXTRACT_AND_RUN=1)
fi

{
  printf '#!/usr/bin/env bash\n'
  printf 'set -euo pipefail\n'
  printf 'cd %q\n' "${REPO_DIR}"
  printf 'env '
  printf '%q ' "${run_env[@]}"
  printf '%q ' "${APPIMAGE_PATH}"
  printf '%q ' "${vlc_args[@]}"
  printf '\n'
} >"${COMMAND_PATH}"
chmod +x "${COMMAND_PATH}"

{
  echo "Open3D presenter pacing probe"
  echo "run_dir=${RUN_DIR}"
  echo "appimage=${APPIMAGE_PATH}"
  echo "media=${MEDIA_PATH}"
  echo "run_time=${RUN_TIME}"
  echo "layout=${LAYOUT}"
  echo "intf=${INTF}"
  echo "presenter_hz=${PRESENTER_HZ}"
  echo "target_flip_hz=${TARGET_FLIP_HZ}"
  echo "fullscreen=${FULLSCREEN}"
  echo "pause_after=${PAUSE_AFTER}"
  echo "settle_after_pause=${SETTLE_AFTER_PAUSE}"
  echo "analyze_after=${ANALYZE_AFTER}"
  echo "enable_wayland=${ENABLE_WAYLAND}"
  echo "qt_platform=${QT_PLATFORM:-auto}"
  echo "native_wayland=${NATIVE_WAYLAND}"
  echo "xlib=${XLIB_ENABLED}"
  echo "prepared_gpu_enable=${PREPARED_GPU_ENABLE}"
  echo "bfi_enable=${BFI_ENABLE}"
  echo "bfi_visible_frames=${OPEN3D_PACING_BFI_VISIBLE_FRAMES:-1}"
  echo "bfi_black_frames=${OPEN3D_PACING_BFI_BLACK_FRAMES:-1}"
  echo "command=${COMMAND_PATH}"
} | tee "${RUN_DIR}/probe_env.txt"

set +e
probe_start_epoch="$(python3 - <<'PY'
import time
print(f"{time.monotonic():.6f}")
PY
)"
env "${run_env[@]}" "${APPIMAGE_PATH}" "${vlc_args[@]}" >"${LOG_PATH}" 2>&1 &
app_pid=$!

if [[ "${NATIVE_WAYLAND}" == "1" ]]; then
  {
    printf '[%s] native_wayland=1 x11_window_actions=skipped fullscreen_arg=%s pause_after=%s\n' \
      "$(date +%H:%M:%S)" "${FULLSCREEN}" "${PAUSE_AFTER}"
  } >>"${ACTIONS_LOG}"
elif [[ "${FULLSCREEN}" == "1" ]] || float_gt_zero "${PAUSE_AFTER}"; then
  if window_info="$(wait_for_window "${app_pid}")"; then
    IFS=$'\t' read -r win_hex win_id win_pid <<<"${window_info}"
    {
      printf '[%s] window_found id=%s hex=%s pid=%s\n' "$(date +%H:%M:%S)" "${win_id}" "${win_hex}" "${win_pid}"
    } >>"${ACTIONS_LOG}"
    if [[ "${FULLSCREEN}" == "1" ]]; then
      if command -v wmctrl >/dev/null 2>&1; then
        wmctrl -ir "${win_hex}" -b add,fullscreen >/dev/null 2>&1 || true
      fi
      if command -v xdotool >/dev/null 2>&1; then
        xdotool windowactivate --sync "${win_id}" >/dev/null 2>&1 || true
      fi
      printf '[%s] fullscreen_requested=1\n' "$(date +%H:%M:%S)" >>"${ACTIONS_LOG}"
    fi
    if float_gt_zero "${PAUSE_AFTER}"; then
      sleep "${PAUSE_AFTER}"
      if command -v xdotool >/dev/null 2>&1; then
        xdotool windowactivate --sync "${win_id}" >/dev/null 2>&1 || true
        xdotool key --window "${win_id}" space >/dev/null 2>&1 || true
        printf '[%s] pause_toggle_sent=1\n' "$(date +%H:%M:%S)" >>"${ACTIONS_LOG}"
      else
        printf '[%s] pause_toggle_sent=0 reason=missing_xdotool\n' "$(date +%H:%M:%S)" >>"${ACTIONS_LOG}"
      fi
      if float_gt_zero "${SETTLE_AFTER_PAUSE}"; then
        sleep "${SETTLE_AFTER_PAUSE}"
      fi
    fi
  else
    printf '[%s] window_found=0\n' "$(date +%H:%M:%S)" >>"${ACTIONS_LOG}"
  fi
fi

probe_now_epoch="$(python3 - <<'PY'
import time
print(f"{time.monotonic():.6f}")
PY
)"
elapsed_since_launch="$(python3 - "${probe_start_epoch}" "${probe_now_epoch}" <<'PY'
import sys
print(f"{float(sys.argv[2]) - float(sys.argv[1]):.3f}")
PY
)"
sleep "$(remaining_sleep_seconds "${RUN_TIME}" "${elapsed_since_launch}")"
helper_terminated=0
if kill -0 "${app_pid}" 2>/dev/null; then
  helper_terminated=1
  kill "${app_pid}" 2>/dev/null || true
  for _ in {1..10}; do
    kill -0 "${app_pid}" 2>/dev/null || break
    sleep 0.2
  done
  kill -9 "${app_pid}" 2>/dev/null || true
fi
wait "${app_pid}" 2>/dev/null
raw_exit_code=$?
set -e

python3 - "${LOG_PATH}" "${SUMMARY_JSON}" "${WINDOWS_TSV}" "${ANALYZE_AFTER}" <<'PY'
import json
import re
import sys

log_path, summary_path, tsv_path, analyze_after = sys.argv[1:5]
try:
    analyze_after_ms = int(round(float(analyze_after) * 1000.0))
except Exception:
    analyze_after_ms = 0
line_re = re.compile(r"open3d presenter pacing window (?P<body>.*)")
kv_re = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
numeric_keys = {
    "elapsed_ms", "ticks", "period_us", "render_avg_us", "render_max_us",
    "render_over_budget", "render_over_budget_max_us", "wake_late",
    "wake_gt_half_period", "wake_gt_period", "wake_max_us",
    "deadline_miss", "deadline_steps", "deadline_max_us",
    "flip_late", "flip_steps", "target_divider", "target_wait_ticks", "target_flip_ticks",
    "target_flip_steps", "repeat_skip_ticks", "no_frame_ticks", "bfi_black", "cpu",
}

windows = []
capture_ms = 0
with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
    for line_no, line in enumerate(fh, 1):
        match = line_re.search(line)
        if not match:
            continue
        row = {
            "line": line_no,
            "window_index": len(windows),
            "capture_ms_start": capture_ms,
        }
        for key, value in kv_re.findall(match.group("body")):
            if key in numeric_keys:
                try:
                    row[key] = int(value)
                except ValueError:
                    row[key] = value
            else:
                row[key] = value
        capture_ms += int(row.get("elapsed_ms", 0) or 0)
        row["capture_ms_end"] = capture_ms
        windows.append(row)

analyzed_windows = [
    row for row in windows
    if int(row.get("capture_ms_end", 0) or 0) >= analyze_after_ms
]

def total(key: str) -> int:
    return sum(int(row.get(key, 0) or 0) for row in analyzed_windows)

def maximum(key: str) -> int:
    values = [int(row.get(key, 0) or 0) for row in analyzed_windows]
    return max(values) if values else 0

def score(row: dict) -> int:
    return (
        int(row.get("deadline_steps", 0) or 0) * 1000
        + int(row.get("flip_steps", 0) or 0) * 100
        + int(row.get("render_over_budget", 0) or 0) * 25
        + int(row.get("wake_gt_period", 0) or 0) * 10
        + int(row.get("wake_gt_half_period", 0) or 0)
    )

burst_windows = [row for row in analyzed_windows if row.get("status") == "burst"]
summary = {
    "all_windows": len(windows),
    "analyze_after_ms": analyze_after_ms,
    "skipped_windows": len(windows) - len(analyzed_windows),
    "windows": len(analyzed_windows),
    "burst_windows": len(burst_windows),
    "stable_windows": len(analyzed_windows) - len(burst_windows),
    "deadline_miss_total": total("deadline_miss"),
    "deadline_steps_total": total("deadline_steps"),
    "flip_late_total": total("flip_late"),
    "flip_steps_total": total("flip_steps"),
    "target_wait_ticks_total": total("target_wait_ticks"),
    "target_flip_ticks_total": total("target_flip_ticks"),
    "target_flip_steps_total": total("target_flip_steps"),
    "repeat_skip_ticks_total": total("repeat_skip_ticks"),
    "render_over_budget_total": total("render_over_budget"),
    "wake_gt_period_total": total("wake_gt_period"),
    "wake_gt_half_period_total": total("wake_gt_half_period"),
    "no_frame_ticks_total": total("no_frame_ticks"),
    "bfi_black_total": total("bfi_black"),
    "deadline_max_us": maximum("deadline_max_us"),
    "render_max_us": maximum("render_max_us"),
    "render_over_budget_max_us": maximum("render_over_budget_max_us"),
    "wake_max_us": maximum("wake_max_us"),
    "worst_windows": sorted(analyzed_windows, key=score, reverse=True)[:10],
}

with open(summary_path, "w", encoding="utf-8") as fh:
    json.dump(summary, fh, indent=2, sort_keys=True)
    fh.write("\n")

columns = [
    "window_index", "line", "capture_ms_start", "capture_ms_end",
    "status", "elapsed_ms", "ticks", "period_us", "flip_mode",
    "presenter_hz", "target_hz", "target_divider",
    "deadline_miss", "deadline_steps", "deadline_max_us",
    "flip_late", "flip_steps", "target_wait_ticks", "target_flip_ticks",
    "target_flip_steps", "repeat_skip_ticks", "render_over_budget",
    "render_max_us", "render_over_budget_max_us",
    "wake_gt_period", "wake_gt_half_period", "wake_max_us",
    "no_frame_ticks", "bfi_black", "rt", "affinity", "cpu", "mlock",
    "gl_pinned", "prepare_gl_pinned",
]
with open(tsv_path, "w", encoding="utf-8") as fh:
    fh.write("\t".join(columns) + "\n")
    for row in windows:
        fh.write("\t".join(str(row.get(col, "")) for col in columns) + "\n")

print(json.dumps(summary, indent=2, sort_keys=True))
PY

python3 - "${LOG_PATH}" "${STAGE_SUMMARY_JSON}" "${STAGE_TSV}" <<'PY'
import json
import re
import statistics
import sys

log_path, summary_path, tsv_path = sys.argv[1:4]
line_re = re.compile(r"open3d presenter stage profile reason=(?P<reason>[^ ]+) (?P<body>.*)")
metric_re = re.compile(
    r"(?P<name>[A-Za-z0-9_]+)\(w=(?P<w>\d+)us cpu=(?P<cpu>\d+)us "
    r"wait=(?P<wait>\d+)us rq=(?P<rq>\d+)us\)"
)
kv_re = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
metric_names = [
    "prelude", "emitter_queue", "status_log", "status_debug",
    "status_debug_state_build", "status_debug_state_emit", "status_telemetry",
    "compose", "subtitle_prepare", "subtitle_clone", "subtitle_clone_subtitle",
    "subtitle_clone_ig", "subtitle_clone_ig_copy", "subtitle_pack",
    "overlay_subpicture", "overlay_route", "gl_lock", "make_current",
    "total", "prepare", "display", "swap", "release_current", "cleanup",
]


def percentile(values: list[int], pct: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = int(round((len(ordered) - 1) * pct))
    return ordered[index]


rows: list[dict[str, object]] = []
with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
    for line_no, line in enumerate(fh, 1):
        match = line_re.search(line)
        if not match:
            continue
        body = match.group("body")
        row: dict[str, object] = {"line": line_no, "reason": match.group("reason")}
        for metric in metric_re.finditer(body):
            name = metric.group("name")
            row[f"{name}_w_us"] = int(metric.group("w"))
            row[f"{name}_cpu_us"] = int(metric.group("cpu"))
            row[f"{name}_wait_us"] = int(metric.group("wait"))
            row[f"{name}_rq_us"] = int(metric.group("rq"))
        for key, value in kv_re.findall(body):
            row[key] = value
        rows.append(row)

summary: dict[str, object] = {"stage_profile_lines": len(rows), "metrics": {}}
metrics_summary: dict[str, object] = {}
for name in metric_names:
    values = [int(row.get(f"{name}_w_us", 0) or 0) for row in rows]
    values = [value for value in values if value > 0]
    if not values:
        continue
    metrics_summary[name] = {
        "n": len(values),
        "avg_us": round(statistics.fmean(values), 1),
        "max_us": max(values),
        "p95_us": percentile(values, 0.95),
    }
summary["metrics"] = metrics_summary

path_counts: dict[str, int] = {}
reason_counts: dict[str, int] = {}
for row in rows:
    path = str(row.get("present_path", "unknown"))
    reason = str(row.get("reason", "unknown"))
    path_counts[path] = path_counts.get(path, 0) + 1
    reason_counts[reason] = reason_counts.get(reason, 0) + 1
summary["present_path_counts"] = path_counts
summary["reason_counts"] = reason_counts

with open(summary_path, "w", encoding="utf-8") as fh:
    json.dump(summary, fh, indent=2, sort_keys=True)
    fh.write("\n")

columns = ["line", "reason", "present_path", "prepared_gate", "menu", "current_ig", "pack", "eye"]
for name in metric_names:
    columns.extend([f"{name}_w_us", f"{name}_cpu_us", f"{name}_wait_us", f"{name}_rq_us"])
with open(tsv_path, "w", encoding="utf-8") as fh:
    fh.write("\t".join(columns) + "\n")
    for row in rows:
        fh.write("\t".join(str(row.get(col, "")) for col in columns) + "\n")

print(json.dumps(summary, indent=2, sort_keys=True))
PY

{
  printf 'raw_exit_code=%s\n' "${raw_exit_code}"
  printf 'helper_terminated=%s\n' "${helper_terminated}"
  printf 'log=%s\n' "${LOG_PATH}"
  printf 'summary=%s\n' "${SUMMARY_JSON}"
  printf 'windows=%s\n' "${WINDOWS_TSV}"
  printf 'stage_summary=%s\n' "${STAGE_SUMMARY_JSON}"
  printf 'stage_profiles=%s\n' "${STAGE_TSV}"
} >"${STATUS_PATH}"

echo "Probe log: ${LOG_PATH}"
echo "Probe summary: ${SUMMARY_JSON}"
echo "Probe windows: ${WINDOWS_TSV}"
echo "Stage summary: ${STAGE_SUMMARY_JSON}"
echo "Stage profiles: ${STAGE_TSV}"
echo "Probe status: ${STATUS_PATH}"
