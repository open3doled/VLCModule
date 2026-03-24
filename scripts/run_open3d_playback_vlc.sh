#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_open3d_playback_vlc.sh [/path/to/vlc-3.0.23-source] <media-path-or-url> [vlc args...]

Maintained playback path:
- `.iso` -> `open3dbluraymvc://`
- `.mkv` -> native MKV demux + `edge264mvc`
- `.264/.h264/.mvc` -> `--demux=open3dannexb`

Optional env tuning:
- `OPEN3D_PROCESS_CPUSET=4-7`
- `OPEN3D_PROCESS_CPU_BACKEND=auto|systemd|taskset`
- `OPEN3D_PRESENTER_PREFERRED_CPU=auto|N`
- `OPEN3D_PROCESS_SCHED_POLICY=fifo|rr`
- `OPEN3D_PROCESS_RT_PRIORITY=10`
- `OPEN3D_PROCESS_IO_CLASS=realtime|best-effort|idle`
- `OPEN3D_PROCESS_IO_PRIORITY=0`
- `OPEN3D_MKV_SUBTITLE_PLANE_MAP=/path/to/plane_map.json`
EOF
}

if [[ $# -lt 2 ]]; then
  usage >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
. "${SCRIPT_DIR}/open3d_process_isolation.sh"
. "${SCRIPT_DIR}/open3d_media_routing.sh"
. "${SCRIPT_DIR}/open3d_launcher_media_helpers.sh"
VLC_SRC=""
if [[ -d "$1" ]]; then
  VLC_SRC="$(cd "$1" && pwd)"
fi
INPUT_MEDIA="$2"
shift 2
USER_VLC_ARGS=("$@")
isolation_prefix=()
isolation_vlc_args=()
mkv_plane_map_tmp=""

enable_open3d_vout="${OPEN3D_MVC_ENABLE_VOUT:-0}"
no_plugins_cache="${OPEN3D_MVC_NO_PLUGINS_CACHE:-1}"
ignore_config="${OPEN3D_MVC_IGNORE_CONFIG:-0}"
runtime_profile="${OPEN3D_MVC_RUNTIME_PROFILE:-stable}"
prestaged_plugin_path="${OPEN3D_MVC_PRESTAGED_PLUGIN_PATH:-}"

runtime_shell="$("${SCRIPT_DIR}/resolve_open3d_runtime_profile.sh" "${runtime_profile}")"
resolved_plugins="$(printf '%s\n' "${runtime_shell}" | sed -n 's/^plugins_path=//p')"
resolved_edge264_lib="$(printf '%s\n' "${runtime_shell}" | sed -n 's/^edge264_lib=//p')"
resolved_helper_lib_dir="$(printf '%s\n' "${runtime_shell}" | sed -n 's/^helper_lib_dir=//p')"

if [[ -z "${EDGE264MVC_LIB:-}" && -n "${resolved_edge264_lib}" && -f "${resolved_edge264_lib}" ]]; then
  export EDGE264MVC_LIB="${resolved_edge264_lib}"
fi

if [[ -z "${prestaged_plugin_path}" && -n "${resolved_plugins}" && -d "${resolved_plugins}" ]]; then
  prestaged_plugin_path="${resolved_plugins}"
fi

append_unique_path() {
  local var_name="$1"
  local path_value="$2"
  local current entry

  [[ -n "${path_value}" ]] || return 0
  [[ -d "${path_value}" ]] || return 0

  current="${!var_name:-}"
  IFS=':' read -r -a current_entries <<<"${current}"
  for entry in "${current_entries[@]}"; do
    if [[ "${entry}" == "${path_value}" ]]; then
      return 0
    fi
  done

  if [[ -n "${current}" ]]; then
    printf -v "${var_name}" '%s:%s' "${current}" "${path_value}"
  else
    printf -v "${var_name}" '%s' "${path_value}"
  fi
}

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

discover_system_plugin_path() {
  local candidate

  for candidate in \
    /usr/lib/x86_64-linux-gnu/vlc/plugins \
    /usr/lib/vlc/plugins \
    /usr/local/lib/vlc/plugins; do
    if [[ -d "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
}

validate_plugin_root() {
  local plugin_root="$1"
  local nested_plugin

  nested_plugin="$(find "${plugin_root}" -mindepth 3 -type f \
    \( -path '*/codec/libedge264mvc_plugin.so' \
       -o -path '*/demux/libopen3dannexb_plugin.so' \
       -o -path '*/video_output/libopen3d_plugin.so' \
       -o -path '*/access/libopen3dbluraymvc_plugin.so' \) \
    -print -quit 2>/dev/null || true)"
  if [[ -n "${nested_plugin}" ]]; then
    echo "Error: plugin path contains nested staged plugins: ${nested_plugin}" >&2
    echo "Use an isolated leaf stage dir for VLC_PLUGIN_PATH, not a parent containing archived stages." >&2
    exit 2
  fi
}

PLUGIN_PATH=""
stage_log=""
cleanup() {
  if [[ -n "${stage_log}" && -f "${stage_log}" ]]; then
    rm -f "${stage_log}"
  fi
  if [[ -n "${mkv_plane_map_tmp}" && -f "${mkv_plane_map_tmp}" ]]; then
    rm -f "${mkv_plane_map_tmp}"
  fi
}
trap cleanup EXIT

stage_plugin_root() {
  stage_log="$(mktemp)"
  local -a stage_env=()
  if [[ -z "${VLC_SRC}" ]]; then
    echo "Error: VLC source tree is required to stage plugins when no prestaged runtime is available." >&2
    exit 2
  fi
  if [[ "${enable_open3d_vout}" != "1" ]]; then
    stage_env+=("OPEN3D_STAGE_SKIP_VOUT=1")
  fi
  if [[ -n "${OPEN3D_STAGE_DIR:-}" ]]; then
    env "${stage_env[@]}" \
      "${SCRIPT_DIR}/stage_open3d_plugin_vlc3.sh" "${VLC_SRC}" "${OPEN3D_STAGE_DIR}" >"${stage_log}"
  else
    env "${stage_env[@]}" \
      "${SCRIPT_DIR}/stage_open3d_plugin_vlc3.sh" "${VLC_SRC}" >"${stage_log}"
  fi

  PLUGIN_PATH="$(sed -n 's/^VLC_PLUGIN_PATH=//p' "${stage_log}" | tail -n1)"
  if [[ -z "${PLUGIN_PATH}" ]]; then
    echo "Error: failed to determine staged plugin path" >&2
    cat "${stage_log}" >&2
    exit 3
  fi
}

if [[ -n "${prestaged_plugin_path}" ]]; then
  if [[ ! -d "${prestaged_plugin_path}" ]]; then
    echo "Error: OPEN3D_MVC_PRESTAGED_PLUGIN_PATH directory not found: ${prestaged_plugin_path}" >&2
    exit 4
  fi
  PLUGIN_PATH="$(cd "${prestaged_plugin_path}" && pwd)"
else
  stage_plugin_root
fi

validate_plugin_root "${PLUGIN_PATH}"

selected_media="${INPUT_MEDIA}"
selected_demux=""
needs_edge264=0
needs_access_plugin=0
needs_open3dannexb_plugin=0

user_vout=""
user_intf=""
open3d_scan_user_args host_scan "${USER_VLC_ARGS[@]}"
user_vout="${host_scan_user_vout_value}"
user_intf="${host_scan_user_intf_value}"
user_extraintf_set="${host_scan_user_extraintf_set}"
user_codec_set="${host_scan_user_codec_set}"
user_mkv_static_offset_set="${host_scan_user_mkv_static_offset_set}"
user_mkv_subtitle_force_set="${host_scan_user_mkv_subtitle_force_set}"
user_mkv_plane_set="${host_scan_user_mkv_plane_set}"
user_mkv_source_id_set="${host_scan_user_mkv_source_id_set}"
user_edge264mvc_normalize_ts_set="${host_scan_user_edge264mvc_normalize_ts_set}"

if ! open3d_media_route_path "${INPUT_MEDIA}"; then
  echo "Error: media path not found: ${INPUT_MEDIA}" >&2
  exit 5
fi

selected_media="${OPEN3D_MEDIA_SELECTED}"
selected_demux="${OPEN3D_MEDIA_DEMUX}"
needs_edge264="${OPEN3D_MEDIA_NEEDS_EDGE264}"
needs_access_plugin="${OPEN3D_MEDIA_NEEDS_ACCESS_PLUGIN}"
needs_open3dannexb_plugin="${OPEN3D_MEDIA_NEEDS_OPEN3DANNEXB_PLUGIN}"
is_url="${OPEN3D_MEDIA_IS_URL}"
ext_lc="${OPEN3D_MEDIA_EXT_LC}"
input_media_abs="${OPEN3D_MEDIA_ABS}"

if [[ ! -f "${PLUGIN_PATH}/codec/libedge264mvc_plugin.so" && "${needs_edge264}" == "1" ]]; then
  stage_plugin_root
  validate_plugin_root "${PLUGIN_PATH}"
fi
if [[ ! -f "${PLUGIN_PATH}/access/libopen3dbluraymvc_plugin.so" && "${needs_access_plugin}" == "1" ]]; then
  stage_plugin_root
  validate_plugin_root "${PLUGIN_PATH}"
fi
if [[ ! -f "${PLUGIN_PATH}/demux/libopen3dannexb_plugin.so" && "${needs_open3dannexb_plugin}" == "1" ]]; then
  stage_plugin_root
  validate_plugin_root "${PLUGIN_PATH}"
fi
if [[ "${enable_open3d_vout}" == "1" && ! -f "${PLUGIN_PATH}/video_output/libopen3d_plugin.so" ]]; then
  stage_plugin_root
  validate_plugin_root "${PLUGIN_PATH}"
fi

if [[ "${needs_edge264}" == "1" && ! -f "${PLUGIN_PATH}/codec/libedge264mvc_plugin.so" ]]; then
  echo "Error: edge264mvc plugin missing under plugin path: ${PLUGIN_PATH}" >&2
  exit 6
fi
if [[ "${needs_access_plugin}" == "1" && ! -f "${PLUGIN_PATH}/access/libopen3dbluraymvc_plugin.so" ]]; then
  echo "Error: open3dbluraymvc plugin missing under plugin path: ${PLUGIN_PATH}" >&2
  exit 7
fi
if [[ "${needs_open3dannexb_plugin}" == "1" && ! -f "${PLUGIN_PATH}/demux/libopen3dannexb_plugin.so" ]]; then
  echo "Error: open3dannexb raw Annex-B plugin missing under plugin path: ${PLUGIN_PATH}" >&2
  exit 8
fi
if [[ "${enable_open3d_vout}" == "1" && ! -f "${PLUGIN_PATH}/video_output/libopen3d_plugin.so" ]]; then
  echo "Error: open3d vout requested but plugin missing under plugin path: ${PLUGIN_PATH}" >&2
  exit 9
fi

runtime_plugin_path="${PLUGIN_PATH}"
if system_plugin_path="$(discover_system_plugin_path)"; then
  append_unique_path runtime_plugin_path "${system_plugin_path}"
fi
if [[ -n "${VLC_PLUGIN_PATH:-}" ]]; then
  IFS=':' read -r -a user_plugin_entries <<<"${VLC_PLUGIN_PATH}"
  for user_plugin_entry in "${user_plugin_entries[@]}"; do
    append_unique_path runtime_plugin_path "${user_plugin_entry}"
  done
fi

runtime_ld_library_path="${LD_LIBRARY_PATH:-}"
if [[ -n "${resolved_helper_lib_dir}" && -d "${resolved_helper_lib_dir}" ]]; then
  runtime_ld_library_path="${resolved_helper_lib_dir}${runtime_ld_library_path:+:${runtime_ld_library_path}}"
fi
if [[ -n "${EDGE264MVC_LIB:-}" ]]; then
  edge264_lib_dir="$(cd "$(dirname "${EDGE264MVC_LIB}")" && pwd)"
  runtime_ld_library_path="${edge264_lib_dir}${runtime_ld_library_path:+:${runtime_ld_library_path}}"
fi

start_x11_window_tuner() {
  if [[ "${OPEN3D_X11_BYPASS_COMPOSITOR:-0}" != "1" ]]; then
    return 0
  fi
  if [[ -z "${DISPLAY:-}" ]]; then
    echo "open3d x11 tuner: DISPLAY is not set, skipping"
    return 0
  fi
  "${SCRIPT_DIR}/open3d_x11_window_tuner.sh" "$$" bypass_compositor &
}

vlc_args=(
  --no-metadata-network-access
  --no-video-title-show
  --no-qt-privacy-ask
  --no-qt-error-dialogs
)

if [[ "${OPEN3D_MVC_FORCE_SEPARATE_INSTANCE:-0}" == "1" ]]; then
  vlc_args+=(--no-one-instance)
fi
if [[ -z "${user_intf}" ]]; then
  vlc_args+=(--intf qt)
fi
if [[ "${user_extraintf_set}" == "0" ]]; then
  vlc_args+=(--extraintf=)
fi
if [[ -z "${user_vout}" ]]; then
  if [[ "${enable_open3d_vout}" == "1" ]]; then
    vlc_args+=(--vout=open3d)
  else
    vlc_args+=(--vout=gl)
  fi
fi
if [[ "${no_plugins_cache}" == "1" ]]; then
  vlc_args+=(--no-plugins-cache)
fi
if [[ "${ignore_config}" == "1" ]]; then
  vlc_args+=(--ignore-config)
fi
if [[ "${enable_open3d_vout}" == "1" ]]; then
  vlc_args+=(
    --open3d-enable
    --open3d-emitter-enable
    --open3d-emitter-tty=auto
  )
fi
if [[ -n "${EDGE264MVC_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  extra_decoder_args=( ${EDGE264MVC_EXTRA_ARGS} )
  vlc_args+=("${extra_decoder_args[@]}")
fi
if [[ -n "${selected_demux}" ]]; then
  vlc_args+=("--demux=${selected_demux}")
fi
if [[ "${needs_edge264}" == "1" && "${user_codec_set}" == "0" ]]; then
  vlc_args+=("--codec=edge264mvc,avcodec")
fi

if [[ "${is_url}" -eq 0 && "${ext_lc:-}" == "mkv" &&
      "${user_edge264mvc_normalize_ts_set}" == "0" ]]; then
  vlc_args+=("--edge264mvc-normalize-ts-cadence")
  echo "Playback MKV timestamp normalization: enabled"
fi

if [[ "${is_url}" -eq 0 && "${ext_lc:-}" == "mkv" &&
      "${enable_open3d_vout}" == "1" &&
      ( "${user_mkv_static_offset_set}" == "0" ||
        "${user_mkv_subtitle_force_set}" == "0" ||
        "${user_mkv_plane_set}" == "0" ||
        "${user_mkv_source_id_set}" == "0" ) ]]; then
  if user_sub_track="$(open3d_extract_user_sub_track USER_VLC_ARGS 2>/dev/null)" &&
     [[ "${user_sub_track}" =~ ^-?[0-9]+$ ]] &&
     (( user_sub_track >= 0 )); then
    if plane_map_path="$(open3d_resolve_mkv_plane_map_path "${SCRIPT_DIR}" "${input_media_abs}")"; then
      mkv_plane_map_tmp="${plane_map_path}"
      mkv_offset_info="$(open3d_lookup_mkv_subtitle_static_offset "${plane_map_path}" "${user_sub_track}" || true)"
      IFS='|' read -r mkv_static_units mkv_plane mkv_language mkv_stream_index mkv_source_id <<<"${mkv_offset_info}"
      if [[ -n "${mkv_stream_index:-}" ]]; then
        mkv_source_id_normalized="${mkv_source_id:-}"
        if [[ -n "${mkv_source_id_normalized}" && "${mkv_source_id_normalized}" =~ ^[0-9]+$ ]]; then
          mkv_source_id_normalized="$((10#${mkv_source_id_normalized}))"
        fi
        if [[ "${user_mkv_subtitle_force_set}" == "0" ]]; then
          vlc_args+=("--open3d-mkv-subtitle-force")
        fi
        if [[ -n "${mkv_static_units:-}" && "${user_mkv_static_offset_set}" == "0" ]]; then
          vlc_args+=("--open3d-mkv-subtitle-static-offset-units=${mkv_static_units}")
        fi
        if [[ -n "${mkv_plane:-}" && "${user_mkv_plane_set}" == "0" ]]; then
          vlc_args+=("--open3d-mkv-subtitle-plane=${mkv_plane}")
        fi
        if [[ -n "${mkv_source_id_normalized:-}" && "${user_mkv_source_id_set}" == "0" ]]; then
          vlc_args+=("--open3d-mkv-subtitle-source-id=${mkv_source_id_normalized}")
        fi
        echo "Playback MKV subtitle metadata: sub_track=${user_sub_track} stream_index=${mkv_stream_index:-} language=${mkv_language:-} plane=${mkv_plane:-} source_id=${mkv_source_id_normalized:-${mkv_source_id:-}} units=${mkv_static_units:-} map=${plane_map_path}"
      fi
    fi
  fi
fi

open3d_isolation_build_exec_prefix isolation_prefix
open3d_isolation_append_presenter_args isolation_vlc_args "${vlc_args[@]}" "${USER_VLC_ARGS[@]}"

echo "Playback media: ${selected_media}"
echo "Playback plugin path: ${runtime_plugin_path}"
if [[ -n "${selected_demux}" ]]; then
  echo "Playback demux: ${selected_demux}"
fi
if (( ${#isolation_prefix[@]} > 0 )); then
  echo "Playback isolation: ${isolation_prefix[*]}"
fi
if (( ${#isolation_vlc_args[@]} > 0 )); then
  echo "Playback isolation args: ${isolation_vlc_args[*]}"
fi

log_path="$(prepare_launcher_log playback)"
mkdir -p "$(dirname "${log_path}")"
echo "Playback log: ${log_path}"
exec > >(tee -a "${log_path}") 2>&1

start_x11_window_tuner

exec "${isolation_prefix[@]}" env \
  VLC_PLUGIN_PATH="${runtime_plugin_path}" \
  LD_LIBRARY_PATH="${runtime_ld_library_path}" \
  "${VLC_BIN:-/usr/bin/vlc}" \
  "${vlc_args[@]}" \
  "${isolation_vlc_args[@]}" \
  "${USER_VLC_ARGS[@]}" \
  "${selected_media}"
