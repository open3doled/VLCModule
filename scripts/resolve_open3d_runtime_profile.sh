#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: resolve_open3d_runtime_profile.sh [stable|live]

Prints shell-compatible KEY=VALUE lines describing the resolved runtime profile.

Output keys:
  runtime_profile
  plugins_path
  edge264_lib
EOF
}

if [[ $# -gt 1 ]]; then
  usage >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
profile="${1:-${OPEN3D_MVC_RUNTIME_PROFILE:-stable}}"

case "${profile}" in
  stable|live)
    ;;
  -h|--help)
    usage
    exit 0
    ;;
  *)
    echo "Error: runtime profile must be 'stable' or 'live' (got '${profile}')." >&2
    exit 2
    ;;
esac

stable_runtime_root_default="${REPO_DIR}/local/out/stable_runtime/latest"
legacy_stable_runtime_root="${REPO_DIR}/local/out/runtime_stock_rebuild_latest"
stable_runtime_root="${OPEN3D_MVC_STABLE_RUNTIME_ROOT:-${stable_runtime_root_default}}"
if [[ -z "${OPEN3D_MVC_STABLE_RUNTIME_ROOT:-}" && ! -d "${stable_runtime_root}" && -d "${legacy_stable_runtime_root}" ]]; then
  stable_runtime_root="${legacy_stable_runtime_root}"
fi
stable_plugins="${stable_runtime_root}/plugins"
stable_edge264_lib="${stable_runtime_root}/lib/libedge264.so.1"
live_plugins="${REPO_DIR}/local/out/runtime_plugins_live"
vendor_edge264_ndebug="${REPO_DIR}/vendor/edge264/libedge264_ndebug.so.1"

plugins_path=""
edge264_lib=""

if [[ "${profile}" == "stable" ]]; then
  plugins_path="${stable_plugins}"
  edge264_lib="${stable_edge264_lib}"
else
  plugins_path="${live_plugins}"
  if [[ -f "${stable_edge264_lib}" ]]; then
    edge264_lib="${stable_edge264_lib}"
  elif [[ -f "${vendor_edge264_ndebug}" ]]; then
    edge264_lib="${vendor_edge264_ndebug}"
  fi
fi

printf 'runtime_profile=%s\n' "${profile}"
printf 'plugins_path=%s\n' "${plugins_path}"
printf 'edge264_lib=%s\n' "${edge264_lib}"
