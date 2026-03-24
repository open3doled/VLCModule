#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: update_vlc3_patch.sh [pristine-vlc-source-or-tarball] [output-patch]

Defaults:
  pristine source/tarball: $OPEN3D_VLC_TARBALL or /tmp/vlc-3.0.23.tar.xz
  output patch:           patches/vlc-3.0.23/0001-open3doled-vlc3-mvc-runtime.patch

The input must be a pristine VLC 3.0.23 source tree or tarball.
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_INPUT="${OPEN3D_VLC_TARBALL:-/tmp/vlc-3.0.23.tar.xz}"
INPUT_PATH="${1:-${DEFAULT_INPUT}}"
OUTPUT_PATCH="${2:-${REPO_DIR}/patches/vlc-3.0.23/0001-open3doled-vlc3-mvc-runtime.patch}"
PATCH_DISC_UI_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_open3d_disc_ui.py"
PATCH_T64_HOST_PLUGINS_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_t64_host_plugins.py"
PATCH_MKV_EDGE264MVC_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_mkv_edge264mvc.py"
PATCH_INPUT_ROUTING_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_open3d_input_routing.py"

if [[ "${INPUT_PATH}" == "-h" || "${INPUT_PATH}" == "--help" ]]; then
  usage
  exit 0
fi

tmpdir="$(mktemp -d)"
cleanup() {
  rm -rf "${tmpdir}"
}
trap cleanup EXIT

extract_or_copy_tree() {
  local input_path="$1"
  local dest_root="$2"
  mkdir -p "${dest_root}"

  if [[ -d "${input_path}" ]]; then
    cp -a "${input_path}" "${dest_root}/src"
    printf '%s\n' "${dest_root}/src"
    return 0
  fi

  if [[ -f "${input_path}" ]]; then
    tar -xf "${input_path}" -C "${dest_root}"
    local extracted_root
    extracted_root="$(find "${dest_root}" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    if [[ -z "${extracted_root}" ]]; then
      echo "Error: failed to extract VLC source from ${input_path}" >&2
      exit 2
    fi
    printf '%s\n' "${extracted_root}"
    return 0
  fi

  echo "Error: input not found: ${input_path}" >&2
  exit 2
}

assert_pristine_vlc_tree() {
  local tree="$1"
  if [[ ! -d "${tree}/modules/video_output" ]]; then
    echo "Error: ${tree} does not look like a VLC source tree" >&2
    exit 3
  fi

  if rg -q "libopen3d_plugin_la_SOURCES|libedge264mvc_plugin_la_SOURCES|vout_display_opengl_DisplayNoSwap" \
      "${tree}/modules/video_output/Makefile.am" \
      "${tree}/modules/codec/Makefile.am" \
      "${tree}/modules/video_output/opengl/vout_helper.h" \
      "${tree}/modules/video_output/opengl/vout_helper.c"; then
    echo "Error: ${tree} does not look pristine; Open3D markers are already present" >&2
    exit 4
  fi
}

copy_repo_module_sources() {
  local tree="$1"
  local module_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/video_output/open3d_display.c"
  local font_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/video_output/open3d_font8x16_basic.h"
  local edge264_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/codec/edge264mvc.c"
  local open3dannexb_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/open3dannexb.c"
  local ts_psi_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/mpeg/ts_psi.c"
  local ts_c_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/mpeg/ts.c"
  local demux_makefile_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/Makefile.am"
  local playlist_src_dir="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/playlist"
  local access_src_dir="${REPO_DIR}/renderer/vlc-3.0.23/modules/access"

  cp "${module_src}" "${tree}/modules/video_output/open3d_display.c"
  cp "${font_src}" "${tree}/modules/video_output/open3d_font8x16_basic.h"
  cp "${edge264_src}" "${tree}/modules/codec/edge264mvc.c"
  cp "${open3dannexb_src}" "${tree}/modules/demux/open3dannexb.c"
  rm -f "${tree}/modules/demux/open3dmkv.c"
  cp "${ts_psi_src}" "${tree}/modules/demux/mpeg/ts_psi.c"
  cp "${ts_c_src}" "${tree}/modules/demux/mpeg/ts.c"
  cp "${demux_makefile_src}" "${tree}/modules/demux/Makefile.am"

  mkdir -p "${tree}/modules/demux/playlist" "${tree}/modules/access"
  find "${playlist_src_dir}" -maxdepth 1 -type f -exec cp {} "${tree}/modules/demux/playlist/" \;
  find "${access_src_dir}" -maxdepth 1 -type f -exec cp {} "${tree}/modules/access/" \;

  rm -f "${tree}/modules/demux/mvcasm.c"
  rm -f "${tree}/modules/demux/playlist/mpls.c"
}

patch_qt_disc_ui() {
  local tree="$1"
  if [[ ! -x "${PATCH_DISC_UI_SCRIPT}" ]]; then
    echo "Error: missing Qt Disc UI patch helper: ${PATCH_DISC_UI_SCRIPT}" >&2
    exit 3
  fi
  python3 "${PATCH_DISC_UI_SCRIPT}" "${tree}"
}

patch_t64_host_plugins() {
  local tree="$1"
  if [[ ! -x "${PATCH_T64_HOST_PLUGINS_SCRIPT}" ]]; then
    echo "Error: missing host t64 alias patch helper: ${PATCH_T64_HOST_PLUGINS_SCRIPT}" >&2
    exit 3
  fi
  python3 "${PATCH_T64_HOST_PLUGINS_SCRIPT}" "${tree}"
}

patch_mkv_edge264mvc() {
  local tree="$1"
  if [[ ! -x "${PATCH_MKV_EDGE264MVC_SCRIPT}" ]]; then
    echo "Error: missing MKV edge264mvc patch helper: ${PATCH_MKV_EDGE264MVC_SCRIPT}" >&2
    exit 3
  fi
  python3 "${PATCH_MKV_EDGE264MVC_SCRIPT}" "${tree}"
}

patch_input_routing() {
  local tree="$1"
  if [[ ! -x "${PATCH_INPUT_ROUTING_SCRIPT}" ]]; then
    echo "Error: missing input routing patch helper: ${PATCH_INPUT_ROUTING_SCRIPT}" >&2
    exit 3
  fi
  python3 "${PATCH_INPUT_ROUTING_SCRIPT}" "${tree}"
}

patch_vout_helper() {
  local tree="$1"
  python3 - <<'PY' "${tree}/modules/video_output/opengl/vout_helper.c" "${tree}/modules/video_output/opengl/vout_helper.h"
from pathlib import Path
import sys

c_path = Path(sys.argv[1])
h_path = Path(sys.argv[2])

c_text = c_path.read_text()
h_text = h_path.read_text()

old_sig = """int vout_display_opengl_Display(vout_display_opengl_t *vgl,
                                const video_format_t *source)
{"""
new_sig = """static int vout_display_opengl_DisplayCommon(vout_display_opengl_t *vgl,
                                             const video_format_t *source,
                                             bool do_swap)
{"""
if old_sig not in c_text:
    raise SystemExit("vout_helper.c: expected Display signature not found")
c_text = c_text.replace(old_sig, new_sig, 1)

old_swap = """    /* Display */
    vlc_gl_Swap(vgl->gl);
"""
new_swap = """    if (do_swap)
        vlc_gl_Swap(vgl->gl);
"""
if old_swap not in c_text:
    raise SystemExit("vout_helper.c: expected swap block not found")
c_text = c_text.replace(old_swap, new_swap, 1)

old_tail = """    GL_ASSERT_NOERROR();

    return VLC_SUCCESS;
}
"""
new_tail = """    GL_ASSERT_NOERROR();

    return VLC_SUCCESS;
}

int vout_display_opengl_DisplayNoSwap(vout_display_opengl_t *vgl,
                                      const video_format_t *source)
{
    return vout_display_opengl_DisplayCommon(vgl, source, false);
}

int vout_display_opengl_Display(vout_display_opengl_t *vgl,
                                const video_format_t *source)
{
    return vout_display_opengl_DisplayCommon(vgl, source, true);
}
"""
tail_pos = c_text.rfind(old_tail)
if tail_pos < 0:
    raise SystemExit("vout_helper.c: expected function tail not found")
c_text = c_text[:tail_pos] + new_tail + c_text[tail_pos + len(old_tail):]

h_needle = """int vout_display_opengl_Prepare(vout_display_opengl_t *vgl,
                                picture_t *picture, subpicture_t *subpicture);
int vout_display_opengl_Display(vout_display_opengl_t *vgl,
                                const video_format_t *source);
"""
h_replacement = """int vout_display_opengl_Prepare(vout_display_opengl_t *vgl,
                                picture_t *picture, subpicture_t *subpicture);
int vout_display_opengl_DisplayNoSwap(vout_display_opengl_t *vgl,
                                      const video_format_t *source);
int vout_display_opengl_Display(vout_display_opengl_t *vgl,
                                const video_format_t *source);
"""
if h_needle not in h_text:
    raise SystemExit("vout_helper.h: expected declaration block not found")
h_text = h_text.replace(h_needle, h_replacement, 1)

c_path.write_text(c_text)
h_path.write_text(h_text)
PY
}

patch_video_output_makefile() {
  local tree="$1"
  python3 - <<'PY' "${tree}/modules/video_output/Makefile.am"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

if "libopen3d_plugin_la_SOURCES" in text:
    raise SystemExit("video_output/Makefile.am already contains libopen3d plugin stanza")

block = """
libopen3d_plugin_la_SOURCES = $(OPENGL_COMMONSOURCES) video_output/open3d_display.c
libopen3d_plugin_la_CFLAGS = $(AM_CFLAGS) $(GL_CFLAGS) $(OPENGL_COMMONCLFAGS) $(OPEN3D_PLUGIN_CFLAGS)
libopen3d_plugin_la_LIBADD = $(LIBM) $(OPENGL_COMMONLIBS)
if HAVE_WIN32
libopen3d_plugin_la_CFLAGS += -DHAVE_GL_CORE_SYMBOLS
libopen3d_plugin_la_LIBADD += $(GL_LIBS)
else
libopen3d_plugin_la_LIBADD += -lpthread
endif
""".strip()

marker = """
libgl_plugin_la_SOURCES = $(OPENGL_COMMONSOURCES) video_output/opengl/display.c
libgl_plugin_la_CFLAGS = $(AM_CFLAGS) $(GL_CFLAGS) $(OPENGL_COMMONCLFAGS)
libgl_plugin_la_LIBADD = $(LIBM) $(OPENGL_COMMONLIBS)
if HAVE_WIN32
libgl_plugin_la_CFLAGS += -DHAVE_GL_CORE_SYMBOLS
libgl_plugin_la_LIBADD += $(GL_LIBS)
endif
""".strip()

if marker not in text:
    raise SystemExit("video_output/Makefile.am: libgl plugin stanza not found")
text = text.replace(marker, marker + "\n\n" + block, 1)

ltlib_marker = "vout_LTLIBRARIES += libgl_plugin.la\n"
if ltlib_marker not in text:
    raise SystemExit("video_output/Makefile.am: libgl LTLIBRARIES marker not found")
text = text.replace(ltlib_marker, ltlib_marker + "vout_LTLIBRARIES += libopen3d_plugin.la\n", 1)

path.write_text(text)
PY
}

patch_codec_makefile() {
  local tree="$1"
  python3 - <<'PY' "${tree}/modules/codec/Makefile.am"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

if "libedge264mvc_plugin_la_SOURCES" in text:
    raise SystemExit("codec/Makefile.am already contains edge264mvc stanza")

marker = """
libddummy_plugin_la_SOURCES = codec/ddummy.c
codec_LTLIBRARIES += libddummy_plugin.la
""".strip()

block = """
libedge264mvc_plugin_la_SOURCES = codec/edge264mvc.c
libedge264mvc_plugin_la_CFLAGS = $(AM_CFLAGS) $(EDGE264MVC_PLUGIN_CFLAGS)
libedge264mvc_plugin_la_LIBADD = $(LIBDL)
codec_LTLIBRARIES += libedge264mvc_plugin.la
""".strip()

if marker not in text:
    raise SystemExit("codec/Makefile.am: ddummy stanza not found")
text = text.replace(marker, marker + "\n\n" + block, 1)

path.write_text(text)
PY
}

patch_access_makefile() {
  local tree="$1"
  python3 - <<'PY' "${tree}/modules/access/Makefile.am"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

skip_prefixes = (
    "libopen3diso_plugin_la_SOURCES",
    "access_LTLIBRARIES += libopen3diso_plugin.la",
    "libopen3diso_plugin_la_CFLAGS",
    "libopen3dbluraybase_plugin_la_SOURCES",
    "access_LTLIBRARIES += libopen3dbluraybase_plugin.la",
    "libopen3dbluraybase_plugin_la_CFLAGS",
    "libopen3dbluraybase_plugin_la_LIBADD",
    "libopen3dbluraybase_plugin_la_LDFLAGS",
)

lines = text.splitlines()
out = []
for line in lines:
    if line.startswith(skip_prefixes):
        continue
    out.append(line)
text = "\n".join(out) + "\n"

block = """
libopen3dbluraymvc_plugin_la_SOURCES = access/open3dbluraymvc.c
access_LTLIBRARIES += libopen3dbluraymvc_plugin.la
libopen3dbluraymvc_plugin_la_CFLAGS = $(AM_CFLAGS) $(OPEN3DBLURAYMVC_PLUGIN_CFLAGS) $(BLURAY_CFLAGS)
libopen3dbluraymvc_plugin_la_LIBADD = $(OPEN3DBLURAYMVC_PLUGIN_LIBADD) $(BLURAY_LIBS)
libopen3dbluraymvc_plugin_la_LDFLAGS = $(AM_LDFLAGS) -rpath '$(accessdir)'
""".strip()

if "libopen3dbluraymvc_plugin_la_SOURCES" not in text:
    text += "\n" + block + "\n"

path.write_text(text)
PY
}

refresh_patch() {
  local tree="$1"
  local output_patch="$2"

  pushd "${tree}" >/dev/null
  git init -q
  git config user.name "Open3D Patch Refresh"
  git config user.email "open3d@example.invalid"
  git add -A
  git commit -q -m "upstream base"

  copy_repo_module_sources "${tree}"
  patch_qt_disc_ui "${tree}"
  patch_t64_host_plugins "${tree}"
  patch_mkv_edge264mvc "${tree}"
  patch_input_routing "${tree}"
  patch_vout_helper "${tree}"
  patch_video_output_makefile "${tree}"
  patch_codec_makefile "${tree}"
  patch_access_makefile "${tree}"

  mkdir -p "$(dirname "${output_patch}")"
  git diff --binary > "${output_patch}"
  popd >/dev/null
}

verify_patch_applies() {
  local input_path="$1"
  local patch_file="$2"
  local verify_root
  verify_root="$(extract_or_copy_tree "${input_path}" "${tmpdir}/verify")"
  assert_pristine_vlc_tree "${verify_root}"
  ( cd "${verify_root}" && git apply --check "${patch_file}" )
}

source_root="$(extract_or_copy_tree "${INPUT_PATH}" "${tmpdir}/source")"
assert_pristine_vlc_tree "${source_root}"
refresh_patch "${source_root}" "${OUTPUT_PATCH}"
verify_patch_applies "${INPUT_PATH}" "${OUTPUT_PATCH}"

echo "Updated patch: ${OUTPUT_PATCH}"
echo "Source input: ${INPUT_PATH}"
