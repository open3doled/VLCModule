#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${REPO_DIR}/vendor/libbluray"
BUILD_DIR="${OPEN3D_LIBBLURAY_BUILD_DIR:-${REPO_DIR}/local/out/vendor_build/libbluray}"
STAGE_DIR="${OPEN3D_LIBBLURAY_STAGE_DIR:-${REPO_DIR}/local/out/vendor_stage/libbluray}"
BUILD_TYPE="${OPEN3D_LIBBLURAY_BUILD_TYPE:-debugoptimized}"
LOCAL_DEVROOT="${REPO_DIR}/local/out/devpkgs/root"

if [[ ! -f "${SRC_DIR}/meson.build" ]]; then
  echo "Missing vendored libbluray source at ${SRC_DIR}" >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}" "${STAGE_DIR}"

LOCAL_PKGCONFIG_DIR="${BUILD_DIR}/local-pkgconfig"
mkdir -p "${LOCAL_PKGCONFIG_DIR}"

if ! pkg-config --exists libudfread 2>/dev/null; then
  UDFREAD_CFLAGS_DIR=""
  UDFREAD_LIBS_VALUE=""
  if [[ -d "${LOCAL_DEVROOT}/usr/include/udfread" ]]; then
    UDFREAD_CFLAGS_DIR="${LOCAL_DEVROOT}/usr/include"
  fi
  if [[ -e "${LOCAL_DEVROOT}/usr/lib/x86_64-linux-gnu/libudfread.so" ]] && \
     [[ -f "$(readlink -f "${LOCAL_DEVROOT}/usr/lib/x86_64-linux-gnu/libudfread.so")" ]]; then
    UDFREAD_LIBS_VALUE="-L${LOCAL_DEVROOT}/usr/lib/x86_64-linux-gnu -ludfread"
  elif [[ -f "/usr/lib/x86_64-linux-gnu/libudfread.so.0" ]]; then
    UDFREAD_LIBS_VALUE="/usr/lib/x86_64-linux-gnu/libudfread.so.0"
  elif [[ -f "/lib/x86_64-linux-gnu/libudfread.so.0" ]]; then
    UDFREAD_LIBS_VALUE="/lib/x86_64-linux-gnu/libudfread.so.0"
  fi
  if [[ -n "${UDFREAD_CFLAGS_DIR}" ]] && [[ -n "${UDFREAD_LIBS_VALUE}" ]]; then
    cat > "${LOCAL_PKGCONFIG_DIR}/libudfread.pc" <<EOF
prefix=${LOCAL_DEVROOT}/usr
exec_prefix=\${prefix}
libdir=\${prefix}/lib/x86_64-linux-gnu
includedir=\${prefix}/include

Name: libudfread
Description: Local devroot libudfread fallback for Open3D libbluray builds
Version: 1.2.0
Libs: ${UDFREAD_LIBS_VALUE}
Cflags: -I${UDFREAD_CFLAGS_DIR}
EOF
  fi
fi

PKG_CONFIG_PATH_COMBINED="${LOCAL_PKGCONFIG_DIR}"
if [[ -n "${PKG_CONFIG_PATH:-}" ]]; then
  PKG_CONFIG_PATH_COMBINED+=":${PKG_CONFIG_PATH}"
fi

MESON_ARGS=(
  "setup"
  "${BUILD_DIR}"
  "${SRC_DIR}"
  "--reconfigure"
  "--buildtype=${BUILD_TYPE}"
  "--prefix=${STAGE_DIR}"
  "--libdir=lib/x86_64-linux-gnu"
  "--includedir=include"
  "-Denable_docs=false"
  "-Denable_tools=false"
  "-Denable_devtools=false"
  "-Denable_examples=false"
  "-Dbdj_jar=disabled"
  "-Dlibxml2=disabled"
)

if [[ ! -d "${BUILD_DIR}/meson-private" ]]; then
  MESON_ARGS=(
    "setup"
    "${BUILD_DIR}"
    "${SRC_DIR}"
    "--buildtype=${BUILD_TYPE}"
    "--prefix=${STAGE_DIR}"
    "--libdir=lib/x86_64-linux-gnu"
    "--includedir=include"
    "-Denable_docs=false"
    "-Denable_tools=false"
    "-Denable_devtools=false"
    "-Denable_examples=false"
    "-Dbdj_jar=disabled"
    "-Dlibxml2=disabled"
  )
fi

env PKG_CONFIG_PATH="${PKG_CONFIG_PATH_COMBINED}" meson "${MESON_ARGS[@]}"
env PKG_CONFIG_PATH="${PKG_CONFIG_PATH_COMBINED}" meson compile -C "${BUILD_DIR}"
env PKG_CONFIG_PATH="${PKG_CONFIG_PATH_COMBINED}" meson install -C "${BUILD_DIR}" --only-changed

echo "OPEN3D_LIBBLURAY_BUILD_DIR=${BUILD_DIR}"
echo "OPEN3D_LIBBLURAY_STAGE_DIR=${STAGE_DIR}"
echo "OPEN3D_LIBBLURAY_SO=${STAGE_DIR}/lib/x86_64-linux-gnu/libbluray.so"
