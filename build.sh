#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: build.sh [--check] [/path/to/vlc-3.0.23-source]

Build the maintained Open3D VLC runtime:
- vendored libbluray
- edge264
- VLC plugins (open3d, edge264mvc, open3dannexb, mkv, open3dbluraymvc, playlist, ts)
- staged stable plugin/runtime-lib set

Options:
  --check   Exit 0 if the staged runtime artifacts already exist, else 1.
  -h, --help

Env overrides:
  OPEN3D_VLC_SRC=/path/to/vlc-3.0.23-source
  EDGE264MVC_LIB=/path/to/libedge264.so.1
EOF
}

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECK_ONLY=0

default_vlc_src() {
  local latest=""

  if [[ -n "${OPEN3D_VLC_SRC:-}" ]]; then
    printf '%s\n' "${OPEN3D_VLC_SRC}"
    return 0
  fi

  while IFS= read -r latest; do
    [[ -n "${latest}" ]] && break
  done < <(find /tmp -maxdepth 1 -mindepth 1 -type d -name 'vlc-3.0.23-src-*' -printf '%T@ %p\n' 2>/dev/null | sort -nr | awk 'NR==1 {print $2}')

  if [[ -n "${latest}" ]]; then
    printf '%s\n' "${latest}"
  else
    printf '%s\n' "/tmp/vlc-3.0.23-src-20260310"
  fi
}

VLC_SRC="$(default_vlc_src)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --check)
      CHECK_ONLY=1
      shift
      ;;
    --)
      shift
      if [[ $# -gt 0 ]]; then
        VLC_SRC="$1"
        shift
      fi
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      VLC_SRC="$1"
      shift
      ;;
  esac
done

STABLE_ROOT="${REPO_DIR}/local/out/stable_runtime/latest"
REQUIRED_FILES=(
  "${STABLE_ROOT}/plugins/access/libopen3dbluraymvc_plugin.so"
  "${STABLE_ROOT}/plugins/codec/libedge264mvc_plugin.so"
  "${STABLE_ROOT}/plugins/demux/libopen3dannexb_plugin.so"
  "${STABLE_ROOT}/plugins/demux/libmkv_plugin.so"
  "${STABLE_ROOT}/plugins/video_output/libopen3d_plugin.so"
  "${STABLE_ROOT}/runtime-lib/libedge264.so.1"
)

runtime_ready() {
  local missing=0
  local path

  for path in "${REQUIRED_FILES[@]}"; do
    if [[ ! -f "${path}" ]]; then
      echo "missing ${path}" >&2
      missing=1
    fi
  done

  if ! compgen -G "${STABLE_ROOT}/runtime-lib/libbluray.so*" > /dev/null; then
    echo "missing ${STABLE_ROOT}/runtime-lib/libbluray.so*" >&2
    missing=1
  fi

  return "${missing}"
}

if [[ "${CHECK_ONLY}" == "1" ]]; then
  runtime_ready
  exit $?
fi

if [[ ! -d "${VLC_SRC}" ]]; then
  echo "Error: VLC source tree not found: ${VLC_SRC}" >&2
  exit 2
fi

mkdir -p "${REPO_DIR}/local/out/locks"
exec 9>"${REPO_DIR}/local/out/locks/open3d_build.lock"
flock 9

EDGE264_SOURCE_LIB="${EDGE264MVC_LIB:-}"
if [[ -n "${EDGE264_SOURCE_LIB}" ]]; then
  EDGE264_SOURCE_LIB="$(readlink -f "${EDGE264_SOURCE_LIB}")"
  if [[ ! -f "${EDGE264_SOURCE_LIB}" ]]; then
    echo "Error: EDGE264MVC_LIB not found: ${EDGE264_SOURCE_LIB}" >&2
    exit 3
  fi
else
  "${REPO_DIR}/scripts/build_edge264_ndebug.sh"
  EDGE264_SOURCE_LIB="${REPO_DIR}/vendor/edge264/libedge264_ndebug.so.1"
  if [[ ! -f "${EDGE264_SOURCE_LIB}" ]]; then
    echo "Error: expected built edge264 library missing: ${EDGE264_SOURCE_LIB}" >&2
    exit 4
  fi
fi

"${REPO_DIR}/scripts/build_vendor_libbluray_open3d.sh"
OPEN3D_VENDOR_LIBBLURAY_STAGE="${REPO_DIR}/local/out/vendor_stage/libbluray" \
  OPEN3D_VLC_BUILD_PHASE=prepare \
  "${REPO_DIR}/scripts/build_open3d_module_vlc3.sh" "${VLC_SRC}"
make -C "${VLC_SRC}/src" libvlccore.la
make -C "${VLC_SRC}/lib" libvlc.la
make -C "${VLC_SRC}/bin" vlc
OPEN3D_VENDOR_LIBBLURAY_STAGE="${REPO_DIR}/local/out/vendor_stage/libbluray" \
  OPEN3D_VLC_BUILD_PHASE=plugins \
  "${REPO_DIR}/scripts/build_open3d_module_vlc3.sh" "${VLC_SRC}"
OPEN3D_VENDOR_LIBBLURAY_STAGE="${REPO_DIR}/local/out/vendor_stage/libbluray" \
  "${REPO_DIR}/scripts/update_open3d_mvc_stable_runtime.sh" "${VLC_SRC}" "${EDGE264_SOURCE_LIB}"

echo "Open3D build complete."
echo "  stable runtime: ${STABLE_ROOT}"
