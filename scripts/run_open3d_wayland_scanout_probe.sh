#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_dir}/local/build/tools/wayland-scanout-probe"
out_root="${repo_dir}/local/out/wayland-scanout-probe"
stamp="$(date +%Y%m%d_%H%M%S)"
run_dir="${out_root}/${stamp}"

mkdir -p "${build_dir}" "${run_dir}"

proto_dir="$(pkg-config --variable=pkgdatadir wayland-protocols)"
scanner="${WAYLAND_SCANNER:-wayland-scanner}"

gen_xdg_h="${build_dir}/xdg-shell-client-protocol.h"
gen_xdg_c="${build_dir}/xdg-shell-protocol.c"
gen_pres_h="${build_dir}/presentation-time-client-protocol.h"
gen_pres_c="${build_dir}/presentation-time-protocol.c"

"${scanner}" client-header \
  "${proto_dir}/stable/xdg-shell/xdg-shell.xml" "${gen_xdg_h}"
"${scanner}" private-code \
  "${proto_dir}/stable/xdg-shell/xdg-shell.xml" "${gen_xdg_c}"
"${scanner}" client-header \
  "${proto_dir}/stable/presentation-time/presentation-time.xml" "${gen_pres_h}"
"${scanner}" private-code \
  "${proto_dir}/stable/presentation-time/presentation-time.xml" "${gen_pres_c}"

src="${repo_dir}/tools/open3d_wayland_scanout_probe.c"
bin="${build_dir}/open3d_wayland_scanout_probe"

cc ${CFLAGS:-} -O2 -Wall -Wextra -std=c11 \
  -I"${build_dir}" \
  "${src}" "${gen_xdg_c}" "${gen_pres_c}" \
  $(pkg-config --cflags --libs wayland-client wayland-egl egl glesv2) \
  -lm -o "${bin}"

seconds=10
optical_capture=0
optical_port=""
optical_baud=115200
display_config="${OPEN3D_SCANOUT_DISPLAY_CONFIG:-}"
use_display_config=1
probe_args=()
csv_seen=0

while (($#)); do
  case "$1" in
    --display-config)
      display_config="${2:?missing value for --display-config}"
      shift 2
      ;;
    --no-display-config)
      use_display_config=0
      shift
      ;;
    --optical-capture)
      optical_capture=1
      shift
      ;;
    --optical-port)
      optical_port="${2:?missing value for --optical-port}"
      shift 2
      ;;
    --optical-baud)
      optical_baud="${2:?missing value for --optical-baud}"
      shift 2
      ;;
    --seconds)
      seconds="${2:?missing value for --seconds}"
      probe_args+=("$1" "$2")
      shift 2
      ;;
    --csv)
      csv_seen=1
      probe_args+=("$1" "$2")
      shift 2
      ;;
    *)
      probe_args+=("$1")
      shift
      ;;
  esac
done

presentation_csv="${run_dir}/presentation_feedback.csv"
optical_csv="${run_dir}/optical_debug.csv"
config_args=()

if [[ "${use_display_config}" == "1" ]]; then
  if [[ -z "${display_config}" ]]; then
    candidates=()
    if [[ -n "${XDG_CONFIG_HOME:-}" ]]; then
      candidates+=("${XDG_CONFIG_HOME}/open3doled/vlc/open3d_display_settings.json")
    fi
    candidates+=("${HOME}/.config/open3doled-vlc-appimage/open3doled/vlc/open3d_display_settings.json")
    candidates+=("${HOME}/.config/open3doled/vlc/open3d_display_settings.json")
    for candidate in "${candidates[@]}"; do
      if [[ -f "${candidate}" ]]; then
        display_config="${candidate}"
        break
      fi
    done
  fi

  if [[ -n "${display_config}" && -f "${display_config}" ]]; then
    mapfile -t config_args < <(python3 - "${display_config}" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = json.loads(path.read_text())

def emit(flag, value=None):
    print(flag)
    if value is not None:
        print(str(value))

if data.get("open3d-trigger-enable", True) is False:
    emit("--no-trigger-boxes")

mapping = {
    "open3d-trigger-size": "--trigger-size",
    "open3d-trigger-padding": "--trigger-padding",
    "open3d-trigger-spacing": "--trigger-spacing",
    "open3d-trigger-black-border": "--trigger-border",
    "open3d-trigger-offset-x": "--trigger-offset-x",
    "open3d-trigger-offset-y": "--trigger-offset-y",
    "open3d-trigger-corner": "--trigger-corner",
    "open3d-trigger-brightness": "--trigger-brightness",
}
for key, flag in mapping.items():
    if key in data:
        value = data[key]
        if isinstance(value, float) and value.is_integer():
            value = int(value)
        emit(flag, value)

if data.get("open3d-trigger-invert", False):
    emit("--trigger-invert")
PY
)
    echo "Loaded display trigger settings: ${display_config}" >&2
  elif [[ -n "${display_config}" ]]; then
    echo "Display config not found, using probe defaults: ${display_config}" >&2
  else
    echo "No display config found, using probe defaults" >&2
  fi
fi

if [[ "${csv_seen}" == "0" ]]; then
  probe_args+=(--csv "${presentation_csv}")
fi

echo "Open3D Wayland scanout probe run dir: ${run_dir}" >&2
echo "Presentation CSV: ${presentation_csv}" >&2
if [[ "${optical_capture}" == "1" ]]; then
  echo "Optical CSV: ${optical_csv}" >&2
fi

capture_pid=""
cleanup() {
  if [[ -n "${capture_pid}" ]] && kill -0 "${capture_pid}" 2>/dev/null; then
    kill "${capture_pid}" 2>/dev/null || true
    wait "${capture_pid}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [[ "${optical_capture}" == "1" ]]; then
  optical_seconds="$(python3 - <<PY
seconds = float("${seconds}")
print(f"{seconds + 1.5:.3f}")
PY
)"
  optical_cmd=(python3 "${repo_dir}/tools/open3d_optical_serial_capture.py"
               --seconds "${optical_seconds}"
               --baud "${optical_baud}"
               --csv "${optical_csv}")
  if [[ -n "${optical_port}" ]]; then
    optical_cmd+=(--port "${optical_port}")
  fi
  "${optical_cmd[@]}" 2>"${run_dir}/optical_capture.log" &
  capture_pid="$!"
  sleep 0.35
fi

"${bin}" "${config_args[@]}" "${probe_args[@]}" 2>&1 | tee "${run_dir}/probe.log"

if [[ -n "${capture_pid}" ]]; then
  wait "${capture_pid}" || true
  capture_pid=""
fi

echo "Open3D Wayland scanout probe complete: ${run_dir}" >&2
