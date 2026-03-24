#!/usr/bin/env bash

open3d_extract_user_sub_track() {
  local -n args_ref=$1
  local token
  local i

  for ((i = 0; i < ${#args_ref[@]}; ++i)); do
    token="${args_ref[i]}"
    if [[ "${token}" == --sub-track=* ]]; then
      printf '%s\n' "${token#--sub-track=}"
      return 0
    elif [[ "${token}" == "--sub-track" && $((i + 1)) -lt ${#args_ref[@]} ]]; then
      printf '%s\n' "${args_ref[i + 1]}"
      return 0
    fi
  done

  return 1
}

open3d_resolve_mkv_plane_map_path() {
  local script_dir="$1"
  local media_path="$2"
  local plane_map_path="${OPEN3D_MKV_SUBTITLE_PLANE_MAP:-}"

  if [[ -n "${plane_map_path}" ]]; then
    if [[ ! -f "${plane_map_path}" ]]; then
      printf '%s\n' "open3d launcher: plane map not found: ${plane_map_path}" >&2
      return 1
    fi
    printf '%s\n' "${plane_map_path}"
    return 0
  fi

  if ! command -v ffprobe >/dev/null 2>&1; then
    return 1
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    return 1
  fi
  if [[ ! -f "${script_dir}/extract_mkv_subtitle_plane_map.py" ]]; then
    return 1
  fi

  local ffprobe_json_tmp
  local plane_map_tmp
  ffprobe_json_tmp="$(mktemp)"
  plane_map_tmp="$(mktemp)"
  env -u LD_LIBRARY_PATH -u VLC_PLUGIN_PATH \
    ffprobe -v quiet \
    -show_format \
    -show_streams \
    -print_format json \
    "${media_path}" >"${ffprobe_json_tmp}"
  env -u LD_LIBRARY_PATH -u VLC_PLUGIN_PATH \
    python3 "${script_dir}/extract_mkv_subtitle_plane_map.py" \
    "${ffprobe_json_tmp}" >"${plane_map_tmp}"
  rm -f "${ffprobe_json_tmp}"
  printf '%s\n' "${plane_map_tmp}"
}

open3d_lookup_mkv_subtitle_static_offset() {
  local plane_map_path="$1"
  local sub_track="$2"

  env -u LD_LIBRARY_PATH -u VLC_PLUGIN_PATH python3 - "${plane_map_path}" "${sub_track}" <<'PY'
import json
import sys

plane_map_path = sys.argv[1]
sub_track = int(sys.argv[2])

with open(plane_map_path, "r", encoding="utf-8") as fh:
    obj = json.load(fh)

for entry in obj.get("subtitle_plane_map", []):
    if int(entry.get("sub_track_index", -1)) == sub_track:
        units = entry.get("static_offset_units")
        plane = entry.get("plane")
        language = entry.get("language") or ""
        stream_index = entry.get("stream_index")
        source_id = entry.get("source_id") or ""
        if units is None:
            print("||||")
        else:
            print(f"{units}|{plane}|{language}|{stream_index}|{source_id}")
        break
PY
}

open3d_scan_user_args() {
  local prefix="$1"
  shift

  local -a args=("$@")
  local token
  local media_arg_index=-1
  local skip_media_detection_for_next=0
  local user_vout_value=""
  local user_intf_value=""
  local user_demux_value=""
  local user_vout_set=0
  local user_intf_set=0
  local user_demux_set=0
  local user_extraintf_set=0
  local user_codec_set=0
  local user_mkv_static_offset_set=0
  local user_mkv_subtitle_force_set=0
  local user_mkv_plane_set=0
  local user_mkv_source_id_set=0
  local user_edge264mvc_normalize_ts_set=0
  local i

  for ((i = 0; i < ${#args[@]}; ++i)); do
    token="${args[i]}"

    if [[ "${skip_media_detection_for_next}" == "1" ]]; then
      skip_media_detection_for_next=0
      continue
    fi

    if (( media_arg_index < 0 )) && [[ "${token}" != -* ]]; then
      media_arg_index="${i}"
    fi

    case "${token}" in
      --intf)
        user_intf_set=1
        if (( i + 1 < ${#args[@]} )); then
          user_intf_value="${args[i + 1]}"
        fi
        skip_media_detection_for_next=1
        ;;
      --intf=*)
        user_intf_set=1
        user_intf_value="${token#--intf=}"
        ;;
      -I)
        user_intf_set=1
        if (( i + 1 < ${#args[@]} )); then
          user_intf_value="${args[i + 1]}"
        fi
        skip_media_detection_for_next=1
        ;;
      -I=*)
        user_intf_set=1
        user_intf_value="${token#-I=}"
        ;;
      --vout)
        user_vout_set=1
        if (( i + 1 < ${#args[@]} )); then
          user_vout_value="${args[i + 1]}"
        fi
        skip_media_detection_for_next=1
        ;;
      --vout=*)
        user_vout_set=1
        user_vout_value="${token#--vout=}"
        ;;
      --demux)
        user_demux_set=1
        if (( i + 1 < ${#args[@]} )); then
          user_demux_value="${args[i + 1]}"
        fi
        skip_media_detection_for_next=1
        ;;
      --demux=*)
        user_demux_set=1
        user_demux_value="${token#--demux=}"
        ;;
      --codec)
        user_codec_set=1
        skip_media_detection_for_next=1
        ;;
      --codec=*)
        user_codec_set=1
        ;;
      --extraintf)
        user_extraintf_set=1
        skip_media_detection_for_next=1
        ;;
      --extraintf=*)
        user_extraintf_set=1
        ;;
      --sub-track|--run-time|--verbose|--open3d-presenter-affinity-cpu|\
      --open3d-mkv-subtitle-static-offset-units|--open3d-mkv-subtitle-plane|\
      --open3d-mkv-subtitle-source-id)
        case "${token}" in
          --open3d-mkv-subtitle-static-offset-units)
            user_mkv_static_offset_set=1
            ;;
          --open3d-mkv-subtitle-plane)
            user_mkv_plane_set=1
            ;;
          --open3d-mkv-subtitle-source-id)
            user_mkv_source_id_set=1
            ;;
        esac
        skip_media_detection_for_next=1
        ;;
      --open3d-mkv-subtitle-static-offset-units=*)
        user_mkv_static_offset_set=1
        ;;
      --open3d-mkv-subtitle-force|--no-open3d-mkv-subtitle-force)
        user_mkv_subtitle_force_set=1
        ;;
      --open3d-mkv-subtitle-plane=*)
        user_mkv_plane_set=1
        ;;
      --open3d-mkv-subtitle-source-id=*)
        user_mkv_source_id_set=1
        ;;
      --edge264mvc-normalize-ts-cadence|--no-edge264mvc-normalize-ts-cadence)
        user_edge264mvc_normalize_ts_set=1
        ;;
    esac
  done

  printf -v "${prefix}_media_arg_index" '%s' "${media_arg_index}"
  printf -v "${prefix}_user_vout_value" '%s' "${user_vout_value}"
  printf -v "${prefix}_user_intf_value" '%s' "${user_intf_value}"
  printf -v "${prefix}_user_demux_value" '%s' "${user_demux_value}"
  printf -v "${prefix}_user_vout_set" '%s' "${user_vout_set}"
  printf -v "${prefix}_user_intf_set" '%s' "${user_intf_set}"
  printf -v "${prefix}_user_demux_set" '%s' "${user_demux_set}"
  printf -v "${prefix}_user_extraintf_set" '%s' "${user_extraintf_set}"
  printf -v "${prefix}_user_codec_set" '%s' "${user_codec_set}"
  printf -v "${prefix}_user_mkv_static_offset_set" '%s' "${user_mkv_static_offset_set}"
  printf -v "${prefix}_user_mkv_subtitle_force_set" '%s' "${user_mkv_subtitle_force_set}"
  printf -v "${prefix}_user_mkv_plane_set" '%s' "${user_mkv_plane_set}"
  printf -v "${prefix}_user_mkv_source_id_set" '%s' "${user_mkv_source_id_set}"
  printf -v "${prefix}_user_edge264mvc_normalize_ts_set" '%s' "${user_edge264mvc_normalize_ts_set}"
}
