#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [edge264-repo-path]" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
EDGE264_REPO="${1:-${REPO_DIR}/vendor/edge264}"

if [[ ! -d "${EDGE264_REPO}" || ( ! -d "${EDGE264_REPO}/.git" && ! -f "${EDGE264_REPO}/.git" ) ]]; then
  echo "Error: edge264 repo not found or not a git checkout: ${EDGE264_REPO}" >&2
  exit 2
fi

(
  cd "${EDGE264_REPO}"
  make clean >/dev/null 2>&1 || true
  make BUILD_TEST=yes VARIANTS=logs CFLAGS='-DNDEBUG'
  cp -f libedge264.so.1 libedge264_ndebug.so.1
)

echo "Built: ${EDGE264_REPO}/libedge264_ndebug.so.1"
