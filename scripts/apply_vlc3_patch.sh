#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/vlc-3.0.23-source" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PATCH_DIR="${REPO_DIR}/patches/vlc-3.0.23"
VLC_SRC="$(cd "$1" && pwd)"

if [[ ! -d "${VLC_SRC}/modules/video_output" ]]; then
  echo "Error: ${VLC_SRC} does not look like a VLC source tree" >&2
  exit 2
fi

mapfile -t PATCH_FILES < <(find "${PATCH_DIR}" -maxdepth 1 -type f -name '*.patch' | sort)
if [[ ${#PATCH_FILES[@]} -eq 0 ]]; then
  echo "Error: no patch files found in ${PATCH_DIR}" >&2
  exit 3
fi

patch_already_present() {
  local patch_file="$1"
  local -a patch_targets=()
  local -a added_lines=()

  mapfile -t patch_targets < <(awk '/^\+\+\+ b\// {print substr($0, 7)}' "${patch_file}")
  if [[ ${#patch_targets[@]} -eq 0 ]]; then
    return 1
  fi
  for rel in "${patch_targets[@]}"; do
    if [[ ! -f "${VLC_SRC}/${rel}" ]]; then
      return 1
    fi
  done

  mapfile -t added_lines < <(
    awk '
      /^\+\+\+ / {next}
      /^\+[^+]/ {
        line = substr($0, 2)
        if (line ~ /^[[:space:]]*$/) next
        print line
      }
    ' "${patch_file}"
  )
  if [[ ${#added_lines[@]} -eq 0 ]]; then
    return 1
  fi

  for line in "${added_lines[@]}"; do
    local found=0
    for rel in "${patch_targets[@]}"; do
      if grep -Fq -- "${line}" "${VLC_SRC}/${rel}"; then
        found=1
        break
      fi
    done
    if [[ "${found}" -eq 0 ]]; then
      return 1
    fi
  done
  return 0
}

cd "${VLC_SRC}"

for patch_file in "${PATCH_FILES[@]}"; do
  if [[ ! -f "${patch_file}" ]]; then
    echo "Error: patch file not found: ${patch_file}" >&2
    exit 4
  fi

  if git apply --reverse --check "${patch_file}" >/dev/null 2>&1; then
    echo "Patch already applied: ${patch_file}"
    continue
  fi

  if git apply --check "${patch_file}" >/dev/null 2>&1; then
    git apply "${patch_file}"
    echo "Applied patch: ${patch_file}"
    continue
  fi

  if patch_already_present "${patch_file}"; then
    echo "Patch already integrated (heuristic): ${patch_file}"
    continue
  fi

  echo "Error: cannot apply patch and it is not detected as already integrated: ${patch_file}" >&2
  exit 5
done
