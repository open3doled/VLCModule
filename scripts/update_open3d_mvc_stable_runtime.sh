#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "Usage: $0 /path/to/vlc-3.0.23-source [edge264-lib-path] [stable-runtime-root]" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VLC_SRC="$(cd "$1" && pwd)"
EDGE264_REPO_DEFAULT="${REPO_DIR}/vendor/edge264"

EDGE264_LIB="${2:-}"
if [[ -z "${EDGE264_LIB}" ]]; then
  if [[ -f "${EDGE264_REPO_DEFAULT}/libedge264_ndebug.so.1" ]]; then
    EDGE264_LIB="${EDGE264_REPO_DEFAULT}/libedge264_ndebug.so.1"
  elif [[ -f "${EDGE264_REPO_DEFAULT}/libedge264.so.1" ]]; then
    EDGE264_LIB="${EDGE264_REPO_DEFAULT}/libedge264.so.1"
  else
    echo "Error: edge264 library not provided and no default found in ${EDGE264_REPO_DEFAULT}." >&2
    exit 2
  fi
fi
EDGE264_LIB="$(cd "$(dirname "${EDGE264_LIB}")" && pwd)/$(basename "${EDGE264_LIB}")"

if [[ ! -f "${EDGE264_LIB}" ]]; then
  echo "Error: edge264 library not found: ${EDGE264_LIB}" >&2
  exit 3
fi

STABLE_ROOT="${3:-${REPO_DIR}/local/out/stable_runtime}"
TARGET="${STABLE_ROOT}/latest"
PLUGINS_DIR="${TARGET}/plugins"
LIB_DIR="${TARGET}/lib"
META_FILE="${TARGET}/STAMP.txt"
VENDOR_BLURAY_STAGE="${OPEN3D_VENDOR_LIBBLURAY_STAGE:-${REPO_DIR}/local/out/vendor_stage/libbluray}"
VENDOR_BLURAY_LIBDIR="${VENDOR_BLURAY_STAGE}/lib/x86_64-linux-gnu"

DECODER_SRC="${VLC_SRC}/modules/.libs/libedge264mvc_plugin.so"
OPEN3DMKV_SRC="${VLC_SRC}/modules/.libs/libopen3dmkv_plugin.so"
BLURAY_MVC_SRC="${VLC_SRC}/modules/.libs/libopen3dbluraymvc_plugin.so"
VOUT_SRC="${VLC_SRC}/modules/.libs/libopen3d_plugin.so"
PLAYLIST_SRC="${VLC_SRC}/modules/.libs/libplaylist_plugin.so"

if [[ ! -f "${DECODER_SRC}" ]]; then
  echo "Error: decoder plugin not found: ${DECODER_SRC}" >&2
  exit 4
fi

copy_if_needed() {
  local src="$1"
  local dst="$2"
  local src_real=""
  local dst_real=""

  [[ -f "${src}" ]] || return 0

  src_real="$(readlink -f "${src}")"
  if [[ -e "${dst}" ]]; then
    dst_real="$(readlink -f "${dst}")"
    if [[ "${src_real}" == "${dst_real}" ]]; then
      return 0
    fi
  fi

  cp -f "${src}" "${dst}"
}

mkdir -p "${PLUGINS_DIR}/codec" "${PLUGINS_DIR}/demux" "${PLUGINS_DIR}/video_output" "${PLUGINS_DIR}/access" "${LIB_DIR}"

copy_if_needed "${DECODER_SRC}" "${PLUGINS_DIR}/codec/libedge264mvc_plugin.so"
rm -f "${PLUGINS_DIR}/demux/libmvcasm_plugin.so"
if [[ -f "${OPEN3DMKV_SRC}" ]]; then
  copy_if_needed "${OPEN3DMKV_SRC}" "${PLUGINS_DIR}/demux/libopen3dmkv_plugin.so"
else
  rm -f "${PLUGINS_DIR}/demux/libopen3dmkv_plugin.so"
fi
if [[ -f "${BLURAY_MVC_SRC}" ]]; then
  copy_if_needed "${BLURAY_MVC_SRC}" "${PLUGINS_DIR}/access/libopen3dbluraymvc_plugin.so"
else
  rm -f "${PLUGINS_DIR}/access/libopen3dbluraymvc_plugin.so"
fi
if [[ -f "${PLAYLIST_SRC}" ]]; then
  copy_if_needed "${PLAYLIST_SRC}" "${PLUGINS_DIR}/demux/libplaylist_plugin.so"
fi
if [[ -f "${VOUT_SRC}" ]]; then
  copy_if_needed "${VOUT_SRC}" "${PLUGINS_DIR}/video_output/libopen3d_plugin.so"
fi
copy_if_needed "${EDGE264_LIB}" "${LIB_DIR}/libedge264.so.1"
copy_if_needed "${EDGE264_LIB}" "${LIB_DIR}/$(basename "${EDGE264_LIB}")"
rm -f "${LIB_DIR}/libbluray.so" "${LIB_DIR}/libbluray.so.3" "${LIB_DIR}/libbluray.so.3.1.0"
if compgen -G "${VENDOR_BLURAY_LIBDIR}/libbluray.so*" > /dev/null; then
  for bluray_lib in "${VENDOR_BLURAY_LIBDIR}"/libbluray.so*; do
    cp -a "${bluray_lib}" "${LIB_DIR}/"
  done
fi
rm -f "${PLUGINS_DIR}/plugins.dat"

{
  echo "updated_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "vlc_src=${VLC_SRC}"
  echo "edge264_lib_source=${EDGE264_LIB}"
  echo "vendor_bluray_stage=${VENDOR_BLURAY_STAGE}"
  echo "repo_head=$(git -C "${REPO_DIR}" rev-parse --short HEAD 2>/dev/null || echo n/a)"
  echo "edge264_head=$(git -C "${EDGE264_REPO_DEFAULT}" rev-parse --short HEAD 2>/dev/null || echo n/a)"
} > "${META_FILE}"

echo "Stable runtime updated:"
echo "  plugins: ${PLUGINS_DIR}"
echo "  edge264: ${LIB_DIR}/libedge264.so.1"
echo "Set defaults:"
echo "  OPEN3D_MVC_PRESTAGED_PLUGIN_PATH=${PLUGINS_DIR}"
echo "  EDGE264MVC_LIB=${LIB_DIR}/libedge264.so.1"
