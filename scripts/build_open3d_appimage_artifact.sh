#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

VLC_VERSION="${OPEN3D_VLC_VERSION:-3.0.23}"
APPIMAGE_ROOT="${OPEN3D_APPIMAGE_WORK_ROOT:-/opt/open3doled-appimage}"
BUILD_ROOT="${APPIMAGE_ROOT}/build"
TOOLS_DIR="${APPIMAGE_ROOT}/tools"
BLURAY_STAGE_DIR="${OPEN3D_LIBBLURAY_STAGE_DIR:-${BUILD_ROOT}/vendor/libbluray/stage}"
RUN_STAMP="${OPEN3D_APPIMAGE_RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
APPDIR_OUT_ROOT="${OPEN3D_APPIMAGE_APPDIR_OUT_ROOT:-/out/appdir-builds}"
APPIMAGE_OUT_ROOT="${OPEN3D_APPIMAGE_ARTIFACT_OUT_ROOT:-/out/appimage-builds}"
RUN_DIR="${APPIMAGE_OUT_ROOT}/${RUN_STAMP}"
SUMMARY_FILE="${RUN_DIR}/summary.txt"
APPIMAGETOOL_LOG="${RUN_DIR}/appimagetool.log"
VERIFY_LOG="${RUN_DIR}/appimage-verify.log"
SKIP_APPDIR_BUILD="${OPEN3D_APPIMAGE_SKIP_APPDIR_BUILD:-0}"
APPDIR_DIR="${OPEN3D_APPIMAGE_APPDIR_PATH:-${APPDIR_OUT_ROOT}/${RUN_STAMP}/AppDir}"
APPIMAGETOOL_URL="${OPEN3D_APPIMAGETOOL_URL:-https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage}"
APPIMAGETOOL_BIN="${TOOLS_DIR}/appimagetool-x86_64.AppImage"
APPIMAGETOOL_ROOT="${TOOLS_DIR}/appimagetool-root"
APPIMAGETOOL_RUN="${APPIMAGETOOL_ROOT}/AppRun"
APPIMAGE_NAME="${OPEN3D_APPIMAGE_NAME:-Open3DOLED-VLC-${VLC_VERSION}-x86_64.AppImage}"
APPIMAGE_PATH="${RUN_DIR}/${APPIMAGE_NAME}"
VERIFY_USER="${OPEN3D_APPIMAGE_VERIFY_USER:-nobody}"

mkdir -p "${RUN_DIR}"

log() {
  printf '%s\n' "$*" | tee -a "${SUMMARY_FILE}"
}

copy_bundled_bdj_jars() {
  local java_dir="${APPDIR_DIR}/usr/share/java"
  local bluray_java_stage_dir="${BLURAY_STAGE_DIR}/share/java"

  mkdir -p "${java_dir}"
  if [[ -d "${bluray_java_stage_dir}" ]] && compgen -G "${bluray_java_stage_dir}/libbluray-j2se-*.jar" >/dev/null; then
    cp -a "${bluray_java_stage_dir}"/libbluray-j2se-*.jar "${java_dir}/"
    cp -a "${bluray_java_stage_dir}"/libbluray-awt-j2se-*.jar "${java_dir}/"
    return 0
  fi

  cp -a /usr/share/java/libbluray-j2se-*.jar "${java_dir}/"
  cp -a /usr/share/java/libbluray-awt-j2se-*.jar "${java_dir}/"
}

extract_embedded_appimage() {
  local image_path="$1"
  local dest_dir="$2"
  local log_path="$3"
  local offset
  local extracted=1
  mapfile -t offsets < <(grep -aob 'hsqs' "${image_path}" | cut -d: -f1)

  if [[ "${#offsets[@]}" -eq 0 ]]; then
    return 1
  fi

  rm -rf "${dest_dir}"
  for (( idx=${#offsets[@]}-1; idx>=0; idx-- )); do
    offset="${offsets[idx]}"
    if unsquashfs -d "${dest_dir}" -o "${offset}" "${image_path}" >"${log_path}" 2>&1; then
      if [[ -x "${dest_dir}/AppRun" ]]; then
        printf '%s\n' "${offset}"
        return 0
      fi
      rm -rf "${dest_dir}"
    fi
  done

  return "${extracted}"
}

download_appimagetool() {
  mkdir -p "${TOOLS_DIR}"
  curl -L --fail --output "${APPIMAGETOOL_BIN}" "${APPIMAGETOOL_URL}"
  chmod 0755 "${APPIMAGETOOL_BIN}"
}

extract_appimagetool() {
  local offset
  if ! offset="$(
    extract_embedded_appimage \
      "${APPIMAGETOOL_BIN}" \
      "${APPIMAGETOOL_ROOT}" \
      "${RUN_DIR}/appimagetool-extract.log"
  )"; then
    log "Could not extract a runnable appimagetool payload from ${APPIMAGETOOL_BIN}"
    exit 1
  fi
  log "Using extracted appimagetool at offset=${offset}"
}

verify_appimage() {
  local extract_dir="${RUN_DIR}/appimage-verify-extract"
  local extract_log="${RUN_DIR}/appimage-verify-extract.log"
  mkdir -p "${RUN_DIR}/home" "${RUN_DIR}/xdg-config" "${RUN_DIR}/xdg-cache"
  chmod 0777 "${RUN_DIR}/home" "${RUN_DIR}/xdg-config" "${RUN_DIR}/xdg-cache"
  if (
    set -x
    su -s /bin/bash "${VERIFY_USER}" -c "
      env \
        HOME='${RUN_DIR}/home' \
        XDG_CONFIG_HOME='${RUN_DIR}/xdg-config' \
        XDG_CACHE_HOME='${RUN_DIR}/xdg-cache' \
        QT_QPA_PLATFORM=offscreen \
        APPIMAGE_EXTRACT_AND_RUN=1 \
        '${APPIMAGE_PATH}' --version
    "
  ) >"${VERIFY_LOG}" 2>&1; then
    return
  fi

  printf '%s\n' \
    "Direct AppImage execution failed inside Docker; retrying via extracted AppRun." \
    >>"${VERIFY_LOG}"

  if ! extract_embedded_appimage "${APPIMAGE_PATH}" "${extract_dir}" "${extract_log}" >/dev/null; then
    return 1
  fi

  (
    set -x
    su -s /bin/bash "${VERIFY_USER}" -c "
      env \
        HOME='${RUN_DIR}/home' \
        XDG_CONFIG_HOME='${RUN_DIR}/xdg-config' \
        XDG_CACHE_HOME='${RUN_DIR}/xdg-cache' \
        QT_QPA_PLATFORM=offscreen \
        '${extract_dir}/AppRun' --version
    "
  ) >>"${VERIFY_LOG}" 2>&1
}

sync_appdir_runtime_assets() {
  local script_dest="${APPDIR_DIR}/usr/lib/open3doled/scripts"

  mkdir -p "${script_dest}" "${APPDIR_DIR}/usr/share/applications"
  install -m 0755 "${REPO_DIR}/packaging/appimage/AppRun" "${APPDIR_DIR}/AppRun"
  install -m 0755 "${REPO_DIR}/scripts/extract_mkv_subtitle_plane_map.py" \
    "${script_dest}/extract_mkv_subtitle_plane_map.py"
  install -m 0755 "${REPO_DIR}/scripts/open3d_exec_with_state_log.sh" \
    "${script_dest}/open3d_exec_with_state_log.sh"
  install -m 0755 "${REPO_DIR}/scripts/open3d_launcher_media_helpers.sh" \
    "${script_dest}/open3d_launcher_media_helpers.sh"
  install -m 0755 "${REPO_DIR}/scripts/open3d_media_routing.sh" \
    "${script_dest}/open3d_media_routing.sh"
  install -m 0755 "${REPO_DIR}/scripts/open3d_process_isolation.sh" \
    "${script_dest}/open3d_process_isolation.sh"
  install -m 0755 "${REPO_DIR}/scripts/open3d_x11_window_tuner.sh" \
    "${script_dest}/open3d_x11_window_tuner.sh"
  cp -a "${REPO_DIR}/packaging/appimage/open3doled-vlc.desktop" \
    "${APPDIR_DIR}/open3doled-vlc.desktop"
  cp -a "${REPO_DIR}/packaging/appimage/open3doled-vlc.desktop" \
    "${APPDIR_DIR}/usr/share/applications/open3doled-vlc.desktop"
  copy_bundled_bdj_jars
}

prepare_appdir_metadata() {
  local desktop_file="${APPDIR_DIR}/open3doled-vlc.desktop"
  local icon_name
  local icon_path

  if [[ ! -f "${desktop_file}" ]]; then
    return
  fi

  icon_name="$(sed -n 's/^Icon=//p' "${desktop_file}" | head -n1)"
  if [[ -z "${icon_name}" ]]; then
    return
  fi

  icon_path="${APPDIR_DIR}/${icon_name}.png"
  if [[ -f "${icon_path}" ]]; then
    return
  fi

  if [[ -f "${APPDIR_DIR}/.DirIcon" ]]; then
    cp -a "${APPDIR_DIR}/.DirIcon" "${icon_path}"
    log "Filled missing AppDir icon from .DirIcon: ${icon_path}"
  fi
}

log "Open3DOLED AppImage artifact build"
log "repo=${REPO_DIR}"
log "vlc_version=${VLC_VERSION}"
log "run_stamp=${RUN_STAMP}"
log "skip_appdir_build=${SKIP_APPDIR_BUILD}"
log "appimagetool_url=${APPIMAGETOOL_URL}"

if [[ "${SKIP_APPDIR_BUILD}" != "1" ]]; then
  log "Running AppDir build first"
  OPEN3D_APPIMAGE_RUN_STAMP="${RUN_STAMP}" \
    "${REPO_DIR}/scripts/build_open3d_appimage_appdir.sh"
fi

if [[ ! -d "${APPDIR_DIR}" ]]; then
  log "Missing AppDir: ${APPDIR_DIR}"
  exit 1
fi

sync_appdir_runtime_assets
prepare_appdir_metadata
download_appimagetool
extract_appimagetool

(
  set -x
  env \
    ARCH=x86_64 \
    VERSION="${VLC_VERSION}" \
    "${APPIMAGETOOL_RUN}" \
    "${APPDIR_DIR}" \
    "${APPIMAGE_PATH}"
) >"${APPIMAGETOOL_LOG}" 2>&1

if [[ ! -f "${APPIMAGE_PATH}" ]]; then
  log "Missing AppImage artifact: ${APPIMAGE_PATH}"
  exit 1
fi

chmod 0755 "${APPIMAGE_PATH}"
verify_appimage

log "AppImage: ${APPIMAGE_PATH}"
log "AppImage tool log: ${APPIMAGETOOL_LOG}"
log "Verification log: ${VERIFY_LOG}"
log "AppImage artifact build completed successfully."
