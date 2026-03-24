#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

VLC_VERSION="${OPEN3D_VLC_VERSION:-3.0.23}"
VLC_URL="${OPEN3D_VLC_URL:-https://get.videolan.org/vlc/${VLC_VERSION}/vlc-${VLC_VERSION}.tar.xz}"
APPIMAGE_ROOT="${OPEN3D_APPIMAGE_WORK_ROOT:-/opt/open3doled-appimage}"
SRC_CACHE_DIR="${APPIMAGE_ROOT}/src"
BUILD_ROOT="${APPIMAGE_ROOT}/build"
STAGE_ROOT="${APPIMAGE_ROOT}/stage"
TARBALL_PATH="${OPEN3D_VLC_TARBALL:-${SRC_CACHE_DIR}/vlc-${VLC_VERSION}.tar.xz}"
VLC_SRC_DIR="${OPEN3D_APPIMAGE_VLC_SRC_DIR:-${BUILD_ROOT}/vlc-${VLC_VERSION}-src}"
PREFIX_DIR="${OPEN3D_APPIMAGE_PREFIX_DIR:-${STAGE_ROOT}/usr}"
RUN_STAMP="${OPEN3D_APPIMAGE_RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUT_ROOT="${OPEN3D_APPIMAGE_OUT_ROOT:-/out/fullstack-builds}"
RUN_DIR="${OUT_ROOT}/${RUN_STAMP}"
SUMMARY_FILE="${RUN_DIR}/summary.txt"
BUILD_JOBS="${OPEN3D_VLC_BUILD_JOBS:-$(nproc)}"
OPEN3D_VLC_ABI_ALIAS_T64="${OPEN3D_VLC_ABI_ALIAS_T64:-0}"
OPEN3D_APPIMAGE_EXTENDED_MODULE_SET="${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET:-1}"
OPEN3D_APPIMAGE_REUSE_SOURCE_TREE="${OPEN3D_APPIMAGE_REUSE_SOURCE_TREE:-1}"
OPEN3D_APPIMAGE_CLEAN_BUILD="${OPEN3D_APPIMAGE_CLEAN_BUILD:-0}"

mkdir -p "${SRC_CACHE_DIR}" "${BUILD_ROOT}" "${STAGE_ROOT}" "${RUN_DIR}"

log() {
  printf '%s\n' "$*" | tee -a "${SUMMARY_FILE}"
}

run_logged() {
  local log_name="$1"
  shift
  (
    set -x
    "$@"
  ) 2>&1 | tee "${RUN_DIR}/${log_name}"
}

download_tarball() {
  if [[ -f "${TARBALL_PATH}" ]]; then
    log "Using cached VLC tarball: ${TARBALL_PATH}"
    return 0
  fi

  log "Downloading upstream VLC tarball: ${VLC_URL}"
  run_logged "download.log" curl -L --fail --output "${TARBALL_PATH}" "${VLC_URL}"
}

extract_source() {
  if [[ "${OPEN3D_APPIMAGE_CLEAN_BUILD}" == "1" ]]; then
    rm -rf "${VLC_SRC_DIR}"
  fi
  if [[ "${OPEN3D_APPIMAGE_REUSE_SOURCE_TREE}" == "1" &&
        -x "${VLC_SRC_DIR}/configure" &&
        -f "${VLC_SRC_DIR}/Makefile.am" ]]; then
    log "Reusing extracted VLC source tree: ${VLC_SRC_DIR}"
    return 0
  fi

  rm -rf "${VLC_SRC_DIR}"
  mkdir -p "${VLC_SRC_DIR}"
  log "Extracting VLC source into: ${VLC_SRC_DIR}"
  run_logged "extract.log" tar -xf "${TARBALL_PATH}" --strip-components=1 -C "${VLC_SRC_DIR}"
}

qt_generated_targets_from_overlay() {
  python3 - "${REPO_DIR}/packaging/appimage/overlays/vlc-3.0.23/modules/Makefile.in" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
lines = path.read_text().splitlines()
capturing = False
entries = []

for line in lines:
    if line.startswith("nodist_libqt_plugin_la_SOURCES = "):
        capturing = True
        payload = line.split("=", 1)[1].strip()
    elif capturing:
        if not line.startswith("\t"):
            break
        payload = line.strip()
    else:
        continue

    if payload.endswith("\\"):
        payload = payload[:-1].strip()

    for part in payload.split():
        if part.startswith("gui/qt/"):
            entries.append(part)

for entry in entries:
    print(entry)
PY
}

wayland_generated_targets=(
  video_output/wayland/viewporter-client-protocol.h
  video_output/wayland/viewporter-protocol.c
  video_output/wayland/xdg-shell-client-protocol.h
  video_output/wayland/xdg-shell-protocol.c
  video_output/wayland/server-decoration-client-protocol.h
  video_output/wayland/server-decoration-protocol.c
)

configure_args=(
  "--prefix=${PREFIX_DIR}"
  "--libdir=${PREFIX_DIR}/lib"
  "--disable-static"
  "--enable-shared"
  "--enable-qt"
  "--enable-xcb"
  "--enable-wayland"
)

log "Open3DOLED AppImage fullstack build"
log "repo=${REPO_DIR}"
log "vlc_version=${VLC_VERSION}"
log "vlc_url=${VLC_URL}"
log "tarball=${TARBALL_PATH}"
log "vlc_src=${VLC_SRC_DIR}"
log "prefix=${PREFIX_DIR}"
log "jobs=${BUILD_JOBS}"
log "abi_alias_t64=${OPEN3D_VLC_ABI_ALIAS_T64}"
log "extended_module_set=${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET}"
log "reuse_source_tree=${OPEN3D_APPIMAGE_REUSE_SOURCE_TREE}"
log "clean_build=${OPEN3D_APPIMAGE_CLEAN_BUILD}"
log "configure_args=${configure_args[*]}"

download_tarball
extract_source

export OPEN3D_LIBBLURAY_BUILD_DIR="${BUILD_ROOT}/vendor/libbluray/build"
export OPEN3D_LIBBLURAY_STAGE_DIR="${BUILD_ROOT}/vendor/libbluray/stage"
run_logged "libbluray.log" "${REPO_DIR}/scripts/build_vendor_libbluray_open3d.sh"

BLURAY_PKGCONFIG_DIR="${OPEN3D_LIBBLURAY_STAGE_DIR}/lib/x86_64-linux-gnu/pkgconfig"
if [[ -d "${BLURAY_PKGCONFIG_DIR}" ]]; then
  export PKG_CONFIG_PATH="${BLURAY_PKGCONFIG_DIR}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  log "pkg_config_path=${PKG_CONFIG_PATH}"
fi

export OPEN3D_VENDOR_LIBBLURAY_STAGE="${OPEN3D_LIBBLURAY_STAGE_DIR}"
export OPEN3D_VLC_BUILD_JOBS="${BUILD_JOBS}"
export OPEN3D_VLC_ABI_ALIAS_T64

run_logged "open3d-prepare.log" env OPEN3D_VLC_BUILD_PHASE=prepare \
  "${REPO_DIR}/scripts/build_open3d_module_vlc3.sh" \
  "${VLC_SRC_DIR}" \
  "${configure_args[@]}"

run_logged "vlc-core.log" bash -lc "
  set -euo pipefail
  make -C \"${VLC_SRC_DIR}/src\" -j\"${BUILD_JOBS}\" libvlccore.la
  make -C \"${VLC_SRC_DIR}/lib\" -j\"${BUILD_JOBS}\" libvlc.la
  make -C \"${VLC_SRC_DIR}/bin\" -j\"${BUILD_JOBS}\" vlc
"

run_logged "open3d-modules.log" env OPEN3D_VLC_BUILD_PHASE=plugins \
  "${REPO_DIR}/scripts/build_open3d_module_vlc3.sh" \
  "${VLC_SRC_DIR}" \
  "${configure_args[@]}"

run_logged "src-generated.log" make -C "${VLC_SRC_DIR}/src" -j"${BUILD_JOBS}" \
  ../include/vlc_about.h

mapfile -t qt_generated_targets < <(qt_generated_targets_from_overlay)
if [[ ${#qt_generated_targets[@]} -eq 0 ]]; then
  log "Failed to resolve Qt generated-source targets from overlay"
  exit 1
fi

playback_plugin_targets=(
  libfilesystem_plugin.la
  libmkv_plugin.la
  libavcodec_plugin.la
  libpacketizer_dts_plugin.la
  libpacketizer_copy_plugin.la
  libpulse_plugin.la
  libfloat_mixer_plugin.la
  libaudio_format_plugin.la
  libscaletempo_plugin.la
  libsamplerate_plugin.la
  libfreetype_plugin.la
  libswscale_plugin.la
  libyuvp_plugin.la
  libgl_plugin.la
  libegl_x11_plugin.la
  liblogger_plugin.la
  libconsole_logger_plugin.la
  libfile_logger_plugin.la
)
extended_plugin_targets=(
  libpacketizer_a52_plugin.la
  libdbus_plugin.la
  liboldrc_plugin.la
  libdirectory_demux_plugin.la
  libequalizer_plugin.la
  libdeinterlace_plugin.la
  libsubsdelay_plugin.la
  liblua_plugin.la
  libexport_plugin.la
)
desktop_common_plugin_targets=(
  libxml_plugin.la
  libalsa_plugin.la
  libhttp_plugin.la
  libhttps_plugin.la
  libftp_plugin.la
  libtcp_plugin.la
  libudp_plugin.la
  librtp_plugin.la
  libsdp_plugin.la
  libmp4_plugin.la
  libavi_plugin.la
  libogg_plugin.la
  libwav_plugin.la
  libsubtitle_plugin.la
  libsubsdec_plugin.la
  libdvbsub_plugin.la
  libspudec_plugin.la
  libwebvtt_plugin.la
  libegl_wl_plugin.la
  libwl_shm_plugin.la
  libxdg_shell_plugin.la
  libxcb_window_plugin.la
  libglx_plugin.la
)

if [[ "${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET}" == "1" ]]; then
  playback_plugin_targets+=("${extended_plugin_targets[@]}")
  playback_plugin_targets+=("${desktop_common_plugin_targets[@]}")
fi

run_logged "ui-generated.log" make -C "${VLC_SRC_DIR}/modules" -j"${BUILD_JOBS}" \
  "${qt_generated_targets[@]}"

if [[ "${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET}" == "1" ]]; then
  run_logged "wayland-generated.log" make -C "${VLC_SRC_DIR}/modules" -j"${BUILD_JOBS}" \
    "${wayland_generated_targets[@]}"
fi

run_logged "ui.log" make -C "${VLC_SRC_DIR}/modules" -j"${BUILD_JOBS}" \
  libdummy_plugin.la \
  libhotkeys_plugin.la \
  libxcb_hotkeys_plugin.la \
  libqt_plugin.la

run_logged "playback.log" make -C "${VLC_SRC_DIR}/modules" -j"${BUILD_JOBS}" \
  "${playback_plugin_targets[@]}"

log "Artifact checks:"
required_artifacts=(
  "${VLC_SRC_DIR}/bin/vlc" \
  "${VLC_SRC_DIR}/src/.libs/libvlccore.so" \
  "${VLC_SRC_DIR}/lib/.libs/libvlc.so" \
  "${VLC_SRC_DIR}/modules/.libs/libdummy_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libhotkeys_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libxcb_hotkeys_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libqt_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libopen3d_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libedge264mvc_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libopen3dannexb_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libts_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libtsmvc_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libplaylist_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libopen3dbluraymvc_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libfilesystem_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libmkv_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libavcodec_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libvlc_pulse.so" \
  "${VLC_SRC_DIR}/modules/.libs/libpulse_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libfloat_mixer_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libaudio_format_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libscaletempo_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libsamplerate_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libfreetype_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libswscale_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libyuvp_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libgl_plugin.so" \
  "${VLC_SRC_DIR}/modules/.libs/libegl_x11_plugin.so" \
    "${VLC_SRC_DIR}/modules/.libs/liblogger_plugin.so" \
    "${VLC_SRC_DIR}/modules/.libs/libconsole_logger_plugin.so" \
    "${VLC_SRC_DIR}/modules/.libs/libfile_logger_plugin.so"
)

if [[ "${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET}" == "1" ]]; then
  required_artifacts+=(
    "${VLC_SRC_DIR}/modules/.libs/libdbus_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/liboldrc_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libdirectory_demux_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libequalizer_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libdeinterlace_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libsubsdelay_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/liblua_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libexport_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libxml_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libalsa_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libhttp_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libhttps_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libftp_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libtcp_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libudp_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/librtp_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libsdp_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libmp4_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libavi_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libogg_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libwav_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libsubtitle_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libsubsdec_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libdvbsub_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libspudec_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libwebvtt_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libegl_wl_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libwl_shm_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libxdg_shell_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libxcb_window_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libglx_plugin.so"
  )
fi

for artifact in "${required_artifacts[@]}"; do
  if [[ -e "${artifact}" ]]; then
    log "  OK ${artifact}"
  else
    log "  MISSING ${artifact}"
    exit 1
  fi
done

log "Run directory: ${RUN_DIR}"
log "Build completed successfully."
