#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_dir}/local/build/tools"
mkdir -p "${build_dir}"

src="${repo_dir}/tools/open3d_vk_display_flip_probe.c"
bin="${build_dir}/open3d_vk_display_flip_probe"

cc ${CFLAGS:-} -O2 -Wall -Wextra -std=c11 "${src}" -lvulkan -lm -o "${bin}"
exec "${bin}" "$@"
