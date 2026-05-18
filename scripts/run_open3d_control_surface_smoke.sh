#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_open3d_control_surface_smoke.sh

Runs a bounded host smoke for the packaged Open3D control surface.

Env overrides:
  OPEN3D_APPIMAGE_APPDIR=/path/to/AppDir
  OPEN3D_CONTROL_SMOKE_APPRUN=/path/to/AppRun
  OPEN3D_CONTROL_SMOKE_RUN_ROOT=/path/to/output-root
  OPEN3D_CONTROL_SMOKE_PANEL_TIMEOUT=5
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

discover_latest_appdir() {
  find "${REPO_DIR}/local/out/appimage/appdir-builds" \
    -mindepth 2 -maxdepth 2 -type d -name AppDir \
    -printf '%T@ %p\n' 2>/dev/null | sort -nr | awk 'NR==1 {print $2}'
}

APP_RUN="${OPEN3D_CONTROL_SMOKE_APPRUN:-}"
APPDIR_PATH="${OPEN3D_APPIMAGE_APPDIR:-}"
if [[ -z "${APP_RUN}" ]]; then
  if [[ -z "${APPDIR_PATH}" ]]; then
    APPDIR_PATH="$(discover_latest_appdir || true)"
  fi
  if [[ -z "${APPDIR_PATH}" || ! -d "${APPDIR_PATH}" ]]; then
    echo "Error: AppDir not found. Build one first or set OPEN3D_APPIMAGE_APPDIR." >&2
    exit 2
  fi
  APP_RUN="${APPDIR_PATH}/AppRun"
fi

if [[ ! -x "${APP_RUN}" ]]; then
  echo "Error: AppRun is not executable: ${APP_RUN}" >&2
  exit 2
fi

RUN_STAMP="$(date +%Y%m%d_%H%M%S)"
RUN_ROOT="${OPEN3D_CONTROL_SMOKE_RUN_ROOT:-${REPO_DIR}/local/out/appimage/control-surface-smoke}"
RUN_DIR="${RUN_ROOT}/${RUN_STAMP}"
LOG_PATH="${RUN_DIR}/smoke.log"
HOME_DIR="${RUN_DIR}/home"
RUNTIME_DIR="${RUN_DIR}/runtime"

mkdir -p "${HOME_DIR}" "${RUNTIME_DIR}"
chmod 700 "${RUNTIME_DIR}"

run_with_control_env() {
  env \
    HOME="${HOME_DIR}" \
    XDG_RUNTIME_DIR="${RUNTIME_DIR}" \
    APPIMAGELAUNCHER_DISABLE=1 \
    OPEN3D_PROCESS_CPU_BACKEND=taskset \
    OPEN3D_PROCESS_CPUSET=0-1 \
    "$@"
}

run_control_logged() {
  echo "+ $*" | tee -a "${LOG_PATH}"
  run_with_control_env "$@" 2>&1 | tee -a "${LOG_PATH}"
}

{
  echo "Open3D control surface smoke"
  echo "apprun=${APP_RUN}"
  if [[ -n "${APPDIR_PATH}" ]]; then
    echo "appdir=${APPDIR_PATH}"
  fi
  echo "run_dir=${RUN_DIR}"
} | tee "${LOG_PATH}"

run_control_logged "${APP_RUN}" --open3dctl --help >/dev/null
run_control_logged "${APP_RUN}" --open3dctl paths | grep -q '"display_settings"'
run_control_logged "${APP_RUN}" --open3dctl profile-list | grep -q '"profiles"'
run_control_logged "${APP_RUN}" --open3d-control-panel --help >/dev/null

if [[ -x "${SCRIPT_DIR}/run_open3dctl_protocol_smoke.py" ]]; then
  echo "+ ${SCRIPT_DIR}/run_open3dctl_protocol_smoke.py --client ${APP_RUN} --client-arg=--open3dctl" | tee -a "${LOG_PATH}"
  "${SCRIPT_DIR}/run_open3dctl_protocol_smoke.py" \
    --client "${APP_RUN}" \
    --client-arg=--open3dctl \
    2>&1 | tee -a "${LOG_PATH}"
fi

if command -v xvfb-run >/dev/null 2>&1; then
  panel_timeout="${OPEN3D_CONTROL_SMOKE_PANEL_TIMEOUT:-5}"
  echo "+ timeout ${panel_timeout}s xvfb-run -a ${APP_RUN} --open3d-control-panel" | tee -a "${LOG_PATH}"
  set +e
  run_with_control_env timeout "${panel_timeout}s" xvfb-run -a "${APP_RUN}" --open3d-control-panel \
    >>"${LOG_PATH}" 2>&1
  panel_status=$?
  set -e
  if [[ "${panel_status}" != "124" ]]; then
    echo "Error: control panel smoke exited with ${panel_status}, expected timeout 124" | tee -a "${LOG_PATH}" >&2
    exit 1
  fi
  echo "control_panel_timeout=expected" | tee -a "${LOG_PATH}"
else
  echo "xvfb-run unavailable; skipped control panel startup smoke" | tee -a "${LOG_PATH}"
fi

echo "Smoke log: ${LOG_PATH}"
