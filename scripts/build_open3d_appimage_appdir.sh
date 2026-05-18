#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

VLC_VERSION="${OPEN3D_VLC_VERSION:-3.0.23}"
APPIMAGE_ROOT="${OPEN3D_APPIMAGE_WORK_ROOT:-/opt/open3doled-appimage}"
BUILD_ROOT="${APPIMAGE_ROOT}/build"
APPDIR_ROOT="${APPIMAGE_ROOT}/AppDir"
DEFAULT_VLC_SRC_DIR="${BUILD_ROOT}/vlc-${VLC_VERSION}-src"
VLC_SRC_DIR_OVERRIDE="${OPEN3D_APPIMAGE_VLC_SRC_DIR:-}"
VLC_SRC_DIR="${VLC_SRC_DIR_OVERRIDE:-${DEFAULT_VLC_SRC_DIR}}"
BLURAY_STAGE_DIR="${OPEN3D_LIBBLURAY_STAGE_DIR:-${BUILD_ROOT}/vendor/libbluray/stage}"
ACCESS_PLUGIN_OVERRIDE_PATH="${OPEN3D_APPIMAGE_ACCESS_PLUGIN_PATH:-}"
VOUT_PLUGIN_OVERRIDE_PATH="${OPEN3D_APPIMAGE_VOUT_PLUGIN_PATH:-}"
ITEM73_ACCESS_PLUS_HEADER_REFRESH="${OPEN3D_APPIMAGE_ITEM73_ACCESS_PLUS_HEADER_REFRESH:-0}"
ITEM73_MIN_COMPANION_REFRESH="${OPEN3D_APPIMAGE_ITEM73_MIN_COMPANION_REFRESH:-0}"
RUN_STAMP="${OPEN3D_APPIMAGE_RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUT_ROOT="${OPEN3D_APPIMAGE_OUT_ROOT:-/out/fullstack-builds}"
APPDIR_OUT_ROOT="${OPEN3D_APPIMAGE_APPDIR_OUT_ROOT:-/out/appdir-builds}"
RUN_DIR="${APPDIR_OUT_ROOT}/${RUN_STAMP}"
APPDIR_DIR="${RUN_DIR}/AppDir"
SUMMARY_FILE="${RUN_DIR}/summary.txt"
VERIFY_LOG="${RUN_DIR}/appdir-verify.log"
MANIFEST_FILE="${RUN_DIR}/manifest.txt"
SKIP_FULLSTACK_BUILD="${OPEN3D_APPIMAGE_SKIP_FULLSTACK_BUILD:-0}"
SKIP_VERIFY="${OPEN3D_APPIMAGE_SKIP_VERIFY:-0}"
BUILD_JOBS="${OPEN3D_VLC_BUILD_JOBS:-$(nproc)}"
OPEN3D_APPIMAGE_EXTENDED_MODULE_SET="${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET:-1}"
OPEN3D_APPIMAGE_ENABLE_WAYLAND="${OPEN3D_APPIMAGE_ENABLE_WAYLAND:-0}"
ITEM73_HELPER_SCRIPT="${REPO_DIR}/local/projects/bluray_menu_support/build_item73_access_plus_header_refresh_tree.sh"
ITEM73_MIN_COMPANION_HELPER_SCRIPT="${REPO_DIR}/local/projects/bluray_menu_support/build_item73_min_companion_refresh_tree.sh"
ITEM73_REFRESH_TREE_DIR=""

mkdir -p "${RUN_DIR}"
declare -A RUNTIME_CLOSURE_FAMILIES=()

log() {
  printf '%s\n' "$*" | tee -a "${SUMMARY_FILE}"
}

prepare_item73_access_plus_header_refresh_tree() {
  local helper_stamp helper_output

  [[ "${ITEM73_ACCESS_PLUS_HEADER_REFRESH}" == "1" ]] || return 0

  if [[ "${ITEM73_MIN_COMPANION_REFRESH}" == "1" ]]; then
    log "Cannot combine OPEN3D_APPIMAGE_ITEM73_ACCESS_PLUS_HEADER_REFRESH=1 with OPEN3D_APPIMAGE_ITEM73_MIN_COMPANION_REFRESH=1"
    exit 1
  fi

  if [[ -n "${VLC_SRC_DIR_OVERRIDE}" ]]; then
    log "Cannot combine OPEN3D_APPIMAGE_ITEM73_ACCESS_PLUS_HEADER_REFRESH=1 with OPEN3D_APPIMAGE_VLC_SRC_DIR"
    exit 1
  fi

  if [[ -n "${ACCESS_PLUGIN_OVERRIDE_PATH}" || -n "${VOUT_PLUGIN_OVERRIDE_PATH}" ]]; then
    log "Cannot combine OPEN3D_APPIMAGE_ITEM73_ACCESS_PLUS_HEADER_REFRESH=1 with plugin override hooks"
    exit 1
  fi

  if [[ ! -x "${ITEM73_HELPER_SCRIPT}" ]]; then
    log "Missing item 7.3 helper script: ${ITEM73_HELPER_SCRIPT}"
    exit 1
  fi

  helper_stamp="${OPEN3D_ITEM73_TREE_RUN_STAMP:-${RUN_STAMP}_item73_access_plus_header_tree}"
  helper_output="$(
    OPEN3D_ITEM73_TREE_RUN_STAMP="${helper_stamp}" \
    OPEN3D_ITEM73_TREE_OUT_ROOT="${APPDIR_OUT_ROOT}" \
    OPEN3D_ITEM73_BASE_VLC_SRC_DIR="${DEFAULT_VLC_SRC_DIR}" \
    OPEN3D_ITEM73_BLURAY_STAGE_DIR="${BUILD_ROOT}/vendor/libbluray/stage" \
    OPEN3D_VLC_BUILD_JOBS="${BUILD_JOBS}" \
      "${ITEM73_HELPER_SCRIPT}" 2>&1
  )" || {
    printf '%s\n' "${helper_output}" | tee -a "${SUMMARY_FILE}"
    exit 1
  }

  printf '%s\n' "${helper_output}" | tee -a "${SUMMARY_FILE}"
  ITEM73_REFRESH_TREE_DIR="$(printf '%s\n' "${helper_output}" | awk -F= '/^TREE_DIR=/{print $2}' | tail -n 1)"

  if [[ -z "${ITEM73_REFRESH_TREE_DIR}" || ! -d "${ITEM73_REFRESH_TREE_DIR}" ]]; then
    log "Failed to resolve helper-produced VLC source tree"
    exit 1
  fi

  VLC_SRC_DIR="${ITEM73_REFRESH_TREE_DIR}"
}

prepare_item73_min_companion_refresh_tree() {
  local helper_stamp helper_output

  [[ "${ITEM73_MIN_COMPANION_REFRESH}" == "1" ]] || return 0

  if [[ "${ITEM73_ACCESS_PLUS_HEADER_REFRESH}" == "1" ]]; then
    log "Cannot combine OPEN3D_APPIMAGE_ITEM73_MIN_COMPANION_REFRESH=1 with OPEN3D_APPIMAGE_ITEM73_ACCESS_PLUS_HEADER_REFRESH=1"
    exit 1
  fi

  if [[ -n "${VLC_SRC_DIR_OVERRIDE}" ]]; then
    log "Cannot combine OPEN3D_APPIMAGE_ITEM73_MIN_COMPANION_REFRESH=1 with OPEN3D_APPIMAGE_VLC_SRC_DIR"
    exit 1
  fi

  if [[ -n "${ACCESS_PLUGIN_OVERRIDE_PATH}" || -n "${VOUT_PLUGIN_OVERRIDE_PATH}" ]]; then
    log "Cannot combine OPEN3D_APPIMAGE_ITEM73_MIN_COMPANION_REFRESH=1 with plugin override hooks"
    exit 1
  fi

  if [[ ! -x "${ITEM73_MIN_COMPANION_HELPER_SCRIPT}" ]]; then
    log "Missing item 7.3 min-companion helper script: ${ITEM73_MIN_COMPANION_HELPER_SCRIPT}"
    exit 1
  fi

  helper_stamp="${OPEN3D_ITEM73_TREE_RUN_STAMP:-${RUN_STAMP}_item73_min_companion_tree}"
  helper_output="$(
    OPEN3D_ITEM73_TREE_RUN_STAMP="${helper_stamp}" \
    OPEN3D_ITEM73_TREE_OUT_ROOT="${APPDIR_OUT_ROOT}" \
    OPEN3D_ITEM73_BASE_VLC_SRC_DIR="${DEFAULT_VLC_SRC_DIR}" \
    OPEN3D_ITEM73_BLURAY_STAGE_DIR="${BUILD_ROOT}/vendor/libbluray/stage" \
    OPEN3D_VLC_BUILD_JOBS="${BUILD_JOBS}" \
      "${ITEM73_MIN_COMPANION_HELPER_SCRIPT}" 2>&1
  )" || {
    printf '%s\n' "${helper_output}" | tee -a "${SUMMARY_FILE}"
    exit 1
  }

  printf '%s\n' "${helper_output}" | tee -a "${SUMMARY_FILE}"
  ITEM73_REFRESH_TREE_DIR="$(printf '%s\n' "${helper_output}" | awk -F= '/^TREE_DIR=/{print $2}' | tail -n 1)"

  if [[ -z "${ITEM73_REFRESH_TREE_DIR}" || ! -d "${ITEM73_REFRESH_TREE_DIR}" ]]; then
    log "Failed to resolve min-companion helper-produced VLC source tree"
    exit 1
  fi

  VLC_SRC_DIR="${ITEM73_REFRESH_TREE_DIR}"
}

copy_library_family() {
  local source_dir="$1"
  local pattern="$2"
  local dest_dir="$3"
  local matched=0
  shopt -s nullglob
  for file in "${source_dir}"/${pattern}; do
    matched=1
    cp -a "${file}" "${dest_dir}/"
  done
  shopt -u nullglob
  if [[ "${matched}" == "0" ]]; then
    log "Missing library family: ${source_dir}/${pattern}"
    exit 1
  fi
}

copy_first_matching_library_family() {
  local pattern="$1"
  local dest_dir="$2"
  shift 2
  local source_dir
  local matched=0

  for source_dir in "$@"; do
    [[ -d "${source_dir}" ]] || continue
    if compgen -G "${source_dir}/${pattern}" >/dev/null; then
      copy_library_family "${source_dir}" "${pattern}" "${dest_dir}"
      matched=1
    fi
  done

  if [[ "${matched}" == "0" ]]; then
    log "Missing system library family: ${pattern}"
    exit 1
  fi
}

copy_plugin() {
  local plugin_name="$1"
  local category="$2"
  local source_path
  local dest_dir="${APPDIR_DIR}/usr/lib/vlc/plugins/${category}"

  source_path="$(resolve_plugin_source_path "${plugin_name}")"

  if [[ ! -f "${source_path}" ]]; then
    log "Missing plugin artifact: ${source_path}"
    exit 1
  fi

  mkdir -p "${dest_dir}"
  cp -a "${source_path}" "${dest_dir}/"
}

resolve_plugin_source_path() {
  local plugin_name="$1"
  local source_path="${VLC_SRC_DIR}/modules/.libs/${plugin_name}"

  if [[ "${plugin_name}" == "libopen3dbluraymvc_plugin.so" &&
        -n "${ACCESS_PLUGIN_OVERRIDE_PATH}" ]]; then
    source_path="${ACCESS_PLUGIN_OVERRIDE_PATH}"
  fi

  if [[ "${plugin_name}" == "libopen3d_plugin.so" &&
        -n "${VOUT_PLUGIN_OVERRIDE_PATH}" ]]; then
    source_path="${VOUT_PLUGIN_OVERRIDE_PATH}"
  fi

  printf '%s\n' "${source_path}"
}

copy_optional_plugin() {
  local plugin_name="$1"
  local category="$2"
  local source_path

  source_path="$(resolve_plugin_source_path "${plugin_name}")"
  if [[ ! -f "${source_path}" ]]; then
    log "Skipping optional plugin artifact: ${source_path}"
    return 0
  fi

  copy_plugin "${plugin_name}" "${category}"
}

copy_optional_library_family() {
  local source_dir="$1"
  local pattern="$2"
  local dest_dir="$3"

  if ! compgen -G "${source_dir}/${pattern}" >/dev/null; then
    log "Skipping optional library family: ${source_dir}/${pattern}"
    return 0
  fi

  copy_library_family "${source_dir}" "${pattern}" "${dest_dir}"
}

bundle_runtime_closure_if_present() {
  local input_path="$1"

  if [[ ! -e "${input_path}" ]]; then
    log "Skipping optional runtime closure input: ${input_path}"
    return 0
  fi

  bundle_runtime_closure_for_file "${input_path}"
}

copy_required_file() {
  local source_path="$1"
  local dest_path="$2"

  if [[ ! -f "${source_path}" ]]; then
    log "Missing required file: ${source_path}"
    exit 1
  fi

  mkdir -p "$(dirname "${dest_path}")"
  cp -a "${source_path}" "${dest_path}"
}

copy_first_matching_file() {
  local relative_path="$1"
  local dest_root="$2"
  shift 2
  local source_root

  for source_root in "$@"; do
    [[ -d "${source_root}" ]] || continue
    if [[ -f "${source_root}/${relative_path}" ]]; then
      copy_required_file "${source_root}/${relative_path}" "${dest_root}/${relative_path}"
      return 0
    fi
  done

  log "Missing required file: ${relative_path}"
  exit 1
}

should_skip_runtime_closure_copy() {
  local base_name
  base_name="$(basename "$1")"
  case "${base_name}" in
    ld-linux*.so*|libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|libresolv.so*|\
    libutil.so*|libnsl.so*|libcrypt.so*|libanl.so*|libBrokenLocale.so*|\
    libvlccore.so*|libvlc.so*|libbluray.so*|libedge264.so*)
      return 0
      ;;
  esac
  return 1
}

copy_resolved_library_family() {
  local resolved_path="$1"
  local source_dir source_name family_pattern family_key

  source_dir="$(dirname "${resolved_path}")"
  source_name="$(basename "${resolved_path}")"
  family_pattern="${source_name%%.so*}.so*"
  family_key="${source_dir}/${family_pattern}"

  if [[ -n "${RUNTIME_CLOSURE_FAMILIES[${family_key}]:-}" ]]; then
    return 0
  fi

  copy_library_family "${source_dir}" "${family_pattern}" "${APPDIR_DIR}/usr/lib"
  RUNTIME_CLOSURE_FAMILIES["${family_key}"]=1
}

bundle_runtime_closure_for_file() {
  local input_path="$1"
  local resolved_path

  if [[ ! -e "${input_path}" ]]; then
    log "Missing runtime-closure input: ${input_path}"
    exit 1
  fi

  while read -r resolved_path; do
    [[ -n "${resolved_path}" ]] || continue
    [[ -e "${resolved_path}" ]] || continue
    if should_skip_runtime_closure_copy "${resolved_path}"; then
      continue
    fi
    copy_resolved_library_family "${resolved_path}"
  done < <(
    ldd "${input_path}" | awk '
      /=> \// { print $3 }
      /^\// { print $1 }
    ' | sort -u
  )
}

prepare_bundled_system_runtime() {
  local source_dirs=(
    "${REPO_DIR}/local/out/devpkgs/dvbpsi_root/usr/lib/x86_64-linux-gnu"
    /usr/lib/x86_64-linux-gnu
    /lib/x86_64-linux-gnu
  )
  local library_patterns=(
    "libQt5Core.so*"
    "libQt5DBus.so*"
    "libQt5Gui.so*"
    "libQt5Svg.so*"
    "libQt5Widgets.so*"
    "libQt5XcbQpa.so*"
    "libQt5X11Extras.so*"
    "libGL.so*"
    "libGLX.so*"
    "libGLdispatch.so*"
    "libICE.so*"
    "libSM.so*"
    "libX11.so*"
    "libX11-xcb.so*"
    "libXau.so*"
    "libXdmcp.so*"
    "libXrender.so*"
    "libbrotlicommon.so*"
    "libbrotlidec.so*"
    "libbsd.so*"
    "libdouble-conversion.so*"
    "libdvbpsi.so*"
    "libexpat.so*"
    "libffi.so*"
    "libfontconfig.so*"
    "libfreetype.so*"
    "libgcc_s.so*"
    "libglib-2.0.so*"
    "libgraphite2.so*"
    "libharfbuzz.so*"
    "libicudata.so*"
    "libicui18n.so*"
    "libicuuc.so*"
    "libmd.so*"
    "libmd4c.so*"
    "libpcre.so*"
    "libpcre2-16.so*"
    "libpng16.so*"
    "libstdc++.so*"
    "libuuid.so*"
    "libxkbcommon.so*"
    "libxkbcommon-x11.so*"
    "libxcb-icccm.so*"
    "libxcb-image.so*"
    "libxcb-keysyms.so*"
    "libxcb-randr.so*"
    "libxcb-render-util.so*"
    "libxcb-render.so*"
    "libxcb-shape.so*"
    "libxcb-shm.so*"
    "libxcb.so*"
    "libxcb-sync.so*"
    "libxcb-util.so*"
    "libxcb-xfixes.so*"
    "libxcb-xinerama.so*"
    "libxcb-xinput.so*"
    "libxcb-xkb.so*"
    "libz.so*"
  )
  local pattern

  if [[ "${OPEN3D_APPIMAGE_ENABLE_WAYLAND}" == "1" ]]; then
    library_patterns+=("libwayland-client.so*")
  fi

  for pattern in "${library_patterns[@]}"; do
    copy_first_matching_library_family "${pattern}" "${APPDIR_DIR}/usr/lib" "${source_dirs[@]}"
  done
}

copy_playback_plugin_surface() {
  copy_plugin "libfilesystem_plugin.so" "access"
  copy_plugin "libmkv_plugin.so" "demux"
  copy_plugin "libavcodec_plugin.so" "codec"
  copy_plugin "librawvideo_plugin.so" "codec"
  copy_plugin "libpacketizer_dts_plugin.so" "packetizer"
  copy_plugin "libpacketizer_copy_plugin.so" "packetizer"
  copy_plugin "libpulse_plugin.so" "audio_output"
  copy_plugin "libfloat_mixer_plugin.so" "audio_mixer"
  copy_plugin "libaudio_format_plugin.so" "audio_filter"
  copy_plugin "libscaletempo_plugin.so" "audio_filter"
  copy_optional_plugin "libsamplerate_plugin.so" "audio_filter"
  copy_plugin "libfreetype_plugin.so" "text_renderer"
  copy_plugin "libswscale_plugin.so" "video_chroma"
  copy_plugin "libyuvp_plugin.so" "video_chroma"
  copy_plugin "libgl_plugin.so" "video_output"
  copy_plugin "libegl_x11_plugin.so" "video_output"
  copy_plugin "liblogger_plugin.so" "misc"
  copy_plugin "libconsole_logger_plugin.so" "logger"
  copy_plugin "libfile_logger_plugin.so" "logger"
  copy_optional_library_family "${VLC_SRC_DIR}/modules/.libs" "libvlc_pulse.so*" "${APPDIR_DIR}/usr/lib"
}

copy_extended_plugin_surface() {
  copy_optional_plugin "libpacketizer_a52_plugin.so" "packetizer"
  copy_optional_plugin "libdbus_plugin.so" "control"
  copy_optional_plugin "liboldrc_plugin.so" "control"
  copy_optional_plugin "libdirectory_demux_plugin.so" "demux"
  copy_optional_plugin "libequalizer_plugin.so" "audio_filter"
  copy_optional_plugin "libdeinterlace_plugin.so" "video_filter"
  copy_optional_plugin "libsubsdelay_plugin.so" "spu"
  copy_optional_plugin "liblua_plugin.so" "lua"
  copy_optional_plugin "libexport_plugin.so" "misc"
  copy_optional_plugin "libxml_plugin.so" "misc"
  copy_optional_plugin "libalsa_plugin.so" "audio_output"
  copy_optional_plugin "libhttp_plugin.so" "access"
  copy_optional_plugin "libhttps_plugin.so" "access"
  copy_optional_plugin "libftp_plugin.so" "access"
  copy_optional_plugin "libtcp_plugin.so" "access"
  copy_optional_plugin "libudp_plugin.so" "access"
  copy_optional_plugin "librtp_plugin.so" "access"
  copy_optional_plugin "libsdp_plugin.so" "access"
  copy_optional_plugin "libmp4_plugin.so" "demux"
  copy_optional_plugin "libavi_plugin.so" "demux"
  copy_optional_plugin "libogg_plugin.so" "demux"
  copy_optional_plugin "libwav_plugin.so" "demux"
  copy_optional_plugin "libsubtitle_plugin.so" "demux"
  copy_optional_plugin "libsubsdec_plugin.so" "codec"
  copy_optional_plugin "libdvbsub_plugin.so" "codec"
  copy_optional_plugin "libspudec_plugin.so" "codec"
  copy_optional_plugin "libwebvtt_plugin.so" "codec"
  copy_optional_plugin "libxcb_window_plugin.so" "video_output"
  copy_optional_plugin "libglx_plugin.so" "video_output"

  if [[ "${OPEN3D_APPIMAGE_ENABLE_WAYLAND}" == "1" ]]; then
    copy_optional_plugin "libegl_wl_plugin.so" "video_output"
    copy_optional_plugin "libwl_shm_plugin.so" "video_output"
    copy_optional_plugin "libxdg_shell_plugin.so" "video_output"
  fi
}

prepare_playback_runtime_closure() {
  local closure_targets=(
    "${VLC_SRC_DIR}/modules/.libs/libfilesystem_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libmkv_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libavcodec_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libpacketizer_dts_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libpacketizer_copy_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libvlc_pulse.so"
    "${VLC_SRC_DIR}/modules/.libs/libpulse_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libfloat_mixer_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libaudio_format_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libscaletempo_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libfreetype_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libswscale_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libyuvp_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libgl_plugin.so"
    "${VLC_SRC_DIR}/modules/.libs/libegl_x11_plugin.so"
  )
  local target

  for target in "${closure_targets[@]}"; do
    bundle_runtime_closure_for_file "${target}"
  done

  bundle_runtime_closure_if_present "${VLC_SRC_DIR}/modules/.libs/libsamplerate_plugin.so"

  if [[ "${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET}" == "1" ]]; then
    for target in \
      "${VLC_SRC_DIR}/modules/.libs/libdbus_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libpacketizer_a52_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/liboldrc_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libdirectory_demux_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libequalizer_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libdeinterlace_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libsubsdelay_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/liblua_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libexport_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libxml_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libalsa_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libhttp_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libhttps_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libftp_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libtcp_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libudp_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/librtp_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libsdp_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libmp4_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libavi_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libogg_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libwav_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libsubtitle_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libsubsdec_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libdvbsub_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libspudec_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libwebvtt_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libxcb_window_plugin.so" \
      "${VLC_SRC_DIR}/modules/.libs/libglx_plugin.so"; do
      bundle_runtime_closure_if_present "${target}"
    done

    if [[ "${OPEN3D_APPIMAGE_ENABLE_WAYLAND}" == "1" ]]; then
      for target in \
        "${VLC_SRC_DIR}/modules/.libs/libegl_wl_plugin.so" \
        "${VLC_SRC_DIR}/modules/.libs/libwl_shm_plugin.so" \
        "${VLC_SRC_DIR}/modules/.libs/libxdg_shell_plugin.so"; do
        bundle_runtime_closure_if_present "${target}"
      done
    fi
  fi
}

prepare_qt_plugin_tree() {
  local plugin_roots=(
    /usr/lib/x86_64-linux-gnu/qt5/plugins
    /usr/lib/qt5/plugins
  )

  copy_first_matching_file "platforms/libqxcb.so" "${APPDIR_DIR}/usr/lib/qt5/plugins" "${plugin_roots[@]}"
  copy_first_matching_file "platforms/libqoffscreen.so" "${APPDIR_DIR}/usr/lib/qt5/plugins" "${plugin_roots[@]}"
  copy_first_matching_file "platforms/libqminimal.so" "${APPDIR_DIR}/usr/lib/qt5/plugins" "${plugin_roots[@]}"
  copy_first_matching_file "platforminputcontexts/libcomposeplatforminputcontextplugin.so" "${APPDIR_DIR}/usr/lib/qt5/plugins" "${plugin_roots[@]}"
  copy_first_matching_file "iconengines/libqsvgicon.so" "${APPDIR_DIR}/usr/lib/qt5/plugins" "${plugin_roots[@]}"
  copy_first_matching_file "imageformats/libqgif.so" "${APPDIR_DIR}/usr/lib/qt5/plugins" "${plugin_roots[@]}"
  copy_first_matching_file "imageformats/libqico.so" "${APPDIR_DIR}/usr/lib/qt5/plugins" "${plugin_roots[@]}"
  copy_first_matching_file "imageformats/libqjpeg.so" "${APPDIR_DIR}/usr/lib/qt5/plugins" "${plugin_roots[@]}"
  copy_first_matching_file "imageformats/libqsvg.so" "${APPDIR_DIR}/usr/lib/qt5/plugins" "${plugin_roots[@]}"
  if [[ "${OPEN3D_APPIMAGE_ENABLE_WAYLAND}" == "1" ]]; then
    for optional_relative_path in \
      "platforms/libqwayland-egl.so" \
      "platforms/libqwayland-generic.so" \
      "wayland-graphics-integration-client/libqt-plugin-wayland-egl.so" \
      "wayland-shell-integration/libxdg-shell.so"; do
      for plugin_root in "${plugin_roots[@]}"; do
        [[ -d "${plugin_root}" ]] || continue
        if [[ -f "${plugin_root}/${optional_relative_path}" ]]; then
          copy_required_file \
            "${plugin_root}/${optional_relative_path}" \
            "${APPDIR_DIR}/usr/lib/qt5/plugins/${optional_relative_path}"
          break
        fi
      done
    done
  fi

  for optional_relative_path in \
    "xcbglintegrations/libqxcb-egl-integration.so" \
    "xcbglintegrations/libqxcb-glx-integration.so"; do
    for plugin_root in "${plugin_roots[@]}"; do
      [[ -d "${plugin_root}" ]] || continue
      if [[ -f "${plugin_root}/${optional_relative_path}" ]]; then
        copy_required_file \
          "${plugin_root}/${optional_relative_path}" \
          "${APPDIR_DIR}/usr/lib/qt5/plugins/${optional_relative_path}"
        break
      fi
    done
  done
}

prepare_qt_conf() {
  cat >"${APPDIR_DIR}/usr/bin/qt.conf" <<'EOF'
[Paths]
Prefix=..
Plugins=lib/qt5/plugins
Data=share
EOF
}

prepare_share_tree() {
  mkdir -p "${APPDIR_DIR}/usr/share"
  cp -a "${VLC_SRC_DIR}/share" "${APPDIR_DIR}/usr/share/vlc"

  mkdir -p "${APPDIR_DIR}/usr/share/applications"
  cp -a "${REPO_DIR}/packaging/appimage/open3doled-vlc.desktop" \
    "${APPDIR_DIR}/usr/share/applications/open3doled-vlc.desktop"
  cp -a "${REPO_DIR}/packaging/appimage/open3doled-vlc.desktop" \
    "${APPDIR_DIR}/open3doled-vlc.desktop"

  mkdir -p "${APPDIR_DIR}/usr/share/icons/hicolor/256x256/apps"
  cp -a "${VLC_SRC_DIR}/share/icons/256x256/vlc.png" \
    "${APPDIR_DIR}/usr/share/icons/hicolor/256x256/apps/open3doled-vlc.png"
  cp -a "${APPDIR_DIR}/usr/share/icons/hicolor/256x256/apps/open3doled-vlc.png" \
    "${APPDIR_DIR}/.DirIcon"
  cp -a "${APPDIR_DIR}/usr/share/icons/hicolor/256x256/apps/open3doled-vlc.png" \
    "${APPDIR_DIR}/open3doled-vlc.png"
}

prepare_bluray_bdj_assets() {
  local java_dir="${APPDIR_DIR}/usr/share/java"
  local bluray_java_stage_dir="${BLURAY_STAGE_DIR}/share/java"

  mkdir -p "${java_dir}"
  if [[ -d "${bluray_java_stage_dir}" ]] && compgen -G "${bluray_java_stage_dir}/libbluray-j2se-*.jar" >/dev/null; then
    copy_library_family "${bluray_java_stage_dir}" "libbluray-j2se-*.jar" "${java_dir}"
    copy_library_family "${bluray_java_stage_dir}" "libbluray-awt-j2se-*.jar" "${java_dir}"
    return 0
  fi

  copy_library_family "/usr/share/java" "libbluray-j2se-*.jar" "${java_dir}"
  copy_library_family "/usr/share/java" "libbluray-awt-j2se-*.jar" "${java_dir}"
}

prepare_scripts() {
  local dest_dir="${APPDIR_DIR}/usr/lib/open3doled/scripts"
  mkdir -p "${dest_dir}" "${APPDIR_DIR}/usr/bin"
  install -m 0755 "${REPO_DIR}/packaging/appimage/AppRun" "${APPDIR_DIR}/AppRun"
  install -m 0755 "${REPO_DIR}/scripts/open3dctl" "${APPDIR_DIR}/usr/bin/open3dctl"
  install -m 0755 "${REPO_DIR}/scripts/open3d_control_panel.py" "${APPDIR_DIR}/usr/bin/open3d-control-panel"
  install -m 0644 "${REPO_DIR}/scripts/open3d_control_common.py" "${APPDIR_DIR}/usr/bin/open3d_control_common.py"
  install -m 0755 "${REPO_DIR}/scripts/extract_mkv_subtitle_plane_map.py" "${dest_dir}/extract_mkv_subtitle_plane_map.py"
  install -m 0755 "${REPO_DIR}/scripts/open3d_exec_with_state_log.sh" "${dest_dir}/open3d_exec_with_state_log.sh"
  install -m 0755 "${REPO_DIR}/scripts/open3d_launcher_media_helpers.sh" "${dest_dir}/open3d_launcher_media_helpers.sh"
  install -m 0755 "${REPO_DIR}/scripts/open3d_media_routing.sh" "${dest_dir}/open3d_media_routing.sh"
  install -m 0755 "${REPO_DIR}/scripts/open3d_process_isolation.sh" "${dest_dir}/open3d_process_isolation.sh"
  install -m 0755 "${REPO_DIR}/scripts/open3d_x11_window_tuner.sh" "${dest_dir}/open3d_x11_window_tuner.sh"
}

prepare_runtime() {
  rm -rf "${APPDIR_DIR}"
  mkdir -p "${APPDIR_DIR}/usr/bin" "${APPDIR_DIR}/usr/lib"

  cp -a "${VLC_SRC_DIR}/bin/.libs/vlc" "${APPDIR_DIR}/usr/bin/vlc"
  copy_library_family "${VLC_SRC_DIR}/lib/.libs" "libvlc.so*" "${APPDIR_DIR}/usr/lib"
  copy_library_family "${VLC_SRC_DIR}/src/.libs" "libvlccore.so*" "${APPDIR_DIR}/usr/lib"
  copy_library_family "${BLURAY_STAGE_DIR}/lib/x86_64-linux-gnu" "libbluray.so*" "${APPDIR_DIR}/usr/lib"
  copy_library_family "${REPO_DIR}/vendor/edge264" "libedge264.so*" "${APPDIR_DIR}/usr/lib"
  prepare_bundled_system_runtime
  prepare_qt_plugin_tree
  prepare_qt_conf
  bundle_runtime_closure_for_file "${APPDIR_DIR}/usr/lib/qt5/plugins/platforms/libqxcb.so"
  if [[ "${OPEN3D_APPIMAGE_ENABLE_WAYLAND}" == "1" &&
        -f "${APPDIR_DIR}/usr/lib/qt5/plugins/platforms/libqwayland-egl.so" ]]; then
    bundle_runtime_closure_for_file "${APPDIR_DIR}/usr/lib/qt5/plugins/platforms/libqwayland-egl.so"
  fi
  if [[ -f "${APPDIR_DIR}/usr/lib/qt5/plugins/platforms/libqwayland-generic.so" ]]; then
    bundle_runtime_closure_for_file "${APPDIR_DIR}/usr/lib/qt5/plugins/platforms/libqwayland-generic.so"
  fi

  copy_plugin "libdummy_plugin.so" "control"
  copy_plugin "libhotkeys_plugin.so" "control"
  copy_plugin "libxcb_hotkeys_plugin.so" "control"
  copy_plugin "libqt_plugin.so" "gui"
  copy_plugin "libopen3d_plugin.so" "video_output"
  copy_plugin "libedge264mvc_plugin.so" "codec"
  copy_plugin "libopen3dannexb_plugin.so" "demux"
  copy_plugin "libplaylist_plugin.so" "demux"
  copy_plugin "libts_plugin.so" "demux"
  copy_plugin "libtsmvc_plugin.so" "demux"
  copy_plugin "libopen3dbluraymvc_plugin.so" "access"
  copy_playback_plugin_surface
  if [[ "${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET}" == "1" ]]; then
    copy_extended_plugin_surface
  fi
  prepare_playback_runtime_closure

  prepare_share_tree
  prepare_bluray_bdj_assets
  prepare_scripts
}

verify_appdir() {
  local verify_user="${OPEN3D_APPIMAGE_VERIFY_USER:-nobody}"
  mkdir -p "${RUN_DIR}/home" "${RUN_DIR}/xdg-config" "${RUN_DIR}/xdg-cache"
  chmod 0777 "${RUN_DIR}/home" "${RUN_DIR}/xdg-config" "${RUN_DIR}/xdg-cache"
  (
    set -x
    su -s /bin/bash "${verify_user}" -c "
      env \
        HOME='${RUN_DIR}/home' \
        XDG_CONFIG_HOME='${RUN_DIR}/xdg-config' \
        XDG_CACHE_HOME='${RUN_DIR}/xdg-cache' \
        QT_QPA_PLATFORM=offscreen \
        '${APPDIR_DIR}/AppRun' --version
    "
  ) >"${VERIFY_LOG}" 2>&1
}

log "Open3DOLED AppDir assembly"
log "repo=${REPO_DIR}"
log "vlc_version=${VLC_VERSION}"
log "item73_access_plus_header_refresh=${ITEM73_ACCESS_PLUS_HEADER_REFRESH}"
log "item73_min_companion_refresh=${ITEM73_MIN_COMPANION_REFRESH}"

prepare_item73_access_plus_header_refresh_tree
prepare_item73_min_companion_refresh_tree

log "vlc_src=${VLC_SRC_DIR}"
log "item73_refresh_tree_dir=${ITEM73_REFRESH_TREE_DIR}"
log "access_plugin_override=${ACCESS_PLUGIN_OVERRIDE_PATH}"
log "vout_plugin_override=${VOUT_PLUGIN_OVERRIDE_PATH}"
log "build_jobs=${BUILD_JOBS}"
log "skip_fullstack_build=${SKIP_FULLSTACK_BUILD}"
log "skip_verify=${SKIP_VERIFY}"
log "extended_module_set=${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET}"

if [[ "${SKIP_FULLSTACK_BUILD}" != "1" ]]; then
  log "Running matched fullstack build first"
  OPEN3D_APPIMAGE_RUN_STAMP="${RUN_STAMP}" \
  OPEN3D_VLC_BUILD_JOBS="${BUILD_JOBS}" \
  OPEN3D_APPIMAGE_EXTENDED_MODULE_SET="${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET}" \
    "${REPO_DIR}/scripts/build_open3d_appimage_fullstack.sh"
fi

if [[ ! -x "${VLC_SRC_DIR}/bin/.libs/vlc" ]]; then
  log "Missing VLC binary after bounded build: ${VLC_SRC_DIR}/bin/.libs/vlc"
  exit 1
fi

prepare_runtime
if [[ "${SKIP_VERIFY}" != "1" ]]; then
  verify_appdir
else
  log "Skipping AppDir verify step"
fi

find "${APPDIR_DIR}" -type f | sed "s#^${APPDIR_DIR}/##" | sort >"${MANIFEST_FILE}"

log "AppDir: ${APPDIR_DIR}"
log "Manifest: ${MANIFEST_FILE}"
log "Verification log: ${VERIFY_LOG}"
log "AppDir assembly completed successfully."
