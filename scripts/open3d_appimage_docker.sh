#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: open3d_appimage_docker.sh <build-image|shell|exec> [args...]

Commands:
  build-image         Build the Docker image for the AppImage prototype.
  clean-cache         Remove the persistent Docker-side AppImage build cache.
  shell               Start an interactive shell in the container.
  exec -- <command>   Run a command in the container.

Env overrides:
  OPEN3D_APPIMAGE_DOCKER_IMAGE=open3doled-appimage-builder:debian11
  OPEN3D_APPIMAGE_OUT_DIR=/host/path/for/appimage/artifacts
  OPEN3D_APPIMAGE_CACHE_DIR=/host/path/for/persistent/docker-build-cache
EOF
}

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="${OPEN3D_APPIMAGE_DOCKER_IMAGE:-open3doled-appimage-builder:debian11}"
DOCKERFILE_DIR="${REPO_DIR}/packaging/appimage"
OUT_DIR="${OPEN3D_APPIMAGE_OUT_DIR:-${REPO_DIR}/local/out/appimage}"
CACHE_DIR="${OPEN3D_APPIMAGE_CACHE_DIR:-${REPO_DIR}/local/cache/appimage-docker}"

build_image() {
  docker build \
    -t "${IMAGE_NAME}" \
    -f "${DOCKERFILE_DIR}/Dockerfile" \
    "${DOCKERFILE_DIR}"
}

run_container() {
  mkdir -p "${OUT_DIR}" "${CACHE_DIR}"
  docker run --rm \
    -v "${REPO_DIR}:/work" \
    -v "${OUT_DIR}:/out" \
    -v "${CACHE_DIR}:/opt/open3doled-appimage" \
    -w /work \
    "$@"
}

clean_cache() {
  rm -rf "${CACHE_DIR}"
}

if [[ $# -lt 1 ]]; then
  usage >&2
  exit 1
fi

case "$1" in
  -h|--help)
    usage
    exit 0
    ;;
  build-image)
    shift
    if [[ $# -ne 0 ]]; then
      usage >&2
      exit 1
    fi
    build_image
    ;;
  clean-cache)
    shift
    if [[ $# -ne 0 ]]; then
      usage >&2
      exit 1
    fi
    clean_cache
    ;;
  shell)
    shift
    if [[ $# -ne 0 ]]; then
      usage >&2
      exit 1
    fi
    run_container -it "${IMAGE_NAME}" /bin/bash
    ;;
  exec)
    shift
    if [[ $# -lt 1 || "$1" != "--" ]]; then
      usage >&2
      exit 1
    fi
    shift
    if [[ $# -lt 1 ]]; then
      usage >&2
      exit 1
    fi
    run_container "${IMAGE_NAME}" "$@"
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac
