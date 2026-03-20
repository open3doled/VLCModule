#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 /path/to/vlc-3.0.23-source [stage-dir]" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VLC_SRC="$(cd "$1" && pwd)"
STAGE_DIR="${2:-${REPO_DIR}/local/out/runtime_plugins_live}"

PLUGIN_SRC="${VLC_SRC}/modules/.libs/libopen3d_plugin.so"
PLUGIN_DST_DIR="${STAGE_DIR}/video_output"
PLUGIN_DST="${PLUGIN_DST_DIR}/libopen3d_plugin.so"
SKIP_VOUT="${OPEN3D_STAGE_SKIP_VOUT:-0}"

DECODER_PLUGIN_SRC="${VLC_SRC}/modules/.libs/libedge264mvc_plugin.so"
DECODER_PLUGIN_DST_DIR="${STAGE_DIR}/codec"
DECODER_PLUGIN_DST="${DECODER_PLUGIN_DST_DIR}/libedge264mvc_plugin.so"

OPEN3DMKV_PLUGIN_SRC="${VLC_SRC}/modules/.libs/libopen3dmkv_plugin.so"
OPEN3DMKV_PLUGIN_DST_DIR="${STAGE_DIR}/demux"
OPEN3DMKV_PLUGIN_DST="${OPEN3DMKV_PLUGIN_DST_DIR}/libopen3dmkv_plugin.so"

TS_PLUGIN_SRC="${VLC_SRC}/modules/.libs/libts_plugin.so"
TS_PLUGIN_DST_DIR="${STAGE_DIR}/demux"
TS_PLUGIN_DST="${TS_PLUGIN_DST_DIR}/libts_plugin.so"

TSMVC_PLUGIN_SRC="${VLC_SRC}/modules/.libs/libtsmvc_plugin.so"
TSMVC_PLUGIN_DST_DIR="${STAGE_DIR}/demux"
TSMVC_PLUGIN_DST="${TSMVC_PLUGIN_DST_DIR}/libtsmvc_plugin.so"

PLAYLIST_PLUGIN_SRC="${VLC_SRC}/modules/.libs/libplaylist_plugin.so"
PLAYLIST_PLUGIN_DST_DIR="${STAGE_DIR}/demux"
PLAYLIST_PLUGIN_DST="${PLAYLIST_PLUGIN_DST_DIR}/libplaylist_plugin.so"

BLURAY_MVC_PLUGIN_SRC="${VLC_SRC}/modules/.libs/libopen3dbluraymvc_plugin.so"
BLURAY_MVC_PLUGIN_DST_DIR="${STAGE_DIR}/access"
BLURAY_MVC_PLUGIN_DST="${BLURAY_MVC_PLUGIN_DST_DIR}/libopen3dbluraymvc_plugin.so"

find_nested_stage_plugins() {
  # A valid leaf stage contains plugin files directly under codec/, demux/,
  # and video_output/. Only flag deeper archived stage subdirectories.
  find "${STAGE_DIR}" -mindepth 3 -type f \
    \( -path '*/codec/libedge264mvc_plugin.so' \
       -o -path '*/demux/libopen3dmkv_plugin.so' \
       -o -path '*/video_output/libopen3d_plugin.so' \) \
    -print -quit 2>/dev/null
}

if [[ "${OPEN3D_STAGE_ALLOW_RECURSIVE_ROOT:-0}" != "1" ]]; then
  nested_plugin="$(find_nested_stage_plugins || true)"
  if [[ -n "${nested_plugin}" ]]; then
    echo "Error: stage dir contains nested staged plugins: ${nested_plugin}" >&2
    echo "VLC recursively scans plugin roots; use an isolated leaf stage dir, not a parent containing archived stages." >&2
    echo "Set OPEN3D_STAGE_ALLOW_RECURSIVE_ROOT=1 only if this is intentional." >&2
    exit 5
  fi
fi

if [[ "${SKIP_VOUT}" != "1" ]]; then
  if [[ ! -f "${PLUGIN_SRC}" ]]; then
    echo "Error: built plugin not found: ${PLUGIN_SRC}" >&2
    echo "Build first with scripts/build_open3d_module_vlc3.sh" >&2
    exit 2
  fi

  mkdir -p "${PLUGIN_DST_DIR}"
  install -m 755 "${PLUGIN_SRC}" "${PLUGIN_DST}"
else
  rm -f "${PLUGIN_DST}"
fi

if [[ -f "${DECODER_PLUGIN_SRC}" ]]; then
  mkdir -p "${DECODER_PLUGIN_DST_DIR}"
  install -m 755 "${DECODER_PLUGIN_SRC}" "${DECODER_PLUGIN_DST}"
else
  rm -f "${DECODER_PLUGIN_DST}"
  echo "Warning: edge264mvc decoder plugin not found at source: ${DECODER_PLUGIN_SRC}" >&2
fi

rm -f "${STAGE_DIR}/demux/libmvcasm_plugin.so"

if [[ -f "${OPEN3DMKV_PLUGIN_SRC}" ]]; then
  mkdir -p "${OPEN3DMKV_PLUGIN_DST_DIR}"
  install -m 755 "${OPEN3DMKV_PLUGIN_SRC}" "${OPEN3DMKV_PLUGIN_DST}"
else
  rm -f "${OPEN3DMKV_PLUGIN_DST}"
  echo "Warning: open3dmkv demux plugin not found at source: ${OPEN3DMKV_PLUGIN_SRC}" >&2
fi

if [[ -f "${TS_PLUGIN_SRC}" ]]; then
  mkdir -p "${TS_PLUGIN_DST_DIR}"
  install -m 755 "${TS_PLUGIN_SRC}" "${TS_PLUGIN_DST}"
else
  rm -f "${TS_PLUGIN_DST}"
  echo "Warning: TS demux plugin not found at source: ${TS_PLUGIN_SRC}" >&2
fi

if [[ -f "${TSMVC_PLUGIN_SRC}" ]]; then
  mkdir -p "${TSMVC_PLUGIN_DST_DIR}"
  install -m 755 "${TSMVC_PLUGIN_SRC}" "${TSMVC_PLUGIN_DST}"
else
  rm -f "${TSMVC_PLUGIN_DST}"
fi

if [[ -f "${PLAYLIST_PLUGIN_SRC}" ]]; then
  mkdir -p "${PLAYLIST_PLUGIN_DST_DIR}"
  install -m 755 "${PLAYLIST_PLUGIN_SRC}" "${PLAYLIST_PLUGIN_DST}"
else
  rm -f "${PLAYLIST_PLUGIN_DST}"
  echo "Warning: playlist plugin not found at source: ${PLAYLIST_PLUGIN_SRC}" >&2
fi

if [[ -f "${BLURAY_MVC_PLUGIN_SRC}" ]]; then
  mkdir -p "${BLURAY_MVC_PLUGIN_DST_DIR}"
  install -m 755 "${BLURAY_MVC_PLUGIN_SRC}" "${BLURAY_MVC_PLUGIN_DST}"
else
  rm -f "${BLURAY_MVC_PLUGIN_DST}"
  echo "Warning: Open3D Blu-ray MVC plugin not found at source: ${BLURAY_MVC_PLUGIN_SRC}" >&2
fi

# Force VLC to rescan the staged directory content.
rm -f "${STAGE_DIR}/plugins.dat"

if [[ "${SKIP_VOUT}" != "1" ]]; then
  echo "Staged Open3D plugin: ${PLUGIN_DST}"
else
  echo "Skipped Open3D vout staging (OPEN3D_STAGE_SKIP_VOUT=1)"
fi
if [[ -f "${DECODER_PLUGIN_DST}" ]]; then
  echo "Staged edge264mvc decoder plugin: ${DECODER_PLUGIN_DST}"
fi
if [[ -f "${OPEN3DMKV_PLUGIN_DST}" ]]; then
  echo "Staged open3dmkv demux plugin: ${OPEN3DMKV_PLUGIN_DST}"
fi
if [[ -f "${TS_PLUGIN_DST}" ]]; then
  echo "Staged TS demux plugin: ${TS_PLUGIN_DST}"
fi
if [[ -f "${TSMVC_PLUGIN_DST}" ]]; then
  echo "Staged TSMVC demux plugin: ${TSMVC_PLUGIN_DST}"
fi
if [[ -f "${PLAYLIST_PLUGIN_DST}" ]]; then
  echo "Staged playlist bridge plugin: ${PLAYLIST_PLUGIN_DST}"
fi
if [[ -f "${BLURAY_MVC_PLUGIN_DST}" ]]; then
  echo "Staged Open3D Blu-ray MVC plugin: ${BLURAY_MVC_PLUGIN_DST}"
fi
echo "VLC_PLUGIN_PATH=${STAGE_DIR}"
