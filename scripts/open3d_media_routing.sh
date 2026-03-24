#!/usr/bin/env bash

open3d_media_lower_ext() {
  local path="$1"
  local base
  base="$(basename -- "${path}")"
  if [[ "${base}" != *.* ]]; then
    return 1
  fi
  printf '%s\n' "${base##*.}" | tr '[:upper:]' '[:lower:]'
}

open3d_media_is_url() {
  [[ "$1" == *"://"* ]]
}

open3d_media_is_bluray_root_dir() {
  local path="$1"
  [[ -d "${path}" && -f "${path}/BDMV/index.bdmv" ]]
}

open3d_media_is_bluray_bdmv_dir() {
  local path="$1"
  [[ -d "${path}" ]] || return 1
  [[ "$(basename -- "${path}")" == "BDMV" && -f "${path}/index.bdmv" ]]
}

open3d_media_is_bluray_index_file() {
  local path="$1"
  local base
  local parent
  base="$(basename -- "${path}")"
  parent="$(basename -- "$(dirname -- "${path}")")"
  [[ "${base}" == "index.bdmv" && "${parent}" == "BDMV" ]]
}

open3d_media_normalize_bluray_path() {
  local input_path="$1"
  local abs_path=""
  local ext_lc=""

  if [[ ! -e "${input_path}" && ! -b "${input_path}" ]]; then
    return 1
  fi

  abs_path="$(readlink -f "${input_path}")"
  if ext_lc="$(open3d_media_lower_ext "${abs_path}" 2>/dev/null)"; then
    if [[ "${ext_lc}" == "iso" ]]; then
      printf '%s\n' "${abs_path}"
      return 0
    fi
  fi

  if [[ -b "${abs_path}" ]]; then
    printf '%s\n' "${abs_path}"
    return 0
  fi

  if open3d_media_is_bluray_root_dir "${abs_path}"; then
    printf '%s\n' "${abs_path}"
    return 0
  fi

  if open3d_media_is_bluray_bdmv_dir "${abs_path}"; then
    printf '%s\n' "$(dirname -- "${abs_path}")"
    return 0
  fi

  if open3d_media_is_bluray_index_file "${abs_path}"; then
    printf '%s\n' "$(dirname -- "$(dirname -- "${abs_path}")")"
    return 0
  fi

  return 1
}

open3d_media_reset_route() {
  OPEN3D_MEDIA_SELECTED=""
  OPEN3D_MEDIA_ABS=""
  OPEN3D_MEDIA_EXT_LC=""
  OPEN3D_MEDIA_KIND="other"
  OPEN3D_MEDIA_DEMUX=""
  OPEN3D_MEDIA_NEEDS_EDGE264=0
  OPEN3D_MEDIA_NEEDS_ACCESS_PLUGIN=0
  OPEN3D_MEDIA_NEEDS_OPEN3DANNEXB_PLUGIN=0
  OPEN3D_MEDIA_IS_URL=0
}

open3d_media_route_path() {
  local input_path="$1"
  local normalized_bluray=""

  open3d_media_reset_route
  OPEN3D_MEDIA_SELECTED="${input_path}"

  if open3d_media_is_url "${input_path}"; then
    OPEN3D_MEDIA_IS_URL=1
    if [[ "${input_path}" == open3dbluraymvc://* ]]; then
      OPEN3D_MEDIA_KIND="bluray"
      OPEN3D_MEDIA_NEEDS_EDGE264=1
      OPEN3D_MEDIA_NEEDS_ACCESS_PLUGIN=1
    fi
    return 0
  fi

  if [[ ! -e "${input_path}" && ! -b "${input_path}" ]]; then
    return 1
  fi

  OPEN3D_MEDIA_ABS="$(readlink -f "${input_path}")"
  OPEN3D_MEDIA_EXT_LC="$(open3d_media_lower_ext "${input_path}" 2>/dev/null || true)"

  if normalized_bluray="$(open3d_media_normalize_bluray_path "${input_path}" 2>/dev/null)"; then
    OPEN3D_MEDIA_SELECTED="open3dbluraymvc://${normalized_bluray}"
    OPEN3D_MEDIA_ABS="${normalized_bluray}"
    OPEN3D_MEDIA_KIND="bluray"
    OPEN3D_MEDIA_NEEDS_EDGE264=1
    OPEN3D_MEDIA_NEEDS_ACCESS_PLUGIN=1
    if [[ "${OPEN3D_MEDIA_EXT_LC}" != "iso" ]]; then
      OPEN3D_MEDIA_EXT_LC=""
    fi
    return 0
  fi

  case "${OPEN3D_MEDIA_EXT_LC}" in
    mkv)
      OPEN3D_MEDIA_SELECTED="${OPEN3D_MEDIA_ABS}"
      OPEN3D_MEDIA_KIND="mkv"
      OPEN3D_MEDIA_NEEDS_EDGE264=1
      ;;
    264|h264|mvc)
      OPEN3D_MEDIA_SELECTED="${OPEN3D_MEDIA_ABS}"
      OPEN3D_MEDIA_KIND="annexb"
      OPEN3D_MEDIA_DEMUX="open3dannexb"
      OPEN3D_MEDIA_NEEDS_EDGE264=1
      OPEN3D_MEDIA_NEEDS_OPEN3DANNEXB_PLUGIN=1
      ;;
    *)
      OPEN3D_MEDIA_SELECTED="${OPEN3D_MEDIA_ABS}"
      ;;
  esac

  return 0
}
