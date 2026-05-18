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
PATCH_MKV_EDGE264MVC_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_mkv_edge264mvc.py"
PATCH_INPUT_ROUTING_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_open3d_input_routing.py"
PATCH_OLDRC_POPUP_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_oldrc_popup.py"

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
  local bridge_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/video_output/open3d_subtitle_bridge.h"
  local font_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/video_output/open3d_font8x16_basic.h"
  local edge264_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/codec/edge264mvc.c"
  local open3dannexb_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/open3dannexb.c"
  local ts_psi_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/mpeg/ts_psi.c"
  local ts_c_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/mpeg/ts.c"
  local demux_makefile_src="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/Makefile.am"
  local playlist_src_dir="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/playlist"
  local access_src_dir="${REPO_DIR}/renderer/vlc-3.0.23/modules/access"

  cp "${module_src}" "${tree}/modules/video_output/open3d_display.c"
  cp "${bridge_src}" "${tree}/modules/video_output/open3d_subtitle_bridge.h"
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

patch_oldrc_popup() {
  local tree="$1"
  if [[ ! -x "${PATCH_OLDRC_POPUP_SCRIPT}" ]]; then
    echo "Error: missing oldrc popup patch helper: ${PATCH_OLDRC_POPUP_SCRIPT}" >&2
    exit 3
  fi
  python3 "${PATCH_OLDRC_POPUP_SCRIPT}" "${tree}"
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

old_prepare = """int vout_display_opengl_Prepare(vout_display_opengl_t *vgl,
                                picture_t *picture, subpicture_t *subpicture)
{
    GL_ASSERT_NOERROR();

    opengl_tex_converter_t *tc = vgl->prgm->tc;

    /* Update the texture */
    int ret = tc->pf_update(tc, vgl->texture, vgl->tex_width, vgl->tex_height,
                            picture, NULL);
    if (ret != VLC_SUCCESS)
        return ret;

    int         last_count = vgl->region_count;
    gl_region_t *last = vgl->region;

    vgl->region_count = 0;
    vgl->region       = NULL;

    tc = vgl->sub_prgm->tc;
    if (subpicture) {

        int count = 0;
        for (subpicture_region_t *r = subpicture->p_region; r; r = r->p_next)
            count++;

        vgl->region_count = count;
        vgl->region       = calloc(count, sizeof(*vgl->region));

        int i = 0;
        for (subpicture_region_t *r = subpicture->p_region;
             r && ret == VLC_SUCCESS; r = r->p_next, i++) {
            gl_region_t *glr = &vgl->region[i];

            glr->width  = r->fmt.i_visible_width;
            glr->height = r->fmt.i_visible_height;
            if (!vgl->supports_npot) {
                glr->width  = GetAlignedSize(glr->width);
                glr->height = GetAlignedSize(glr->height);
                glr->tex_width  = (float) r->fmt.i_visible_width  / glr->width;
                glr->tex_height = (float) r->fmt.i_visible_height / glr->height;
            } else {
                glr->tex_width  = 1.0;
                glr->tex_height = 1.0;
            }
            glr->alpha  = (float)subpicture->i_alpha * r->i_alpha / 255 / 255;
            glr->left   =  2.0 * (r->i_x                          ) / subpicture->i_original_picture_width  - 1.0;
            glr->top    = -2.0 * (r->i_y                          ) / subpicture->i_original_picture_height + 1.0;
            glr->right  =  2.0 * (r->i_x + r->fmt.i_visible_width ) / subpicture->i_original_picture_width  - 1.0;
            glr->bottom = -2.0 * (r->i_y + r->fmt.i_visible_height) / subpicture->i_original_picture_height + 1.0;

            glr->texture = 0;
            /* Try to recycle the textures allocated by the previous
               call to this function. */
            for (int j = 0; j < last_count; j++) {
                if (last[j].texture &&
                    last[j].width  == glr->width &&
                    last[j].height == glr->height) {
                    glr->texture = last[j].texture;
                    memset(&last[j], 0, sizeof(last[j]));
                    break;
                }
            }

            const size_t pixels_offset =
                r->fmt.i_y_offset * r->p_picture->p->i_pitch +
                r->fmt.i_x_offset * r->p_picture->p->i_pixel_pitch;
            if (!glr->texture)
            {
                /* Could not recycle a previous texture, generate a new one. */
                ret = GenTextures(tc, &glr->width, &glr->height, &glr->texture);
                if (ret != VLC_SUCCESS)
                    continue;
            }
            /* Use the visible pitch of the region */
            r->p_picture->p[0].i_visible_pitch = r->fmt.i_visible_width
                                               * r->p_picture->p[0].i_pixel_pitch;
            ret = tc->pf_update(tc, &glr->texture, &glr->width, &glr->height,
                                r->p_picture, &pixels_offset);
        }
    }
    for (int i = 0; i < last_count; i++) {
        if (last[i].texture)
            DelTextures(tc, &last[i].texture);
    }
    free(last);

    VLC_UNUSED(subpicture);

    GL_ASSERT_NOERROR();
    return ret;
}
"""
new_prepare = """static int vout_display_opengl_PrepareSubpictureCommon(vout_display_opengl_t *vgl,
                                                 subpicture_t *subpicture)
{
    int ret = VLC_SUCCESS;
    int         last_count = vgl->region_count;
    gl_region_t *last = vgl->region;

    vgl->region_count = 0;
    vgl->region       = NULL;

    opengl_tex_converter_t *tc = vgl->sub_prgm->tc;
    if (subpicture) {

        int count = 0;
        for (subpicture_region_t *r = subpicture->p_region; r; r = r->p_next)
            count++;

        vgl->region_count = count;
        vgl->region       = calloc(count, sizeof(*vgl->region));

        int i = 0;
        for (subpicture_region_t *r = subpicture->p_region;
             r && ret == VLC_SUCCESS; r = r->p_next, i++) {
            gl_region_t *glr = &vgl->region[i];

            glr->width  = r->fmt.i_visible_width;
            glr->height = r->fmt.i_visible_height;
            if (!vgl->supports_npot) {
                glr->width  = GetAlignedSize(glr->width);
                glr->height = GetAlignedSize(glr->height);
                glr->tex_width  = (float) r->fmt.i_visible_width  / glr->width;
                glr->tex_height = (float) r->fmt.i_visible_height / glr->height;
            } else {
                glr->tex_width  = 1.0;
                glr->tex_height = 1.0;
            }
            glr->alpha  = (float)subpicture->i_alpha * r->i_alpha / 255 / 255;
            glr->left   =  2.0 * (r->i_x                          ) / subpicture->i_original_picture_width  - 1.0;
            glr->top    = -2.0 * (r->i_y                          ) / subpicture->i_original_picture_height + 1.0;
            glr->right  =  2.0 * (r->i_x + r->fmt.i_visible_width ) / subpicture->i_original_picture_width  - 1.0;
            glr->bottom = -2.0 * (r->i_y + r->fmt.i_visible_height) / subpicture->i_original_picture_height + 1.0;

            glr->texture = 0;
            /* Try to recycle the textures allocated by the previous
               call to this function. */
            for (int j = 0; j < last_count; j++) {
                if (last[j].texture &&
                    last[j].width  == glr->width &&
                    last[j].height == glr->height) {
                    glr->texture = last[j].texture;
                    memset(&last[j], 0, sizeof(last[j]));
                    break;
                }
            }

            const size_t pixels_offset =
                r->fmt.i_y_offset * r->p_picture->p->i_pitch +
                r->fmt.i_x_offset * r->p_picture->p->i_pixel_pitch;
            if (!glr->texture)
            {
                /* Could not recycle a previous texture, generate a new one. */
                ret = GenTextures(tc, &glr->width, &glr->height, &glr->texture);
                if (ret != VLC_SUCCESS)
                    continue;
            }
            /* Use the visible pitch of the region */
            r->p_picture->p[0].i_visible_pitch = r->fmt.i_visible_width
                                               * r->p_picture->p[0].i_pixel_pitch;
            ret = tc->pf_update(tc, &glr->texture, &glr->width, &glr->height,
                                r->p_picture, &pixels_offset);
        }
    }
    for (int i = 0; i < last_count; i++) {
        if (last[i].texture)
            DelTextures(tc, &last[i].texture);
    }
    free(last);

    return ret;
}

int vout_display_opengl_PrepareSubpicture(vout_display_opengl_t *vgl,
                                          subpicture_t *subpicture)
{
    GL_ASSERT_NOERROR();
    int ret = vout_display_opengl_PrepareSubpictureCommon(vgl, subpicture);
    GL_ASSERT_NOERROR();
    return ret;
}

int vout_display_opengl_Prepare(vout_display_opengl_t *vgl,
                                picture_t *picture, subpicture_t *subpicture)
{
    GL_ASSERT_NOERROR();

    opengl_tex_converter_t *tc = vgl->prgm->tc;

    /* Update the texture */
    int ret = tc->pf_update(tc, vgl->texture, vgl->tex_width, vgl->tex_height,
                            picture, NULL);
    if (ret != VLC_SUCCESS)
        return ret;

    ret = vout_display_opengl_PrepareSubpictureCommon(vgl, subpicture);
    GL_ASSERT_NOERROR();
    return ret;
}
"""
if old_prepare not in c_text:
    raise SystemExit("vout_helper.c: expected Prepare function not found")
c_text = c_text.replace(old_prepare, new_prepare, 1)

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
int vout_display_opengl_PrepareSubpicture(vout_display_opengl_t *vgl,
                                          subpicture_t *subpicture);
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
libopen3d_plugin_la_LIBADD += $(X_LIBS) $(X_PRE_LIBS) -lX11
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

patch_shared_gl_context() {
  local tree="$1"
  python3 - <<'PY' "${tree}/include/vlc_opengl.h" "${tree}/src/video_output/opengl.c" "${tree}/modules/video_output/opengl/egl.c" "${tree}/src/libvlccore.sym"
from pathlib import Path
import sys

h_path = Path(sys.argv[1])
c_path = Path(sys.argv[2])
egl_path = Path(sys.argv[3])
sym_path = Path(sys.argv[4])

h_text = h_path.read_text()
c_text = c_path.read_text()
egl_text = egl_path.read_text()
sym_text = sym_path.read_text()

h_old = """    module_t *module;
    void *sys;

    int  (*makeCurrent)(vlc_gl_t *);
"""
h_new = """    module_t *module;
    void *sys;
    unsigned api_type;
    vlc_gl_t *shared_context;

    int  (*makeCurrent)(vlc_gl_t *);
"""
if h_old not in h_text:
    raise SystemExit("vlc_opengl.h: expected struct block not found")
h_text = h_text.replace(h_old, h_new, 1)

h_decl_old = """VLC_API vlc_gl_t *vlc_gl_Create(struct vout_window_t *, unsigned, const char *) VLC_USED;
VLC_API void vlc_gl_Release(vlc_gl_t *);
"""
h_decl_new = """VLC_API vlc_gl_t *vlc_gl_Create(struct vout_window_t *, unsigned, const char *) VLC_USED;
VLC_API vlc_gl_t *vlc_gl_CreateShared(vlc_gl_t *) VLC_USED;
VLC_API void vlc_gl_Release(vlc_gl_t *);
"""
if h_decl_old not in h_text:
    raise SystemExit("vlc_opengl.h: expected declaration block not found")
h_text = h_text.replace(h_decl_old, h_decl_new, 1)

c_anchor = """struct vlc_gl_priv_t
{
    vlc_gl_t gl;
    atomic_uint ref_count;
};
"""
c_insert = """struct vlc_gl_priv_t
{
    vlc_gl_t gl;
    atomic_uint ref_count;
};

static const char *vlc_gl_SharedModuleName(const vlc_gl_t *gl)
{
    if (gl == NULL)
        return NULL;

    switch (gl->ext)
    {
        case VLC_GL_EXT_EGL:
            return "egl";
        default:
            return NULL;
    }
}
"""
if c_anchor not in c_text:
    raise SystemExit("opengl.c: expected struct anchor not found")
c_text = c_text.replace(c_anchor, c_insert, 1)

c_create_old = """    glpriv = vlc_custom_create(parent, sizeof (*glpriv), "gl");
    if (unlikely(glpriv == NULL))
        return NULL;

    glpriv->gl.surface = wnd;
    glpriv->gl.module = module_need(&glpriv->gl, type, name, true);
"""
c_create_new = """    glpriv = vlc_custom_create(parent, sizeof (*glpriv), "gl");
    if (unlikely(glpriv == NULL))
        return NULL;

    glpriv->gl.surface = wnd;
    glpriv->gl.api_type = flags;
    glpriv->gl.shared_context = NULL;
    glpriv->gl.module = module_need(&glpriv->gl, type, name, true);
"""
if c_create_old not in c_text:
    raise SystemExit("opengl.c: expected create block not found")
c_text = c_text.replace(c_create_old, c_create_new, 1)

c_after_create = """    atomic_init(&glpriv->ref_count, 1);

    return &glpriv->gl;
}

void vlc_gl_Hold(vlc_gl_t *gl)
"""
c_after_create_new = """    atomic_init(&glpriv->ref_count, 1);

    return &glpriv->gl;
}

vlc_gl_t *vlc_gl_CreateShared(vlc_gl_t *master)
{
    if (master == NULL)
        return NULL;

    const char *name = vlc_gl_SharedModuleName(master);
    if (name == NULL)
        return NULL;

    vlc_object_t *parent = (vlc_object_t *)master->surface;
    struct vlc_gl_priv_t *glpriv = vlc_custom_create(parent, sizeof (*glpriv), "gl");
    if (unlikely(glpriv == NULL))
        return NULL;

    const char *type;
    switch (master->api_type)
    {
        case VLC_OPENGL:
            type = "opengl";
            break;
        case VLC_OPENGL_ES2:
            type = "opengl es2";
            break;
        default:
            vlc_object_release(&glpriv->gl);
            return NULL;
    }

    glpriv->gl.surface = master->surface;
    glpriv->gl.api_type = master->api_type;
    glpriv->gl.shared_context = master;
    vlc_gl_Hold(master);
    glpriv->gl.module = module_need(&glpriv->gl, type, name, true);
    if (glpriv->gl.module == NULL)
    {
        vlc_gl_Release(master);
        vlc_object_release(&glpriv->gl);
        return NULL;
    }
    atomic_init(&glpriv->ref_count, 1);
    return &glpriv->gl;
}

void vlc_gl_Hold(vlc_gl_t *gl)
"""
if c_after_create not in c_text:
    raise SystemExit("opengl.c: expected post-create anchor not found")
c_text = c_text.replace(c_after_create, c_after_create_new, 1)

c_release_old = """void vlc_gl_Release(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = (struct vlc_gl_priv_t *)gl;
    if (atomic_fetch_sub(&glpriv->ref_count, 1) != 1)
        return;
    module_unneed(gl, gl->module);
    vlc_object_release(gl);
}
"""
c_release_new = """void vlc_gl_Release(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = (struct vlc_gl_priv_t *)gl;
    if (atomic_fetch_sub(&glpriv->ref_count, 1) != 1)
        return;
    vlc_gl_t *shared_context = gl->shared_context;
    module_unneed(gl, gl->module);
    vlc_object_release(gl);
    if (shared_context != NULL)
        vlc_gl_Release(shared_context);
}
"""
if c_release_old not in c_text:
    raise SystemExit("opengl.c: expected release block not found")
c_text = c_text.replace(c_release_old, c_release_new, 1)

egl_struct_old = """typedef struct vlc_gl_sys_t
{
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
"""
egl_struct_new = """typedef struct vlc_gl_sys_t
{
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    EGLConfig config;
    bool owns_display;
    bool owns_surface;
"""
if egl_struct_old not in egl_text:
    raise SystemExit("egl.c: expected sys struct block not found")
egl_text = egl_text.replace(egl_struct_old, egl_struct_new, 1)

egl_close_old = """static void Close (vlc_object_t *obj)
{
    vlc_gl_t *gl = (vlc_gl_t *)obj;
    vlc_gl_sys_t *sys = gl->sys;

    if (sys->display != EGL_NO_DISPLAY)
    {
        if (sys->context != EGL_NO_CONTEXT)
            eglDestroyContext(sys->display, sys->context);
        if (sys->surface != EGL_NO_SURFACE)
            eglDestroySurface(sys->display, sys->surface);
        eglTerminate(sys->display);
    }
#ifdef USE_PLATFORM_X11
    if (sys->x11 != NULL) {
        if (sys->restore_forget_gravity) {
            XSetWindowAttributes swa;
            swa.bit_gravity = ForgetGravity;
            XChangeWindowAttributes (sys->x11, gl->surface->handle.xid,
                                     CWBitGravity, &swa);
        }
        XCloseDisplay(sys->x11);
    }
#endif
"""
egl_close_new = """static void Close (vlc_object_t *obj)
{
    vlc_gl_t *gl = (vlc_gl_t *)obj;
    vlc_gl_sys_t *sys = gl->sys;

    if (sys->display != EGL_NO_DISPLAY)
    {
        if (sys->context != EGL_NO_CONTEXT)
            eglDestroyContext(sys->display, sys->context);
        if (sys->surface != EGL_NO_SURFACE && sys->owns_surface)
            eglDestroySurface(sys->display, sys->surface);
        if (sys->owns_display)
            eglTerminate(sys->display);
    }
#ifdef USE_PLATFORM_X11
    if (sys->owns_display && sys->x11 != NULL) {
        if (sys->restore_forget_gravity) {
            XSetWindowAttributes swa;
            swa.bit_gravity = ForgetGravity;
            XChangeWindowAttributes (sys->x11, gl->surface->handle.xid,
                                     CWBitGravity, &swa);
        }
        XCloseDisplay(sys->x11);
    }
#endif
"""
if egl_close_old not in egl_text:
    raise SystemExit("egl.c: expected close block not found")
egl_text = egl_text.replace(egl_close_old, egl_close_new, 1)

egl_open_old = """static int Open (vlc_object_t *obj, const struct gl_api *api)
{
    vlc_gl_t *gl = (vlc_gl_t *)obj;
    vlc_gl_sys_t *sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    gl->sys = sys;
    sys->display = EGL_NO_DISPLAY;
    sys->surface = EGL_NO_SURFACE;
    sys->context = EGL_NO_CONTEXT;
    sys->eglCreateImageKHR = NULL;
    sys->eglDestroyImageKHR = NULL;

    vout_window_t *wnd = gl->surface;
    EGLSurface (*createSurface)(EGLDisplay, EGLConfig, void *, const EGLint *)
        = CreateWindowSurface;
    void *window;
"""
egl_open_new = """static int Open (vlc_object_t *obj, const struct gl_api *api)
{
    vlc_gl_t *gl = (vlc_gl_t *)obj;
    vlc_gl_sys_t *sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    gl->sys = sys;
    sys->display = EGL_NO_DISPLAY;
    sys->surface = EGL_NO_SURFACE;
    sys->context = EGL_NO_CONTEXT;
    sys->config = NULL;
    sys->owns_display = true;
    sys->owns_surface = true;
    sys->eglCreateImageKHR = NULL;
    sys->eglDestroyImageKHR = NULL;

    vout_window_t *wnd = gl->surface;
    EGLSurface (*createSurface)(EGLDisplay, EGLConfig, void *, const EGLint *)
        = CreateWindowSurface;
    void *window = NULL;
    vlc_gl_sys_t *master_sys = NULL;

    if (gl->shared_context != NULL)
    {
        if (gl->shared_context->ext != VLC_GL_EXT_EGL ||
            gl->shared_context->sys == NULL)
            goto error;

        master_sys = gl->shared_context->sys;
        sys->display = master_sys->display;
        sys->config = master_sys->config;
        sys->owns_display = false;
        sys->owns_surface = true;
        goto shared_display_ready;
    }
"""
if egl_open_old not in egl_text:
    raise SystemExit("egl.c: expected open block not found")
egl_text = egl_text.replace(egl_open_old, egl_open_new, 1)

egl_display_old = """    if (sys->display == EGL_NO_DISPLAY)
        goto error;

    /* Initialize EGL display */
    EGLint major, minor;
    if (eglInitialize(sys->display, &major, &minor) != EGL_TRUE)
        goto error;
    msg_Dbg(obj, "EGL version %s by %s",
            eglQueryString(sys->display, EGL_VERSION),
            eglQueryString(sys->display, EGL_VENDOR));

    const char *ext = eglQueryString(sys->display, EGL_EXTENSIONS);
    if (*ext)
        msg_Dbg(obj, " extensions: %s", ext);

    if (major != 1 || minor < api->min_minor
     || !CheckAPI(sys->display, api->name))
    {
        msg_Err(obj, "cannot select %s API", api->name);
        goto error;
    }

    const EGLint conf_attr[] = {
        EGL_RED_SIZE, 5,
        EGL_GREEN_SIZE, 5,
        EGL_BLUE_SIZE, 5,
        EGL_RENDERABLE_TYPE, api->render_bit,
        EGL_NONE
    };
    EGLConfig cfgv[1];
    EGLint cfgc;

    if (eglChooseConfig(sys->display, conf_attr, cfgv, 1, &cfgc) != EGL_TRUE
     || cfgc == 0)
    {
        msg_Err (obj, "cannot choose EGL configuration");
        goto error;
    }

    /* Create a drawing surface */
    sys->surface = createSurface(sys->display, cfgv[0], window, NULL);
    if (sys->surface == EGL_NO_SURFACE)
    {
        msg_Err (obj, "cannot create EGL window surface");
        goto error;
    }

    if (eglBindAPI (api->api) != EGL_TRUE)
    {
        msg_Err (obj, "cannot bind EGL API");
        goto error;
    }

    EGLContext ctx = eglCreateContext(sys->display, cfgv[0], EGL_NO_CONTEXT,
                                      api->attr);
"""
egl_display_new = """shared_display_ready:
    if (sys->display == EGL_NO_DISPLAY)
        goto error;

    EGLint major = 1, minor = api->min_minor;
    if (sys->owns_display)
    {
        if (eglInitialize(sys->display, &major, &minor) != EGL_TRUE)
            goto error;
        msg_Dbg(obj, "EGL version %s by %s",
                eglQueryString(sys->display, EGL_VERSION),
                eglQueryString(sys->display, EGL_VENDOR));

        const char *ext = eglQueryString(sys->display, EGL_EXTENSIONS);
        if (*ext)
            msg_Dbg(obj, " extensions: %s", ext);

        if (major != 1 || minor < api->min_minor
         || !CheckAPI(sys->display, api->name))
        {
            msg_Err(obj, "cannot select %s API", api->name);
            goto error;
        }

        const EGLint conf_attr[] = {
            EGL_RED_SIZE, 5,
            EGL_GREEN_SIZE, 5,
            EGL_BLUE_SIZE, 5,
            EGL_RENDERABLE_TYPE, api->render_bit,
            EGL_NONE
        };
        EGLConfig cfgv[1];
        EGLint cfgc;

        if (eglChooseConfig(sys->display, conf_attr, cfgv, 1, &cfgc) != EGL_TRUE
         || cfgc == 0)
        {
            msg_Err (obj, "cannot choose EGL configuration");
            goto error;
        }
        sys->config = cfgv[0];

        /* Create a drawing surface */
        sys->surface = createSurface(sys->display, sys->config, window, NULL);
        if (sys->surface == EGL_NO_SURFACE)
        {
            msg_Err (obj, "cannot create EGL window surface");
            goto error;
        }
    }
    else
    {
        if (!CheckAPI(sys->display, api->name))
        {
            msg_Err(obj, "cannot reuse shared EGL API %s", api->name);
            goto error;
        }

        const EGLint pbuffer_attr[] = {
            EGL_WIDTH, 1,
            EGL_HEIGHT, 1,
            EGL_NONE
        };
        sys->surface = eglCreatePbufferSurface(sys->display, sys->config,
                                               pbuffer_attr);
        if (sys->surface == EGL_NO_SURFACE)
        {
            msg_Err(obj, "cannot create shared EGL pbuffer surface");
            goto error;
        }
    }

    if (eglBindAPI (api->api) != EGL_TRUE)
    {
        msg_Err (obj, "cannot bind EGL API");
        goto error;
    }

    EGLContext share_ctx =
        (master_sys != NULL) ? master_sys->context : EGL_NO_CONTEXT;
    EGLContext ctx = eglCreateContext(sys->display, sys->config, share_ctx,
                                      api->attr);
"""
if egl_display_old not in egl_text:
    raise SystemExit("egl.c: expected display/config block not found")
egl_text = egl_text.replace(egl_display_old, egl_display_new, 1)

h_path.write_text(h_text)
c_path.write_text(c_text)
egl_path.write_text(egl_text)

sym_old = """vlc_gl_Create
vlc_gl_Release
"""
sym_new = """vlc_gl_Create
vlc_gl_CreateShared
vlc_gl_Release
"""
if sym_old not in sym_text:
    raise SystemExit("libvlccore.sym: expected GL export block not found")
sym_text = sym_text.replace(sym_old, sym_new, 1)
sym_path.write_text(sym_text)
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
  patch_mkv_edge264mvc "${tree}"
  patch_input_routing "${tree}"
  patch_oldrc_popup "${tree}"
  patch_shared_gl_context "${tree}"
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
