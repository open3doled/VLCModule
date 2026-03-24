#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sync_open3doled_repo_branches.sh [--dry-run] [parent-branch]
  sync_open3doled_repo_branches.sh --new-project <branch-name>

Sync the VLCModule child repos to the expected branch names for the selected
parent VLCModule branch.

Create a new project branch across VLCModule, edge264, libbluray, and local by
branching from the current mapped working branches.

Branch mapping:
  main                  -> edge264=open3doled_001, libbluray=open3doled_001, local=main
  open3doled_*          -> matching child branch when it exists, otherwise the
                           canonical fallback above

Examples:
  ./scripts/sync_open3doled_repo_branches.sh
  ./scripts/sync_open3doled_repo_branches.sh main
  ./scripts/sync_open3doled_repo_branches.sh --dry-run open3doled_003_distribution
  ./scripts/sync_open3doled_repo_branches.sh --new-project ui_cleanup
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

DRY_RUN=0
PARENT_BRANCH=""
NEW_PROJECT_BRANCH=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --new-project)
      shift
      if [[ $# -lt 1 || -n "${NEW_PROJECT_BRANCH}" ]]; then
        usage >&2
        exit 1
      fi
      NEW_PROJECT_BRANCH="$1"
      shift
      ;;
    *)
      if [[ -n "${PARENT_BRANCH}" || -n "${NEW_PROJECT_BRANCH}" ]]; then
        usage >&2
        exit 1
      fi
      PARENT_BRANCH="$1"
      shift
      ;;
  esac
done

if [[ -z "${PARENT_BRANCH}" ]]; then
  PARENT_BRANCH="$(git -C "${REPO_DIR}" rev-parse --abbrev-ref HEAD)"
fi

is_branch_supported() {
  local branch="$1"
  [[ "${branch}" == "main" || "${branch}" == open3doled_* ]]
}

normalize_project_branch() {
  local branch="$1"

  if [[ "${branch}" == open3doled_* ]]; then
    printf '%s\n' "${branch}"
    return 0
  fi

  if [[ "${branch}" == "main" ]]; then
    printf '%s\n' "${branch}"
    return 0
  fi

  printf 'open3doled_%s\n' "${branch}"
}

repo_has_branch() {
  local repo_path="$1"
  local branch="$2"
  git -C "${repo_path}" show-ref --verify --quiet "refs/heads/${branch}"
}

repo_is_clean() {
  local repo_path="$1"
  [[ -z "$(git -C "${repo_path}" status --porcelain)" ]]
}

resolve_target_branch() {
  local repo_path="$1"
  local fallback_branch="$2"

  if repo_has_branch "${repo_path}" "${PARENT_BRANCH}"; then
    printf '%s\n' "${PARENT_BRANCH}"
  else
    printf '%s\n' "${fallback_branch}"
  fi
}

sync_repo_branch() {
  local label="$1"
  local repo_path="$2"
  local fallback_branch="$3"
  local current_branch=""
  local target_branch=""

  current_branch="$(git -C "${repo_path}" rev-parse --abbrev-ref HEAD)"
  target_branch="$(resolve_target_branch "${repo_path}" "${fallback_branch}")"

  printf '%s: current=%s target=%s\n' "${label}" "${current_branch}" "${target_branch}"

  if [[ "${current_branch}" == "${target_branch}" ]]; then
    return 0
  fi

  if ! repo_is_clean "${repo_path}"; then
    printf 'error: %s has uncommitted changes; refusing to switch branches\n' "${label}" >&2
    return 1
  fi

  if [[ "${DRY_RUN}" -eq 1 ]]; then
    return 0
  fi

  git -C "${repo_path}" checkout "${target_branch}" >/dev/null
}

create_or_checkout_repo_branch() {
  local label="$1"
  local repo_path="$2"
  local target_branch="$3"
  local current_branch=""

  current_branch="$(git -C "${repo_path}" rev-parse --abbrev-ref HEAD)"
  printf '%s: base=%s new=%s\n' "${label}" "${current_branch}" "${target_branch}"

  if ! repo_is_clean "${repo_path}"; then
    printf 'error: %s has uncommitted changes; refusing to create or switch branches\n' "${label}" >&2
    return 1
  fi

  if [[ "${DRY_RUN}" -eq 1 ]]; then
    return 0
  fi

  if repo_has_branch "${repo_path}" "${target_branch}"; then
    git -C "${repo_path}" checkout "${target_branch}" >/dev/null
  else
    git -C "${repo_path}" checkout -b "${target_branch}" >/dev/null
  fi
}

if ! is_branch_supported "${PARENT_BRANCH}"; then
  printf 'error: unsupported parent branch "%s"\n' "${PARENT_BRANCH}" >&2
  printf 'supported parent branches: main, open3doled_*\n' >&2
  exit 1
fi

if [[ -n "${NEW_PROJECT_BRANCH}" ]]; then
  NEW_PROJECT_BRANCH="$(normalize_project_branch "${NEW_PROJECT_BRANCH}")"
  if ! is_branch_supported "${NEW_PROJECT_BRANCH}"; then
    printf 'error: unsupported project branch "%s"\n' "${NEW_PROJECT_BRANCH}" >&2
    exit 1
  fi
  if ! repo_is_clean "${REPO_DIR}"; then
    printf 'error: VLCModule has uncommitted changes; refusing to create or switch branches\n' >&2
    exit 1
  fi
fi

sync_repo_branch "vendor/edge264" "${REPO_DIR}/vendor/edge264" "open3doled_001"
sync_repo_branch "vendor/libbluray" "${REPO_DIR}/vendor/libbluray" "open3doled_001"
sync_repo_branch "local" "${REPO_DIR}/local" "main"

if [[ -n "${NEW_PROJECT_BRANCH}" ]]; then
  printf 'VLCModule: base=%s new=%s\n' "${PARENT_BRANCH}" "${NEW_PROJECT_BRANCH}"
  if [[ "${DRY_RUN}" -eq 0 ]]; then
    if repo_has_branch "${REPO_DIR}" "${NEW_PROJECT_BRANCH}"; then
      git -C "${REPO_DIR}" checkout "${NEW_PROJECT_BRANCH}" >/dev/null
    else
      git -C "${REPO_DIR}" checkout -b "${NEW_PROJECT_BRANCH}" >/dev/null
    fi
    PARENT_BRANCH="${NEW_PROJECT_BRANCH}"
  fi
  create_or_checkout_repo_branch "vendor/edge264" "${REPO_DIR}/vendor/edge264" "${NEW_PROJECT_BRANCH}"
  create_or_checkout_repo_branch "vendor/libbluray" "${REPO_DIR}/vendor/libbluray" "${NEW_PROJECT_BRANCH}"
  create_or_checkout_repo_branch "local" "${REPO_DIR}/local" "${NEW_PROJECT_BRANCH}"
fi
