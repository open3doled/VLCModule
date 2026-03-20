#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_open3d_playback_vlc.sh /path/to/vlc-3.0.23-source <media-path-or-url> [vlc args...]

Maintained playback path:
- `.iso` -> `open3dbluraymvc://`
- `.mkv` -> native MKV demux + `edge264mvc`
- `.264/.h264/.mvc` -> `--demux=open3dmkv`
EOF
}

if [[ $# -lt 2 ]]; then
  usage >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VLC_SRC="$(cd "$1" && pwd)"
INPUT_MEDIA="$2"
shift 2
USER_VLC_ARGS=("$@")

enable_open3d_vout="${OPEN3D_MVC_ENABLE_VOUT:-0}"
no_plugins_cache="${OPEN3D_MVC_NO_PLUGINS_CACHE:-1}"
ignore_config="${OPEN3D_MVC_IGNORE_CONFIG:-0}"
runtime_profile="${OPEN3D_MVC_RUNTIME_PROFILE:-stable}"
prestaged_plugin_path="${OPEN3D_MVC_PRESTAGED_PLUGIN_PATH:-}"

runtime_shell="$("${SCRIPT_DIR}/resolve_open3d_runtime_profile.sh" "${runtime_profile}")"
resolved_plugins="$(printf '%s\n' "${runtime_shell}" | sed -n 's/^plugins_path=//p')"
resolved_edge264_lib="$(printf '%s\n' "${runtime_shell}" | sed -n 's/^edge264_lib=//p')"

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
       -o -path '*/demux/libopen3dmkv_plugin.so' \
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
}
trap cleanup EXIT

stage_plugin_root() {
  stage_log="$(mktemp)"
  local -a stage_env=()
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
needs_open3dmkv_plugin=0

user_vout=""
user_intf=""
user_extraintf_set=0
user_codec_set=0
user_avcodec_hw_set=0
for ((i = 0; i < ${#USER_VLC_ARGS[@]}; ++i)); do
  token="${USER_VLC_ARGS[i]}"
  if [[ "${token}" == --vout=* ]]; then
    user_vout="${token#--vout=}"
  elif [[ "${token}" == "--vout" && $((i + 1)) -lt ${#USER_VLC_ARGS[@]} ]]; then
    user_vout="${USER_VLC_ARGS[i + 1]}"
  elif [[ "${token}" == --intf=* ]]; then
    user_intf="${token#--intf=}"
  elif [[ "${token}" == "--intf" && $((i + 1)) -lt ${#USER_VLC_ARGS[@]} ]]; then
    user_intf="${USER_VLC_ARGS[i + 1]}"
  elif [[ "${token}" == "-I" && $((i + 1)) -lt ${#USER_VLC_ARGS[@]} ]]; then
    user_intf="${USER_VLC_ARGS[i + 1]}"
  elif [[ "${token}" == --extraintf=* || "${token}" == "--extraintf" ]]; then
    user_extraintf_set=1
  elif [[ "${token}" == --codec=* || "${token}" == "--codec" ]]; then
    user_codec_set=1
  elif [[ "${token}" == --avcodec-hw=* || "${token}" == "--avcodec-hw" ]]; then
    user_avcodec_hw_set=1
  fi
done

is_url=0
if [[ "${INPUT_MEDIA}" == *"://"* ]]; then
  is_url=1
fi

if [[ "${is_url}" -eq 0 && ! -e "${INPUT_MEDIA}" ]]; then
  echo "Error: media path not found: ${INPUT_MEDIA}" >&2
  exit 5
fi

if [[ "${is_url}" -eq 0 && "${INPUT_MEDIA}" == *.* ]]; then
  input_media_abs="$(readlink -f "${INPUT_MEDIA}")"
  ext_lc="$(printf '%s' "${INPUT_MEDIA##*.}" | tr '[:upper:]' '[:lower:]')"
  case "${ext_lc}" in
    iso)
      selected_media="open3dbluraymvc://${input_media_abs}"
      needs_edge264=1
      needs_access_plugin=1
      ;;
    mkv)
      selected_media="${input_media_abs}"
      needs_edge264=1
      ;;
    264|h264|mvc)
      selected_media="${input_media_abs}"
      selected_demux="open3dmkv"
      needs_edge264=1
      needs_open3dmkv_plugin=1
      ;;
    *)
      selected_media="${input_media_abs}"
      ;;
  esac
fi

if [[ "${selected_media}" == open3dbluraymvc://* ]]; then
  needs_access_plugin=1
fi

if [[ ! -f "${PLUGIN_PATH}/codec/libedge264mvc_plugin.so" && "${needs_edge264}" == "1" ]]; then
  stage_plugin_root
  validate_plugin_root "${PLUGIN_PATH}"
fi
if [[ ! -f "${PLUGIN_PATH}/access/libopen3dbluraymvc_plugin.so" && "${needs_access_plugin}" == "1" ]]; then
  stage_plugin_root
  validate_plugin_root "${PLUGIN_PATH}"
fi
if [[ ! -f "${PLUGIN_PATH}/demux/libopen3dmkv_plugin.so" && "${needs_open3dmkv_plugin}" == "1" ]]; then
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
if [[ "${needs_open3dmkv_plugin}" == "1" && ! -f "${PLUGIN_PATH}/demux/libopen3dmkv_plugin.so" ]]; then
  echo "Error: open3dmkv plugin missing under plugin path: ${PLUGIN_PATH}" >&2
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
if [[ -n "${EDGE264MVC_LIB:-}" ]]; then
  edge264_lib_dir="$(cd "$(dirname "${EDGE264MVC_LIB}")" && pwd)"
  runtime_ld_library_path="${edge264_lib_dir}${runtime_ld_library_path:+:${runtime_ld_library_path}}"
fi

vlc_args=(
  --no-qt-privacy-ask
  --no-metadata-network-access
  --no-video-title-show
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
  if [[ "${user_avcodec_hw_set}" == "0" ]]; then
    vlc_args+=("--avcodec-hw=none")
  fi
fi

echo "Playback media: ${selected_media}"
echo "Playback plugin path: ${runtime_plugin_path}"
if [[ -n "${selected_demux}" ]]; then
  echo "Playback demux: ${selected_demux}"
fi

exec env \
  VLC_PLUGIN_PATH="${runtime_plugin_path}" \
  LD_LIBRARY_PATH="${runtime_ld_library_path}" \
  "${VLC_BIN:-vlc}" \
  "${vlc_args[@]}" \
  "${USER_VLC_ARGS[@]}" \
  "${selected_media}"
