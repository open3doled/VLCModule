#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 /path/to/vlc-3.0.23-source [configure args...]" >&2
  exit 1
fi

VLC_SRC="$(cd "$1" && pwd)"
shift || true
CONFIGURE_ARGS=("$@")
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODULE_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/video_output/open3d_display.c"
MODULE_FONT_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/video_output/open3d_font8x16_basic.h"
MODULE_SUBTITLE_BRIDGE_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/video_output/open3d_subtitle_bridge.h"
OPEN3D_WAYLAND_WINDOW_HEADER_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/video_output/open3d_wayland_window.h"
EDGE264MVC_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/codec/edge264mvc.c"
OPEN3DANNEXB_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/open3dannexb.c"
TS_PSI_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/mpeg/ts_psi.c"
TS_C_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/mpeg/ts.c"
DEMUX_MAKEFILE_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/Makefile.am"
PACKAGING_MODULES_MAKEFILE_IN="${REPO_DIR}/packaging/appimage/overlays/vlc-3.0.23/modules/Makefile.in"
PLAYLIST_SRC_DIR="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/playlist"
ACCESS_SRC_DIR="${REPO_DIR}/renderer/vlc-3.0.23/modules/access"
APPLY_PATCH_SCRIPT="${REPO_DIR}/scripts/apply_vlc3_patch.sh"
PATCH_DISC_UI_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_open3d_disc_ui.py"
PATCH_MKV_EDGE264MVC_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_mkv_edge264mvc.py"
PATCH_INPUT_ROUTING_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_open3d_input_routing.py"
PATCH_OLDRC_POPUP_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_oldrc_popup.py"
PATCH_QT_POPUP_MENU_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_qt_popup_menu.py"
PATCH_WAYLAND_PRESENTATION_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_open3d_wayland_presentation.py"

text_pattern_exists() {
  local pattern="$1"
  shift

  if command -v rg >/dev/null 2>&1; then
    rg -q "${pattern}" "$@"
  else
    grep -E -q "${pattern}" "$@"
  fi
}

qmake_svg_probe() {
  local probe_root="${REPO_DIR}/local/out/devpkgs/qt5svg_probe"
  mkdir -p "${probe_root}"
  cat > "${probe_root}/probe.pro" <<'EOF'
QT += core gui widgets svg
TARGET = probe
TEMPLATE = app
SOURCES += main.cpp
EOF
  cat > "${probe_root}/main.cpp" <<'EOF'
int main() { return 0; }
EOF
  qmake "${probe_root}/probe.pro" -o "${probe_root}/Makefile" >/dev/null 2>&1
}

prepare_qt_svg_overlay() {
  if pkg-config --exists "Qt5Core >= 5.5.0 Qt5Widgets Qt5Gui Qt5Svg" && qmake_svg_probe; then
    return 0
  fi

  local overlay_root="${REPO_DIR}/local/out/devpkgs/qt5svg_overlay"
  local overlay_pkg_dir="${overlay_root}/pkg"
  local overlay_extract_root="${overlay_root}/root"
  local overlay_usr="${overlay_extract_root}/usr"
  local overlay_pc="${overlay_usr}/lib/x86_64-linux-gnu/pkgconfig/Qt5Svg.pc"
  local overlay_pri="${overlay_usr}/lib/x86_64-linux-gnu/qt5/mkspecs/modules/qt_lib_svg.pri"
  local svg_runtime_lib=""
  local svg_dev_deb=""

  mkdir -p "${overlay_pkg_dir}" "${overlay_extract_root}"

  if ! compgen -G "${overlay_pkg_dir}/libqt5svg5-dev_*_amd64.deb" >/dev/null; then
    echo "Preparing local Qt5Svg development overlay..."
    (
      cd "${overlay_pkg_dir}"
      apt download libqt5svg5-dev
    )
  fi

  svg_dev_deb="$(find "${overlay_pkg_dir}" -maxdepth 1 -type f -name 'libqt5svg5-dev_*_amd64.deb' | sort | tail -n 1)"
  if [[ -z "${svg_dev_deb}" ]]; then
    echo "Error: could not stage libqt5svg5-dev into ${overlay_pkg_dir}" >&2
    return 1
  fi

  rm -rf "${overlay_extract_root}"
  mkdir -p "${overlay_extract_root}"
  dpkg-deb -x "${svg_dev_deb}" "${overlay_extract_root}"

  svg_runtime_lib="$(find /usr/lib/x86_64-linux-gnu -maxdepth 1 -type f -name 'libQt5Svg.so.5*' | sort | tail -n 1)"
  if [[ -z "${svg_runtime_lib}" ]]; then
    echo "Error: system Qt5Svg runtime library not found under /usr/lib/x86_64-linux-gnu" >&2
    return 1
  fi

  python3 - <<'PY' "${overlay_pc}" "${overlay_pri}" "${overlay_usr}"
from pathlib import Path
import sys

pc = Path(sys.argv[1])
pri = Path(sys.argv[2])
overlay_usr = Path(sys.argv[3])

pc_text = pc.read_text()
pc_text = pc_text.replace("prefix=/usr\n", f"prefix={overlay_usr}\n")
pc_text = pc_text.replace("libdir=${prefix}/lib/x86_64-linux-gnu\n", "libdir=/usr/lib/x86_64-linux-gnu\n")
pc.write_text(pc_text)

qt_inc = overlay_usr / "include/x86_64-linux-gnu/qt5"
pri_text = pri.read_text()
pri_text = pri_text.replace("QT.svg.libs = $$QT_MODULE_LIB_BASE\n", "QT.svg.libs = /usr/lib/x86_64-linux-gnu\n")
pri_text = pri_text.replace(
    "QT.svg.includes = $$QT_MODULE_INCLUDE_BASE $$QT_MODULE_INCLUDE_BASE/QtSvg\n",
    f"QT.svg.includes = {qt_inc} {qt_inc / 'QtSvg'}\n",
)
pri.write_text(pri_text)
PY

  ln -sf "${svg_runtime_lib}" "${overlay_usr}/lib/x86_64-linux-gnu/$(basename "${svg_runtime_lib}")"

  export PKG_CONFIG_PATH="${overlay_usr}/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  export QMAKEPATH="${overlay_usr}/lib/x86_64-linux-gnu/qt5${QMAKEPATH:+:${QMAKEPATH}}"
  export LIBRARY_PATH="${overlay_usr}/lib/x86_64-linux-gnu${LIBRARY_PATH:+:${LIBRARY_PATH}}"
}

prepare_qt_private_overlay() {
  local system_qpa=""
  system_qpa="$(find /usr/include -path '*qplatformnativeinterface.h' 2>/dev/null | sort | head -n 1 || true)"
  if [[ -n "${system_qpa}" ]]; then
    return 0
  fi

  local overlay_root="${REPO_DIR}/local/out/devpkgs/qtbase5_private_overlay"
  local overlay_pkg_dir="${overlay_root}/pkg"
  local overlay_extract_root="${overlay_root}/root"
  local overlay_usr="${overlay_extract_root}/usr"
  local private_dev_deb=""
  local qpa_header=""
  local qtgui_include=""
  local qtgui_version=""
  local qtgui_versioned=""

  mkdir -p "${overlay_pkg_dir}" "${overlay_extract_root}"

  if ! compgen -G "${overlay_pkg_dir}/qtbase5-private-dev_*_amd64.deb" >/dev/null; then
    echo "Preparing local Qt private development overlay..."
    (
      cd "${overlay_pkg_dir}"
      apt download qtbase5-private-dev
    )
  fi

  private_dev_deb="$(find "${overlay_pkg_dir}" -maxdepth 1 -type f -name 'qtbase5-private-dev_*_amd64.deb' | sort | tail -n 1)"
  if [[ -z "${private_dev_deb}" ]]; then
    echo "Error: could not stage qtbase5-private-dev into ${overlay_pkg_dir}" >&2
    return 1
  fi

  rm -rf "${overlay_extract_root}"
  mkdir -p "${overlay_extract_root}"
  dpkg-deb -x "${private_dev_deb}" "${overlay_extract_root}"

  qpa_header="$(find "${overlay_usr}/include" -path '*qplatformnativeinterface.h' | sort | head -n 1 || true)"
  if [[ -z "${qpa_header}" ]]; then
    echo "Error: qplatformnativeinterface.h missing from ${private_dev_deb}" >&2
    return 1
  fi

  qtgui_include="$(dirname "$(dirname "$(dirname "$(dirname "${qpa_header}")")")")"
  qtgui_version="$(dirname "$(dirname "$(dirname "${qpa_header}")")")"
  qtgui_versioned="$(dirname "$(dirname "${qpa_header}")")"

  if [[ -n "${QT_PLUGIN_CFLAGS:-}" ]]; then
    QT_PLUGIN_CFLAGS+=" "
  fi
  QT_PLUGIN_CFLAGS+="-I${qtgui_include} -I${qtgui_version} -I${qtgui_versioned}"
}

prepare_qt_x11extras_overlay() {
  if pkg-config --exists "Qt5Core >= 5.5.0 Qt5Widgets Qt5Gui Qt5X11Extras"; then
    return 0
  fi

  local overlay_root="${REPO_DIR}/local/out/devpkgs/qt5x11extras_overlay"
  local overlay_pkg_dir="${overlay_root}/pkg"
  local overlay_extract_root="${overlay_root}/root"
  local overlay_usr="${overlay_extract_root}/usr"
  local overlay_libdir="${overlay_usr}/lib/x86_64-linux-gnu"
  local overlay_pc="${overlay_libdir}/pkgconfig/Qt5X11Extras.pc"
  local overlay_pri="${overlay_libdir}/qt5/mkspecs/modules/qt_lib_x11extras.pri"
  local x11extras_dev_deb=""
  local x11extras_runtime_lib=""

  mkdir -p "${overlay_pkg_dir}" "${overlay_extract_root}"

  if ! compgen -G "${overlay_pkg_dir}/libqt5x11extras5-dev_*_amd64.deb" >/dev/null; then
    echo "Preparing local Qt5X11Extras development overlay..."
    (
      cd "${overlay_pkg_dir}"
      apt download libqt5x11extras5-dev
    )
  fi

  x11extras_dev_deb="$(find "${overlay_pkg_dir}" -maxdepth 1 -type f -name 'libqt5x11extras5-dev_*_amd64.deb' | sort | tail -n 1)"
  if [[ -z "${x11extras_dev_deb}" ]]; then
    echo "Error: could not stage libqt5x11extras5-dev into ${overlay_pkg_dir}" >&2
    return 1
  fi

  rm -rf "${overlay_extract_root}"
  mkdir -p "${overlay_extract_root}"
  dpkg-deb -x "${x11extras_dev_deb}" "${overlay_extract_root}"

  x11extras_runtime_lib="$(find /usr/lib/x86_64-linux-gnu -maxdepth 1 -type f -name 'libQt5X11Extras.so.5*' | sort | tail -n 1 || true)"
  if [[ -z "${x11extras_runtime_lib}" ]]; then
    echo "Error: system Qt5X11Extras runtime library not found under /usr/lib/x86_64-linux-gnu" >&2
    return 1
  fi

  cp -a "${x11extras_runtime_lib}" "${overlay_libdir}/"

  python3 - <<'PY' "${overlay_pc}" "${overlay_pri}" "${overlay_usr}"
from pathlib import Path
import sys

pc = Path(sys.argv[1])
pri = Path(sys.argv[2])
overlay_usr = Path(sys.argv[3])

pc_text = pc.read_text()
pc_text = pc_text.replace("prefix=/usr\n", f"prefix={overlay_usr}\n")
pc.write_text(pc_text)

qt_inc = overlay_usr / "include/x86_64-linux-gnu/qt5"
pri_text = pri.read_text()
pri_text = pri_text.replace("QT.x11extras.libs = $$QT_MODULE_LIB_BASE\n",
                            f"QT.x11extras.libs = {overlay_usr / 'lib/x86_64-linux-gnu'}\n")
pri_text = pri_text.replace(
    "QT.x11extras.includes = $$QT_MODULE_INCLUDE_BASE $$QT_MODULE_INCLUDE_BASE/QtX11Extras\n",
    f"QT.x11extras.includes = {qt_inc} {qt_inc / 'QtX11Extras'}\n",
)
pri.write_text(pri_text)
PY

  export PKG_CONFIG_PATH="${overlay_libdir}/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  export QMAKEPATH="${overlay_libdir}/qt5${QMAKEPATH:+:${QMAKEPATH}}"
  export LIBRARY_PATH="${overlay_libdir}${LIBRARY_PATH:+:${LIBRARY_PATH}}"
}

prepare_matroska_overlay() {
  if pkg-config --exists "libebml >= 1.3.6 libmatroska"; then
    return 0
  fi

  local overlay_root="${REPO_DIR}/local/out/devpkgs/matroska_overlay"
  local overlay_pkg_dir="${overlay_root}/pkg"
  local overlay_extract_root="${overlay_root}/root"
  local overlay_usr="${overlay_extract_root}/usr"
  local overlay_libdir="${overlay_usr}/lib/x86_64-linux-gnu"
  local ebml_pc="${overlay_usr}/lib/x86_64-linux-gnu/pkgconfig/libebml.pc"
  local matroska_pc="${overlay_usr}/lib/x86_64-linux-gnu/pkgconfig/libmatroska.pc"
  local ebml_dev_deb=""
  local matroska_dev_deb=""

  mkdir -p "${overlay_pkg_dir}" "${overlay_extract_root}"

  if ! compgen -G "${overlay_pkg_dir}/libebml-dev_*_amd64.deb" >/dev/null || \
     ! compgen -G "${overlay_pkg_dir}/libmatroska-dev_*_amd64.deb" >/dev/null; then
    echo "Preparing local Matroska development overlay..."
    (
      cd "${overlay_pkg_dir}"
      apt download libebml-dev libmatroska-dev
    )
  fi

  ebml_dev_deb="$(find "${overlay_pkg_dir}" -maxdepth 1 -type f -name 'libebml-dev_*_amd64.deb' | sort | tail -n 1)"
  matroska_dev_deb="$(find "${overlay_pkg_dir}" -maxdepth 1 -type f -name 'libmatroska-dev_*_amd64.deb' | sort | tail -n 1)"
  if [[ -z "${ebml_dev_deb}" || -z "${matroska_dev_deb}" ]]; then
    echo "Error: could not stage libebml-dev/libmatroska-dev into ${overlay_pkg_dir}" >&2
    return 1
  fi

  rm -rf "${overlay_extract_root}"
  mkdir -p "${overlay_extract_root}"
  dpkg-deb -x "${ebml_dev_deb}" "${overlay_extract_root}"
  dpkg-deb -x "${matroska_dev_deb}" "${overlay_extract_root}"

  for family in libebml.so.5* libmatroska.so.7*; do
    if compgen -G "/usr/lib/x86_64-linux-gnu/${family}" >/dev/null; then
      for runtime_lib in /usr/lib/x86_64-linux-gnu/${family}; do
        cp -a "${runtime_lib}" "${overlay_libdir}/"
      done
    else
      echo "Error: missing system runtime library family /usr/lib/x86_64-linux-gnu/${family}" >&2
      return 1
    fi
  done

  python3 - <<'PY' "${ebml_pc}" "${matroska_pc}" "${overlay_usr}"
from pathlib import Path
import sys

overlay_usr = Path(sys.argv[3])
for pc_arg in sys.argv[1:3]:
    pc = Path(pc_arg)
    text = pc.read_text()
    text = text.replace("prefix=/usr\n", f"prefix={overlay_usr}\n")
    pc.write_text(text)
PY

  export PKG_CONFIG_PATH="${overlay_usr}/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  export LIBRARY_PATH="${overlay_libdir}${LIBRARY_PATH:+:${LIBRARY_PATH}}"
}

prepare_libxml2_overlay() {
  if pkg-config --exists "libxml-2.0"; then
    return 0
  fi

  local overlay_root="${REPO_DIR}/local/out/devpkgs/libxml2_overlay"
  local overlay_pkg_dir="${overlay_root}/pkg"
  local overlay_extract_root="${overlay_root}/root"
  local overlay_usr="${overlay_extract_root}/usr"
  local overlay_libdir="${overlay_usr}/lib/x86_64-linux-gnu"
  local xml_pc="${overlay_usr}/lib/x86_64-linux-gnu/pkgconfig/libxml-2.0.pc"
  local xml_dev_deb=""
  local xml_runtime_so=""

  mkdir -p "${overlay_pkg_dir}" "${overlay_extract_root}"

  if ! compgen -G "${overlay_pkg_dir}/libxml2-dev_*_amd64.deb" >/dev/null; then
    echo "Preparing local libxml2 development overlay..."
    (
      cd "${overlay_pkg_dir}"
      apt download libxml2-dev
    )
  fi

  xml_dev_deb="$(find "${overlay_pkg_dir}" -maxdepth 1 -type f -name 'libxml2-dev_*_amd64.deb' | sort | tail -n 1)"
  if [[ -z "${xml_dev_deb}" ]]; then
    echo "Error: could not stage libxml2-dev into ${overlay_pkg_dir}" >&2
    return 1
  fi

  rm -rf "${overlay_extract_root}"
  mkdir -p "${overlay_extract_root}"
  dpkg-deb -x "${xml_dev_deb}" "${overlay_extract_root}"

  xml_runtime_so="$(find /usr/lib/x86_64-linux-gnu -maxdepth 1 -type f -name 'libxml2.so.*' | sort | tail -n 1 || true)"
  if [[ -n "${xml_runtime_so}" ]]; then
    cp -a "${xml_runtime_so}" "${overlay_libdir}/"
  fi

  python3 - <<'PY' "${xml_pc}" "${overlay_usr}"
from pathlib import Path
import sys

pc = Path(sys.argv[1])
overlay_usr = Path(sys.argv[2])
text = pc.read_text()
text = text.replace("prefix=/usr\n", f"prefix={overlay_usr}\n")
pc.write_text(text)
PY

  export PKG_CONFIG_PATH="${overlay_usr}/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  export LIBRARY_PATH="${overlay_libdir}${LIBRARY_PATH:+:${LIBRARY_PATH}}"
}

prepare_dvbpsi_overlay() {
  local overlay_root="${REPO_DIR}/local/out/devpkgs/dvbpsi_root"
  local overlay_pkg_dir="${overlay_root}/pkg"
  local overlay_usr="${overlay_root}/usr"
  local overlay_libdir="${overlay_usr}/lib/x86_64-linux-gnu"
  local dvbpsi_dev_deb=""
  local dvbpsi_runtime_so=""

  if [[ -f "${overlay_usr}/include/dvbpsi/dvbpsi.h" &&
        -e "${overlay_libdir}/libdvbpsi.so" ]]; then
    return 0
  fi

  mkdir -p "${overlay_pkg_dir}" "${overlay_root}"

  if ! compgen -G "${overlay_pkg_dir}/libdvbpsi-dev_*_amd64.deb" >/dev/null; then
    echo "Preparing local dvbpsi development overlay..."
    (
      cd "${overlay_pkg_dir}"
      apt download libdvbpsi-dev
    )
  fi

  dvbpsi_dev_deb="$(find "${overlay_pkg_dir}" -maxdepth 1 -type f -name 'libdvbpsi-dev_*_amd64.deb' | sort | tail -n 1)"
  if [[ -z "${dvbpsi_dev_deb}" ]]; then
    echo "Error: could not stage libdvbpsi-dev into ${overlay_pkg_dir}" >&2
    return 1
  fi

  rm -rf "${overlay_usr}"
  mkdir -p "${overlay_root}"
  dpkg-deb -x "${dvbpsi_dev_deb}" "${overlay_root}"

  if [[ ! -e "${overlay_libdir}/libdvbpsi.so" ]]; then
    dvbpsi_runtime_so="$(find /usr/lib/x86_64-linux-gnu -maxdepth 1 -type f -name 'libdvbpsi.so.*' | sort | tail -n 1 || true)"
    if [[ -n "${dvbpsi_runtime_so}" ]]; then
      mkdir -p "${overlay_libdir}"
      cp -a "${dvbpsi_runtime_so}" "${overlay_libdir}/"
      ln -sf "$(basename "${dvbpsi_runtime_so}")" "${overlay_libdir}/libdvbpsi.so"
    fi
  fi

  if [[ ! -f "${overlay_usr}/include/dvbpsi/dvbpsi.h" ||
        ! -e "${overlay_libdir}/libdvbpsi.so" ]]; then
    echo "Error: staged libdvbpsi-dev is incomplete under ${overlay_root}" >&2
    return 1
  fi
}

ensure_vout_helper_exports() {
  local tree="$1"

  if text_pattern_exists "vout_display_opengl_PrepareSubpicture|vout_display_opengl_DisplayNoSwap" \
      "${tree}/modules/video_output/opengl/vout_helper.h" \
      "${tree}/modules/video_output/opengl/vout_helper.c"; then
    return 0
  fi

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

ensure_shared_gl_context_support() {
  local tree="$1"

  if text_pattern_exists "vlc_gl_CreateShared" \
      "${tree}/include/vlc_opengl.h" \
      "${tree}/src/video_output/opengl.c" \
      "${tree}/src/libvlccore.sym" && \
     text_pattern_exists "eglCreatePbufferSurface" "${tree}/modules/video_output/opengl/egl.c" && \
     text_pattern_exists "glXCreatePbuffer" "${tree}/modules/video_output/glx.c"; then
    return 0
  fi

  python3 - <<'PY' "${tree}/include/vlc_opengl.h" "${tree}/src/video_output/opengl.c" "${tree}/modules/video_output/opengl/egl.c" "${tree}/modules/video_output/glx.c" "${tree}/src/libvlccore.sym"
from pathlib import Path
import sys

h_path = Path(sys.argv[1])
c_path = Path(sys.argv[2])
egl_path = Path(sys.argv[3])
glx_path = Path(sys.argv[4])
sym_path = Path(sys.argv[5])

h_text = h_path.read_text()
c_text = c_path.read_text()
egl_text = egl_path.read_text()
glx_text = glx_path.read_text()
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
if "shared_context" not in h_text:
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
if "vlc_gl_CreateShared" not in h_text:
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
if "vlc_gl_SharedModuleName" not in c_text:
    if c_anchor not in c_text:
        raise SystemExit("opengl.c: expected struct anchor not found")
    c_text = c_text.replace(c_anchor, c_insert, 1)
elif_glx_old = """        case VLC_GL_EXT_EGL:
            return "egl";
        default:
            return NULL;
"""
elif_glx_new = """        case VLC_GL_EXT_EGL:
            return "egl";
        case VLC_GL_EXT_DEFAULT:
            return "glx";
        default:
            return NULL;
"""
if "vlc_gl_SharedModuleName" in c_text and "return \"glx\";" not in c_text:
    if elif_glx_old not in c_text:
        raise SystemExit("opengl.c: expected shared module switch block not found")
    c_text = c_text.replace(elif_glx_old, elif_glx_new, 1)

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
if "glpriv->gl.api_type = flags;" not in c_text:
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
if "vlc_gl_t *vlc_gl_CreateShared" not in c_text:
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
if "vlc_gl_t *shared_context = gl->shared_context;" not in c_text:
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
if "owns_display" not in egl_text:
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
if "sys->surface != EGL_NO_SURFACE && sys->owns_surface" not in egl_text:
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
if "vlc_gl_sys_t *master_sys = NULL;" not in egl_text:
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
if "eglCreatePbufferSurface" not in egl_text:
    if egl_display_old not in egl_text:
        raise SystemExit("egl.c: expected display/config block not found")
    egl_text = egl_text.replace(egl_display_old, egl_display_new, 1)

glx_struct_old = """typedef struct vlc_gl_sys_t
{
    Display *display;
    GLXWindow win;
    GLXContext ctx;
    bool restore_forget_gravity;
} vlc_gl_sys_t;
"""
glx_struct_new = """typedef struct vlc_gl_sys_t
{
    Display *display;
    GLXDrawable drawable;
    GLXContext ctx;
    bool owns_pbuffer;
    bool restore_forget_gravity;
} vlc_gl_sys_t;
"""
if glx_struct_old in glx_text:
    glx_text = glx_text.replace(glx_struct_old, glx_struct_new, 1)

glx_text = glx_text.replace("sys->win", "sys->drawable")

glx_shared_old = """    /* Create a drawing surface */
    sys->drawable = glXCreateWindow (dpy, conf, gl->surface->handle.xid, NULL);
    if (sys->drawable == None)
    {
        msg_Err (obj, "cannot create GLX window");
        goto error;
    }

    /* Create an OpenGL context */
    sys->ctx = glXCreateNewContext (dpy, conf, GLX_RGBA_TYPE, NULL, True);
    if (sys->ctx == NULL)
    {
        glXDestroyWindow (dpy, sys->drawable);
        msg_Err (obj, "cannot create GLX context");
        goto error;
    }

    /* Set bit gravity if necessary */
    if (wa.bit_gravity == ForgetGravity) {
        XSetWindowAttributes swa;
        swa.bit_gravity = NorthWestGravity;
        XChangeWindowAttributes (dpy, gl->surface->handle.xid, CWBitGravity,
                                 &swa);
        sys->restore_forget_gravity = true;
    } else
        sys->restore_forget_gravity = false;
"""
glx_shared_new = """    const bool shared = gl->shared_context != NULL;
    GLXContext share_ctx = NULL;
    if (shared)
    {
        if (gl->shared_context->sys == NULL)
            goto error;
        vlc_gl_sys_t *master_sys = gl->shared_context->sys;
        share_ctx = master_sys->ctx;
    }

    /* Create a drawing surface. Shared upload contexts use a tiny pbuffer so
     * they never bind the visible window drawable from the prepare thread. */
    if (shared)
    {
        int drawable_type = 0;
        glXGetFBConfigAttrib(dpy, conf, GLX_DRAWABLE_TYPE, &drawable_type);
        if ((drawable_type & GLX_PBUFFER_BIT) == 0)
        {
            msg_Err(obj, "GLX config does not support pbuffers for shared context");
            goto error;
        }
        static const int pbuffer_attr[] = {
            GLX_PBUFFER_WIDTH, 1,
            GLX_PBUFFER_HEIGHT, 1,
            None
        };
        sys->drawable = glXCreatePbuffer(dpy, conf, pbuffer_attr);
        sys->owns_pbuffer = true;
        sys->restore_forget_gravity = false;
    }
    else
    {
        sys->drawable = glXCreateWindow (dpy, conf, gl->surface->handle.xid, NULL);
        sys->owns_pbuffer = false;
    }
    if (sys->drawable == None)
    {
        msg_Err (obj, shared ? "cannot create shared GLX pbuffer"
                             : "cannot create GLX window");
        goto error;
    }

    /* Create an OpenGL context */
    sys->ctx = glXCreateNewContext (dpy, conf, GLX_RGBA_TYPE, share_ctx, True);
    if (sys->ctx == NULL)
    {
        if (sys->owns_pbuffer)
            glXDestroyPbuffer(dpy, sys->drawable);
        else
            glXDestroyWindow (dpy, sys->drawable);
        msg_Err (obj, "cannot create GLX context");
        goto error;
    }

    /* Set bit gravity if necessary */
    if (!shared && wa.bit_gravity == ForgetGravity) {
        XSetWindowAttributes swa;
        swa.bit_gravity = NorthWestGravity;
        XChangeWindowAttributes (dpy, gl->surface->handle.xid, CWBitGravity,
                                 &swa);
        sys->restore_forget_gravity = true;
    } else
        sys->restore_forget_gravity = false;
"""
if glx_shared_old in glx_text:
    glx_text = glx_text.replace(glx_shared_old, glx_shared_new, 1)
elif "glXCreatePbuffer" not in glx_text:
    raise SystemExit("glx.c: expected surface/context creation block not found")

glx_close_old = """    glXDestroyContext (dpy, sys->ctx);
    glXDestroyWindow (dpy, sys->drawable);
"""
glx_close_new = """    glXDestroyContext (dpy, sys->ctx);
    if (sys->owns_pbuffer)
        glXDestroyPbuffer(dpy, sys->drawable);
    else
        glXDestroyWindow (dpy, sys->drawable);
"""
if glx_close_old in glx_text:
    glx_text = glx_text.replace(glx_close_old, glx_close_new, 1)
elif "glXDestroyPbuffer" not in glx_text:
    raise SystemExit("glx.c: expected close drawable destroy block not found")

sym_old = """vlc_gl_Create
vlc_gl_Release
"""
sym_new = """vlc_gl_Create
vlc_gl_CreateShared
vlc_gl_Release
"""
if "vlc_gl_CreateShared" not in sym_text:
    if sym_old not in sym_text:
        raise SystemExit("libvlccore.sym: expected GL export block not found")
    sym_text = sym_text.replace(sym_old, sym_new, 1)

h_path.write_text(h_text)
c_path.write_text(c_text)
egl_path.write_text(egl_text)
glx_path.write_text(glx_text)
sym_path.write_text(sym_text)
PY
}

ensure_qt_interface_backend_fallback() {
  local tree="$1"
  local qt_cpp="${tree}/modules/gui/qt/qt.cpp"

  [[ -f "${qt_cpp}" ]] || return 0
  if text_pattern_exists "Qt backend helper probe failed, letting Qt auto-select platform" "${qt_cpp}"; then
    return 0
  fi

  python3 - <<'PY' "${qt_cpp}"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

old = """#if defined (QT5_HAS_X11) || defined (QT5_HAS_WAYLAND)
        return VLC_EGENERIC;
#endif
"""

new = """#if defined (QT5_HAS_X11) || defined (QT5_HAS_WAYLAND)
    {
        const char *display_env = getenv( "DISPLAY" );
        const char *wayland_display_env = getenv( "WAYLAND_DISPLAY" );
        if( (display_env != NULL && *display_env != '\\0') ||
            (wayland_display_env != NULL && *wayland_display_env != '\\0') )
            msg_Warn( p_this, "Qt backend helper probe failed, letting Qt auto-select platform (DISPLAY=%s WAYLAND_DISPLAY=%s)",
                      display_env != NULL ? display_env : "",
                      wayland_display_env != NULL ? wayland_display_env : "" );
        else
            return VLC_EGENERIC;
    }
#endif
"""

if old not in text:
    raise SystemExit("qt.cpp: expected backend reject block not found")

path.write_text(text.replace(old, new, 1))
PY
}

ensure_qt_wayland_preferred_backend() {
  local tree="$1"
  local qt_cpp="${tree}/modules/gui/qt/qt.cpp"

  [[ -f "${qt_cpp}" ]] || return 0
  if text_pattern_exists "PreferWaylandQtBackend" "${qt_cpp}"; then
    return 0
  fi

  python3 - <<'PY' "${qt_cpp}"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

helper_anchor = """static bool HasWayland( void )
{
    struct wl_display *dpy = wl_display_connect( NULL );
    if( dpy == NULL )
        return false;

    wl_display_disconnect( dpy );
    return true;
}
#endif
"""

helper_replacement = """static bool HasWayland( void )
{
    struct wl_display *dpy = wl_display_connect( NULL );
    if( dpy == NULL )
        return false;

    wl_display_disconnect( dpy );
    return true;
}

static bool PreferWaylandQtBackend( void )
{
    const QByteArray platform = qgetenv( "QT_QPA_PLATFORM" );
    if( platform.startsWith( "wayland" ) )
        return true;
    if( platform == "xcb" )
        return false;

    const QByteArray enabled = qgetenv( "OPEN3D_APPIMAGE_ENABLE_WAYLAND" ).toLower();
    if( enabled == "0" || enabled == "false" || enabled == "no" || enabled == "off" )
        return false;

    return qgetenv( "XDG_SESSION_TYPE" ).toLower() == "wayland" &&
           !qgetenv( "WAYLAND_DISPLAY" ).isEmpty();
}
#endif
"""

selection_old = """#ifdef QT5_HAS_X11
    if( HasX11( p_this ) )
        thread = ThreadXCB;
    else
#endif
#ifdef QT5_HAS_WAYLAND
    if( HasWayland() )
        thread = ThreadWayland;
    else
#endif
"""

selection_new = """#ifdef QT5_HAS_WAYLAND
    if( PreferWaylandQtBackend() && HasWayland() )
        thread = ThreadWayland;
    else
#endif
#ifdef QT5_HAS_X11
    if( HasX11( p_this ) )
        thread = ThreadXCB;
    else
#endif
#ifdef QT5_HAS_WAYLAND
    if( HasWayland() )
        thread = ThreadWayland;
    else
#endif
"""

if helper_anchor not in text:
    raise SystemExit("qt.cpp: expected HasWayland block not found")
if selection_old not in text:
    raise SystemExit("qt.cpp: expected backend selection block not found")

text = text.replace(helper_anchor, helper_replacement, 1)
text = text.replace(selection_old, selection_new, 1)
path.write_text(text)
PY
}

refresh_release_autotools_outputs() {
  local generated_root=""
  local makefile_am=""
  local makefile_in=""
  for generated_root in aclocal.m4 configure config.h.in; do
    if [[ -f "${generated_root}" ]]; then
      touch "${generated_root}"
    fi
  done
  while IFS= read -r -d '' makefile_am; do
    makefile_in="${makefile_am%.am}.in"
    if [[ -f "${makefile_in}" ]]; then
      touch "${makefile_in}"
    fi
  done < <(find . -name Makefile.am -print0)
}

ensure_open3d_wayland_linkage() {
  local makefile="$1"
  [[ -f "${makefile}" ]] || return 0
  python3 - <<'PY' "${makefile}"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

old = """libopen3d_plugin_la_CFLAGS = $(AM_CFLAGS) $(GL_CFLAGS) \\
\t$(OPENGL_COMMONCLFAGS) $(OPEN3D_PLUGIN_CFLAGS) \\
\t$(am__append_231)
"""
new = """libopen3d_plugin_la_CFLAGS = $(AM_CFLAGS) $(GL_CFLAGS) \\
\t$(OPENGL_COMMONCLFAGS) $(OPEN3D_PLUGIN_CFLAGS) \\
\t$(am__append_231) $(WAYLAND_CLIENT_CFLAGS)
"""
if old in text:
    text = text.replace(old, new, 1)

old = """libopen3d_plugin_la_LIBADD = $(LIBM) $(OPENGL_COMMONLIBS) \\
\t$(am__append_232) $(am__append_122) $(X_LIBS) $(X_PRE_LIBS) -lX11
"""
new = """libopen3d_plugin_la_LIBADD = $(LIBM) $(OPENGL_COMMONLIBS) \\
\t$(am__append_232) $(am__append_122) $(X_LIBS) $(X_PRE_LIBS) -lX11 \\
\t$(WAYLAND_CLIENT_LIBS)
"""
if old in text:
    text = text.replace(old, new, 1)

path.write_text(text)
PY
}

ensure_open3d_wayland_presentation_protocol() {
  local wayland_dir="${VLC_SRC}/modules/video_output/wayland"
  local protocols="${WAYLAND_PROTOCOLS:-}"
  local scanner="${WAYLAND_SCANNER:-}"
  local xml=""

  [[ -d "${wayland_dir}" ]] || return 0
  if [[ -z "${protocols}" ]] && command -v pkg-config >/dev/null 2>&1; then
    protocols="$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null || true)"
  fi
  if [[ -z "${scanner}" ]]; then
    scanner="$(command -v wayland-scanner || true)"
  fi
  [[ -n "${protocols}" && -x "${scanner}" ]] || return 0

  generate_wayland_protocol() {
    local protocol_xml="$1"
    local header_out="$2"
    local code_out="$3"

    [[ -f "${protocol_xml}" ]] || return 0
    "${scanner}" client-header "${protocol_xml}" "${header_out}"
    "${scanner}" private-code "${protocol_xml}" "${code_out}"
  }

  generate_wayland_protocol \
    "${protocols}/unstable/xdg-shell/xdg-shell-unstable-v5.xml" \
    "${wayland_dir}/xdg-shell-client-protocol.h" \
    "${wayland_dir}/xdg-shell-protocol.c"
  generate_wayland_protocol \
    "${wayland_dir}/server-decoration.xml" \
    "${wayland_dir}/server-decoration-client-protocol.h" \
    "${wayland_dir}/server-decoration-protocol.c"
  generate_wayland_protocol \
    "${protocols}/stable/viewporter/viewporter.xml" \
    "${wayland_dir}/viewporter-client-protocol.h" \
    "${wayland_dir}/viewporter-protocol.c"

  xml="${protocols}/stable/presentation-time/presentation-time.xml"
  [[ -f "${xml}" ]] || return 0

  "${scanner}" client-header "${xml}" \
    "${wayland_dir}/presentation-time-client-protocol.h"
  "${scanner}" private-code "${xml}" \
    "${wayland_dir}/presentation-time-protocol.c"
}

qt_plugin_enabled_in_makefile() {
  grep -q '^am__append_145 = libqt_plugin.la' modules/Makefile 2>/dev/null
}

mkv_plugin_deps_configured() {
  grep -Eq '^MATROSKA_LIBS = .*(-lmatroska|libmatroska)' modules/Makefile 2>/dev/null
}

wayland_config_matches_requested() {
  local requested=""
  local arg

  for arg in "${CONFIGURE_ARGS[@]}"; do
    case "${arg}" in
      --enable-wayland)
        requested=1
        ;;
      --disable-wayland)
        requested=0
        ;;
    esac
  done

  if [[ -z "${requested}" ]]; then
    return 0
  fi

  if [[ "${requested}" == "1" ]]; then
    grep -Eq '^WAYLAND_PROTOCOLS = .+' modules/Makefile 2>/dev/null &&
      grep -q '^am__append_246 = libegl_wl_plugin.la' modules/Makefile 2>/dev/null
  else
    ! grep -q '^am__append_246 = libegl_wl_plugin.la' modules/Makefile 2>/dev/null
  fi
}

build_qt_generated_sources() {
  local makefile="${VLC_SRC}/modules/Makefile"
  local targets_file
  targets_file="$(mktemp)"
  python3 - <<'PY' "${makefile}" "${targets_file}"
from pathlib import Path
import sys

makefile = Path(sys.argv[1]).read_text()
out = Path(sys.argv[2])
needle = "nodist_libqt_plugin_la_SOURCES = "
start = makefile.find(needle)
if start < 0:
    raise SystemExit("Qt nodist source list not found")
start += len(needle)
lines = []
for line in makefile[start:].splitlines():
    if not line.strip():
        break
    lines.append(line.rstrip())
joined = " ".join(line.rstrip("\\").strip() for line in lines)
items = [item for item in joined.split() if item.startswith("gui/qt/")]
out.write_text("\n".join(items) + "\n")
PY
  if [[ -s "${targets_file}" ]]; then
    xargs -a "${targets_file}" make -C "${VLC_SRC}/modules"
  fi
  rm -f "${targets_file}"
}

build_qt_prerequisites() {
  if [[ ! -f "${VLC_SRC}/include/vlc_about.h" ]]; then
    make -C "${VLC_SRC}/src" ../include/vlc_about.h
  fi

  build_qt_generated_sources
}

if [[ ! -d "${VLC_SRC}/modules/video_output" ]]; then
  echo "Error: ${VLC_SRC} does not look like a VLC source tree" >&2
  exit 2
fi

cd "${VLC_SRC}"

prepare_dvbpsi_overlay

DVBPSI_BUILD_CFLAGS="${DVBPSI_BUILD_CFLAGS:-}"
DVBPSI_BUILD_LIBS="${DVBPSI_BUILD_LIBS:-}"
LOCAL_DVBPSI_DEVROOT="${REPO_DIR}/local/out/devpkgs/dvbpsi_root"
if [[ -f "${LOCAL_DVBPSI_DEVROOT}/usr/include/dvbpsi/dvbpsi.h" ]]; then
  DVBPSI_BUILD_CFLAGS="-I${LOCAL_DVBPSI_DEVROOT}/usr/include"
  DVBPSI_BUILD_LIBS="-L${LOCAL_DVBPSI_DEVROOT}/usr/lib/x86_64-linux-gnu -ldvbpsi"
fi
export DVBPSI_CFLAGS="${DVBPSI_BUILD_CFLAGS}"
export DVBPSI_LIBS="${DVBPSI_BUILD_LIBS}"
if [[ -z "${DVBPSI_BUILD_CFLAGS}" ]]; then
  unset DVBPSI_CFLAGS || true
fi
if [[ -z "${DVBPSI_BUILD_LIBS}" ]]; then
  unset DVBPSI_LIBS || true
fi

if ! grep -q "libopen3d_plugin_la_SOURCES" modules/video_output/Makefile.am || \
   ! grep -q "vout_display_opengl_DisplayNoSwap" modules/video_output/opengl/vout_helper.h || \
   ! grep -q "libedge264mvc_plugin_la_SOURCES" modules/codec/Makefile.am; then
  if [[ ! -x "${APPLY_PATCH_SCRIPT}" ]]; then
    echo "Error: missing VLC patch apply helper: ${APPLY_PATCH_SCRIPT}" >&2
    exit 3
  fi
  echo "Applying canonical VLC module/runtime patch set..."
  "${APPLY_PATCH_SCRIPT}" "${VLC_SRC}"
fi

if [[ -x "${PATCH_DISC_UI_SCRIPT}" ]]; then
  python3 "${PATCH_DISC_UI_SCRIPT}" "${VLC_SRC}"
fi
if [[ -x "${PATCH_MKV_EDGE264MVC_SCRIPT}" ]]; then
  python3 "${PATCH_MKV_EDGE264MVC_SCRIPT}" "${VLC_SRC}"
fi
if [[ -x "${PATCH_INPUT_ROUTING_SCRIPT}" ]]; then
  python3 "${PATCH_INPUT_ROUTING_SCRIPT}" "${VLC_SRC}"
fi
if [[ -x "${PATCH_OLDRC_POPUP_SCRIPT}" ]]; then
  python3 "${PATCH_OLDRC_POPUP_SCRIPT}" "${VLC_SRC}"
fi
if [[ -x "${PATCH_QT_POPUP_MENU_SCRIPT}" ]]; then
  python3 "${PATCH_QT_POPUP_MENU_SCRIPT}" "${VLC_SRC}"
fi
if [[ -f "${PATCH_WAYLAND_PRESENTATION_SCRIPT}" ]]; then
  python3 "${PATCH_WAYLAND_PRESENTATION_SCRIPT}" "${VLC_SRC}"
fi

ensure_vout_helper_exports "${VLC_SRC}"
ensure_shared_gl_context_support "${VLC_SRC}"
ensure_qt_interface_backend_fallback "${VLC_SRC}"
ensure_qt_wayland_preferred_backend "${VLC_SRC}"
ensure_open3d_wayland_presentation_protocol

if [[ -f "${MODULE_SRC}" ]]; then
  cp "${MODULE_SRC}" "${VLC_SRC}/modules/video_output/open3d_display.c"
fi
if [[ -f "${MODULE_FONT_SRC}" ]]; then
  cp "${MODULE_FONT_SRC}" "${VLC_SRC}/modules/video_output/open3d_font8x16_basic.h"
fi
if [[ -f "${MODULE_SUBTITLE_BRIDGE_SRC}" ]]; then
  cp "${MODULE_SUBTITLE_BRIDGE_SRC}" "${VLC_SRC}/modules/video_output/open3d_subtitle_bridge.h"
fi
if [[ -f "${OPEN3D_WAYLAND_WINDOW_HEADER_SRC}" ]]; then
  cp "${OPEN3D_WAYLAND_WINDOW_HEADER_SRC}" "${VLC_SRC}/modules/video_output/open3d_wayland_window.h"
fi
if [[ -f "${EDGE264MVC_SRC}" ]]; then
  cp "${EDGE264MVC_SRC}" "${VLC_SRC}/modules/codec/edge264mvc.c"
fi
if [[ -f "${OPEN3DANNEXB_SRC}" ]]; then
  cp "${OPEN3DANNEXB_SRC}" "${VLC_SRC}/modules/demux/open3dannexb.c"
fi
rm -f "${VLC_SRC}/modules/demux/open3dmkv.c"
rm -f "${VLC_SRC}/modules/demux/mvcasm.c"
if [[ -f "${TS_PSI_SRC}" ]]; then
  mkdir -p "${VLC_SRC}/modules/demux/mpeg"
  cp "${TS_PSI_SRC}" "${VLC_SRC}/modules/demux/mpeg/ts_psi.c"
fi
if [[ -f "${TS_C_SRC}" ]]; then
  mkdir -p "${VLC_SRC}/modules/demux/mpeg"
  cp "${TS_C_SRC}" "${VLC_SRC}/modules/demux/mpeg/ts.c"
fi
if [[ -f "${DEMUX_MAKEFILE_SRC}" ]]; then
  cp "${DEMUX_MAKEFILE_SRC}" "${VLC_SRC}/modules/demux/Makefile.am"
fi
if [[ -f "${PACKAGING_MODULES_MAKEFILE_IN}" ]]; then
  cp "${PACKAGING_MODULES_MAKEFILE_IN}" "${VLC_SRC}/modules/Makefile.in"
fi
if [[ -d "${PLAYLIST_SRC_DIR}" ]]; then
  mkdir -p "${VLC_SRC}/modules/demux/playlist"
  find "${PLAYLIST_SRC_DIR}" -maxdepth 1 -type f | while read -r src; do
    cp "${src}" "${VLC_SRC}/modules/demux/playlist/"
  done
fi
rm -f "${VLC_SRC}/modules/demux/playlist/mpls.c"
if [[ -d "${ACCESS_SRC_DIR}" ]]; then
  mkdir -p "${VLC_SRC}/modules/access"
  find "${ACCESS_SRC_DIR}" -maxdepth 1 -type f | while read -r src; do
    cp "${src}" "${VLC_SRC}/modules/access/"
  done
fi

ACCESS_MAKEFILE="${VLC_SRC}/modules/access/Makefile.am"
if [[ -f "${ACCESS_MAKEFILE}" ]] && ! grep -q '^access_LTLIBRARIES[[:space:]]*=' "${ACCESS_MAKEFILE}"; then
  VLC_TARBALL="${OPEN3D_VLC_TARBALL:-/tmp/vlc-3.0.23.tar.xz}"
  if [[ -f "${VLC_TARBALL}" ]]; then
    python3 - <<'PY' "${VLC_TARBALL}" "${ACCESS_MAKEFILE}"
import sys, tarfile
from pathlib import Path
tar_path = Path(sys.argv[1])
dst = Path(sys.argv[2])
with tarfile.open(tar_path, 'r:*') as tf:
    member = next((m for m in tf.getmembers() if m.name.endswith('/modules/access/Makefile.am')), None)
    if member is None:
        raise SystemExit('modules/access/Makefile.am not found in tarball')
    data = tf.extractfile(member).read()
dst.write_bytes(data)
print(f"Restored {dst} from {tar_path}")
PY
  else
    echo "Error: ${ACCESS_MAKEFILE} is missing the base access_LTLIBRARIES definition and ${VLC_TARBALL} is unavailable" >&2
    exit 7
  fi
fi

python3 - <<'PY' "${VLC_SRC}/modules/access/Makefile.am"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
skip_prefixes = (
    "libopen3diso_plugin_la_SOURCES",
    "access_LTLIBRARIES += libopen3diso_plugin.la",
    "libopen3diso_plugin_la_CFLAGS",
)

if "libopen3diso_plugin_la_SOURCES" in text:
    lines = text.splitlines()
    out = []
    for line in lines:
        if line.startswith(skip_prefixes):
            continue
        out.append(line)
    text = "\n".join(out) + "\n"
    path.write_text(text)
PY

python3 - <<'PY' "${VLC_SRC}/modules/access/Makefile.am"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
skip_prefixes = (
    "libopen3dbluraybase_plugin_la_SOURCES",
    "access_LTLIBRARIES += libopen3dbluraybase_plugin.la",
    "libopen3dbluraybase_plugin_la_CFLAGS",
    "libopen3dbluraybase_plugin_la_LIBADD",
    "libopen3dbluraybase_plugin_la_LDFLAGS",
)

if "libopen3dbluraybase_plugin_la_SOURCES" in text:
    lines = text.splitlines()
    out = []
    for line in lines:
        if line.startswith(skip_prefixes):
            continue
        out.append(line)
    text = "\n".join(out) + "\n"
    path.write_text(text)
PY

if [[ -f "${ACCESS_SRC_DIR}/open3dbluraymvc.c" ]]; then
  python3 - <<'PY' "${VLC_SRC}/modules/access/Makefile.am"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
block = """
libopen3dbluraymvc_plugin_la_SOURCES = access/open3dbluraymvc.c
access_LTLIBRARIES += libopen3dbluraymvc_plugin.la
libopen3dbluraymvc_plugin_la_CFLAGS = $(AM_CFLAGS) $(OPEN3DBLURAYMVC_PLUGIN_CFLAGS) $(BLURAY_CFLAGS)
libopen3dbluraymvc_plugin_la_LIBADD = $(OPEN3DBLURAYMVC_PLUGIN_LIBADD) $(BLURAY_LIBS)
libopen3dbluraymvc_plugin_la_LDFLAGS = $(AM_LDFLAGS) -rpath '$(accessdir)'
""".strip()

skip_prefixes = (
    "libopen3dbluraymvc_plugin_la_SOURCES",
    "access_LTLIBRARIES += libopen3dbluraymvc_plugin.la",
    "libopen3dbluraymvc_plugin_la_CFLAGS",
    "libopen3dbluraymvc_plugin_la_LIBADD",
    "libopen3dbluraymvc_plugin_la_LDFLAGS",
)

if "libopen3dbluraymvc_plugin_la_SOURCES" in text:
    lines = text.splitlines()
    out = []
    inserted = False
    for line in lines:
        if line.startswith(skip_prefixes):
            if not inserted:
                out.extend(block.splitlines())
                inserted = True
            continue
        out.append(line)
    text = "\n".join(out) + "\n"
else:
    text += "\n" + block + "\n"

path.write_text(text)
PY
fi

python3 - <<'PY' "${VLC_SRC}/modules/demux/Makefile.am"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
block = """
libopen3dannexb_plugin_la_SOURCES = demux/open3dannexb.c
libopen3dannexb_plugin_la_CFLAGS = $(AM_CFLAGS) $(OPEN3DANNEXB_PLUGIN_CFLAGS)
demux_LTLIBRARIES += libopen3dannexb_plugin.la
""".strip()

skip_prefixes = (
    "libopen3dannexb_plugin_la_SOURCES",
    "libopen3dannexb_plugin_la_CFLAGS",
    "demux_LTLIBRARIES += libopen3dannexb_plugin.la",
    "libopen3dmkv_plugin_la_SOURCES",
    "libopen3dmkv_plugin_la_CFLAGS",
    "demux_LTLIBRARIES += libopen3dmkv_plugin.la",
)

if "libopen3dannexb_plugin_la_SOURCES" in text:
    lines = text.splitlines()
    out = []
    inserted = False
    for line in lines:
        if line.startswith(skip_prefixes):
            if not inserted:
                out.extend(block.splitlines())
                inserted = True
            continue
        out.append(line)
    text = "\n".join(out) + "\n"
else:
    marker = "demux_LTLIBRARIES += librawvid_plugin.la"
    idx = text.find(marker)
    if idx < 0:
        text += "\n" + block + "\n"
    else:
        insert_at = text.find("\n", idx)
        if insert_at < 0:
            text += "\n" + block + "\n"
        else:
            text = text[: insert_at + 1] + block + "\n" + text[insert_at + 1 :]

path.write_text(text)
PY

if ! grep -q "libplaylist_plugin_la_CFLAGS = .*PLAYLIST_PLUGIN_CFLAGS" "${VLC_SRC}/modules/demux/Makefile.am"; then
  python3 - <<'PY' "${VLC_SRC}/modules/demux/Makefile.am"
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
target = "demux_LTLIBRARIES += libplaylist_plugin.la"
idx = text.find(target)
if idx < 0:
    raise SystemExit("playlist plugin stanza not found")
insert = "libplaylist_plugin_la_CFLAGS = $(AM_CFLAGS) $(PLAYLIST_PLUGIN_CFLAGS)\n"
line_start = text.rfind("\n", 0, idx) + 1
text = text[:line_start] + insert + text[line_start:]
path.write_text(text)
PY
fi

if [[ ! -x ./configure ]]; then
  echo "Running bootstrap..."
  ./bootstrap
fi

prepare_qt_svg_overlay || true
prepare_qt_private_overlay
prepare_qt_x11extras_overlay
prepare_matroska_overlay
prepare_libxml2_overlay

if [[ ! -f ./config.status ]] || ! qt_plugin_enabled_in_makefile ||
   ! mkv_plugin_deps_configured || ! wayland_config_matches_requested; then
  echo "Running configure ${CONFIGURE_ARGS[*]:-(default options)}"
  configure_env=()
  if [[ -n "${DVBPSI_BUILD_CFLAGS}" ]]; then
    configure_env+=("DVBPSI_CFLAGS=${DVBPSI_BUILD_CFLAGS}")
  fi
  if [[ -n "${DVBPSI_BUILD_LIBS}" ]]; then
    configure_env+=("DVBPSI_LIBS=${DVBPSI_BUILD_LIBS}")
  fi
  if [[ -n "${WAYLAND_PROTOCOLS:-}" ]]; then
    configure_env+=("WAYLAND_PROTOCOLS=${WAYLAND_PROTOCOLS}")
  fi
  if [[ -n "${WAYLAND_SCANNER:-}" ]]; then
    configure_env+=("WAYLAND_SCANNER=${WAYLAND_SCANNER}")
  fi
  env "${configure_env[@]}" ./configure "${CONFIGURE_ARGS[@]}"
fi

# We patch/copy autotools inputs inside a release tarball. Preserve the
# tarball's generated autotools outputs so GNU make does not try to rerun
# autoconf/automake with a different distro toolchain version.
refresh_release_autotools_outputs
ensure_open3d_wayland_linkage "${VLC_SRC}/modules/Makefile"

OPEN3D_PLUGIN_CFLAGS="${OPEN3D_PLUGIN_CFLAGS:-}"
EDGE264MVC_PLUGIN_CFLAGS="${EDGE264MVC_PLUGIN_CFLAGS:-}"
MVCASM_PLUGIN_CFLAGS="${MVCASM_PLUGIN_CFLAGS:-}"
OPEN3DANNEXB_PLUGIN_CFLAGS="${OPEN3DANNEXB_PLUGIN_CFLAGS:-}"
OPEN3DISO_PLUGIN_CFLAGS="${OPEN3DISO_PLUGIN_CFLAGS:-}"
OPEN3DBLURAYMVC_PLUGIN_CFLAGS="${OPEN3DBLURAYMVC_PLUGIN_CFLAGS:-}"
OPEN3DBLURAYMVC_PLUGIN_LIBADD="${OPEN3DBLURAYMVC_PLUGIN_LIBADD:-}"
PLAYLIST_PLUGIN_CFLAGS="${PLAYLIST_PLUGIN_CFLAGS:-}"
QT_PLUGIN_CFLAGS="${QT_PLUGIN_CFLAGS:-}"
DUMMY_PLUGIN_CFLAGS="${DUMMY_PLUGIN_CFLAGS:-}"
HOTKEYS_PLUGIN_CFLAGS="${HOTKEYS_PLUGIN_CFLAGS:-}"
XCB_HOTKEYS_PLUGIN_CFLAGS="${XCB_HOTKEYS_PLUGIN_CFLAGS:-}"
MKV_PLUGIN_CFLAGS="${MKV_PLUGIN_CFLAGS:-}"
BUILD_JOBS="${OPEN3D_VLC_BUILD_JOBS:-$(nproc)}"
BUILD_PHASE="${OPEN3D_VLC_BUILD_PHASE:-all}"
OPEN3D_BUILD_GIT_SHA="$(git -C "${REPO_DIR}" rev-parse --short HEAD 2>/dev/null || printf 'unknown')"
if ! git -C "${REPO_DIR}" diff --quiet --ignore-submodules -- . 2>/dev/null || \
   ! git -C "${REPO_DIR}" diff --cached --quiet --ignore-submodules -- . 2>/dev/null; then
  OPEN3D_BUILD_GIT_SHA+="-dirty"
fi
OPEN3D_BUILD_TIME="$(date -Iseconds)"
LOCAL_DEVROOT="${REPO_DIR}/local/out/devpkgs/root"

if [[ -d "${LOCAL_DEVROOT}/usr/include/udfread" ]]; then
  if [[ -n "${MVCASM_PLUGIN_CFLAGS}" ]]; then
    MVCASM_PLUGIN_CFLAGS+=" "
  fi
  MVCASM_PLUGIN_CFLAGS+="-I${LOCAL_DEVROOT}/usr/include -DMVCASM_HAVE_UDFREAD=1"
fi

if [[ -d "${LOCAL_DEVROOT}/usr/include/libbluray" ]]; then
  if [[ -n "${OPEN3DBLURAYMVC_PLUGIN_CFLAGS}" ]]; then
    OPEN3DBLURAYMVC_PLUGIN_CFLAGS+=" "
  fi
  OPEN3DBLURAYMVC_PLUGIN_CFLAGS+="-I${LOCAL_DEVROOT}/usr/include"
fi
if [[ -n "${OPEN3DBLURAYMVC_PLUGIN_CFLAGS}" ]]; then
  OPEN3DBLURAYMVC_PLUGIN_CFLAGS+=" "
fi
OPEN3DBLURAYMVC_PLUGIN_CFLAGS+="-DOPEN3DBLURAYMVC_BUILD_GIT_SHA=\\\"${OPEN3D_BUILD_GIT_SHA}\\\" -DOPEN3DBLURAYMVC_BUILD_TIME=\\\"${OPEN3D_BUILD_TIME}\\\""

OPEN3D_VENDOR_LIBBLURAY_STAGE="${OPEN3D_VENDOR_LIBBLURAY_STAGE:-${REPO_DIR}/local/out/vendor_stage/libbluray}"
if [[ -n "${OPEN3D_VENDOR_LIBBLURAY_STAGE}" ]]; then
  BLURAY_STAGE_INCLUDE="${OPEN3D_VENDOR_LIBBLURAY_STAGE}/include"
  BLURAY_STAGE_LIB=""
  BLURAY_STAGE_LIBDIR=""
  for candidate in \
    "${OPEN3D_VENDOR_LIBBLURAY_STAGE}/lib/x86_64-linux-gnu/libbluray.so" \
    "${OPEN3D_VENDOR_LIBBLURAY_STAGE}/lib64/libbluray.so" \
    "${OPEN3D_VENDOR_LIBBLURAY_STAGE}/lib/libbluray.so"; do
    if [[ -f "${candidate}" ]]; then
      BLURAY_STAGE_LIB="${candidate}"
      BLURAY_STAGE_LIBDIR="$(dirname "${candidate}")"
      break
    fi
  done
  if [[ -d "${BLURAY_STAGE_INCLUDE}/libbluray" ]]; then
    if [[ -n "${OPEN3DBLURAYMVC_PLUGIN_CFLAGS}" ]]; then
      OPEN3DBLURAYMVC_PLUGIN_CFLAGS+=" "
    fi
    OPEN3DBLURAYMVC_PLUGIN_CFLAGS+="-I${BLURAY_STAGE_INCLUDE}"
  fi
  if [[ -n "${BLURAY_STAGE_LIBDIR}" ]]; then
    COMMON_STAGE_LIBADD="-L${BLURAY_STAGE_LIBDIR} -lbluray"
    if [[ -n "${OPEN3DBLURAYMVC_PLUGIN_LIBADD}" ]]; then
      OPEN3DBLURAYMVC_PLUGIN_LIBADD+=" "
    fi
    OPEN3DBLURAYMVC_PLUGIN_LIBADD+="${COMMON_STAGE_LIBADD}"
  fi
fi

LINK_SHIM_DIR="${REPO_DIR}/local/out/devpkgs/linkshim/usr/lib/x86_64-linux-gnu"
if [[ -f "${LINK_SHIM_DIR}/libbluray.so" ]]; then
  if [[ -n "${OPEN3DBLURAYMVC_PLUGIN_LIBADD}" ]]; then
    OPEN3DBLURAYMVC_PLUGIN_LIBADD+=" "
  fi
  OPEN3DBLURAYMVC_PLUGIN_LIBADD+="-L${LINK_SHIM_DIR} -lbluray -lxml2 -lfreetype -lfontconfig -ludfread"
elif [[ -f "/usr/lib/x86_64-linux-gnu/libbluray.so.2" ]]; then
  if [[ -n "${OPEN3DBLURAYMVC_PLUGIN_LIBADD}" ]]; then
    OPEN3DBLURAYMVC_PLUGIN_LIBADD+=" "
  fi
  OPEN3DBLURAYMVC_PLUGIN_LIBADD+="/usr/lib/x86_64-linux-gnu/libbluray.so.2 /usr/lib/x86_64-linux-gnu/libxml2.so.2 /usr/lib/x86_64-linux-gnu/libfreetype.so.6 /usr/lib/x86_64-linux-gnu/libfontconfig.so.1 /usr/lib/x86_64-linux-gnu/libudfread.so.0"
elif [[ -f "${LOCAL_DEVROOT}/usr/lib/x86_64-linux-gnu/libbluray.so" ]]; then
  if [[ -n "${OPEN3DBLURAYMVC_PLUGIN_LIBADD}" ]]; then
    OPEN3DBLURAYMVC_PLUGIN_LIBADD+=" "
  fi
  OPEN3DBLURAYMVC_PLUGIN_LIBADD+="${LOCAL_DEVROOT}/usr/lib/x86_64-linux-gnu/libbluray.so ${LOCAL_DEVROOT}/usr/lib/x86_64-linux-gnu/libudfread.so"
fi

echo "Building prerequisites..."
make -j"${BUILD_JOBS}" -C compat libcompat.la
make -C src fourcc_tables.h

if [[ "${BUILD_PHASE}" == "prepare" ]]; then
  echo "Open3D VLC helper phase: prepare only"
  exit 0
fi

echo "Building open3d plugin target..."
DEFAULT_PLUGIN_TARGETS="libdummy_plugin.la libhotkeys_plugin.la liboldrc_plugin.la libxcb_hotkeys_plugin.la libqt_plugin.la libopen3d_plugin.la libedge264mvc_plugin.la libopen3dannexb_plugin.la libmkv_plugin.la libts_plugin.la libtsmvc_plugin.la libplaylist_plugin.la libopen3dbluraymvc_plugin.la"
if [[ "${OPEN3D_APPIMAGE_ENABLE_WAYLAND:-0}" == "1" ]] &&
   grep -Eq '^[^#].*libxdg_shell_plugin.la' modules/Makefile 2>/dev/null; then
  DEFAULT_PLUGIN_TARGETS+=" libxdg_shell_plugin.la"
fi
read -r -a PLUGIN_TARGETS <<< "${OPEN3D_VLC_PLUGIN_TARGETS:-${DEFAULT_PLUGIN_TARGETS}}"
for plugin_target in "${PLUGIN_TARGETS[@]}"; do
  if [[ "${plugin_target}" == "libqt_plugin.la" ]]; then
    build_qt_prerequisites
    break
  fi
done

module_make_args=(
  "OPEN3D_PLUGIN_CFLAGS=${OPEN3D_PLUGIN_CFLAGS}"
  "EDGE264MVC_PLUGIN_CFLAGS=${EDGE264MVC_PLUGIN_CFLAGS}"
  "OPEN3DANNEXB_PLUGIN_CFLAGS=${OPEN3DANNEXB_PLUGIN_CFLAGS}"
  "OPEN3DISO_PLUGIN_CFLAGS=${OPEN3DISO_PLUGIN_CFLAGS}"
  "OPEN3DBLURAYMVC_PLUGIN_CFLAGS=${OPEN3DBLURAYMVC_PLUGIN_CFLAGS}"
  "OPEN3DBLURAYMVC_PLUGIN_LIBADD=${OPEN3DBLURAYMVC_PLUGIN_LIBADD}"
  "PLAYLIST_PLUGIN_CFLAGS=${PLAYLIST_PLUGIN_CFLAGS}"
  "QT_PLUGIN_CFLAGS=${QT_PLUGIN_CFLAGS}"
  "DUMMY_PLUGIN_CFLAGS=${DUMMY_PLUGIN_CFLAGS}"
  "HOTKEYS_PLUGIN_CFLAGS=${HOTKEYS_PLUGIN_CFLAGS}"
  "XCB_HOTKEYS_PLUGIN_CFLAGS=${XCB_HOTKEYS_PLUGIN_CFLAGS}"
  "MKV_PLUGIN_CFLAGS=${MKV_PLUGIN_CFLAGS}"
)
if [[ -n "${DVBPSI_BUILD_CFLAGS}" ]]; then
  module_make_args+=("DVBPSI_CFLAGS=${DVBPSI_BUILD_CFLAGS}")
fi
if [[ -n "${DVBPSI_BUILD_LIBS}" ]]; then
  module_make_args+=("DVBPSI_LIBS=${DVBPSI_BUILD_LIBS}")
fi

# Ensure ABI-flag changes take effect when reusing a build tree.
rm -f modules/video_output/libopen3d_plugin_la-open3d_display.lo \
      modules/.libs/libopen3d_plugin.so \
      modules/libopen3d_plugin.la \
      modules/control/libdummy_plugin_la-dummy.lo \
      modules/.libs/libdummy_plugin.so \
      modules/libdummy_plugin.la \
      modules/control/libhotkeys_plugin_la-hotkeys.lo \
      modules/.libs/libhotkeys_plugin.so \
      modules/libhotkeys_plugin.la \
      modules/control/liboldrc_plugin_la-oldrc.lo \
      modules/.libs/liboldrc_plugin.so \
      modules/liboldrc_plugin.la \
      modules/control/libxcb_hotkeys_plugin_la-xcb.lo \
      modules/.libs/libxcb_hotkeys_plugin.so \
      modules/libxcb_hotkeys_plugin.la \
      modules/gui/qt/libqt_plugin_la-qt.lo \
      modules/gui/qt/components/libqt_plugin_la-open_panels.lo \
      modules/gui/qt/components/libqt_plugin_la-open_panels.moc.lo \
      modules/gui/qt/dialogs/libqt_plugin_la-open.lo \
      modules/.libs/libqt_plugin.so \
      modules/libqt_plugin.la \
      modules/codec/libedge264mvc_plugin_la-edge264mvc.lo \
      modules/.libs/libedge264mvc_plugin.so \
      modules/libedge264mvc_plugin.la \
      modules/demux/libopen3dannexb_plugin_la-open3dannexb.lo \
      modules/.libs/libopen3dannexb_plugin.so \
      modules/libopen3dannexb_plugin.la \
      modules/demux/mkv/libmkv_plugin_la-demux.lo \
      modules/demux/mkv/libmkv_plugin_la-mkv.lo \
      modules/demux/mkv/libmkv_plugin_la-Ebml_parser.lo \
      modules/demux/mkv/libmkv_plugin_la-matroska_segment.lo \
      modules/demux/mkv/libmkv_plugin_la-matroska_segment_parse.lo \
      modules/demux/mkv/libmkv_plugin_la-matroska_segment_seeker.lo \
      modules/demux/mkv/libmkv_plugin_la-stream_io_callback.lo \
      modules/demux/mkv/libmkv_plugin_la-util.lo \
      modules/demux/mkv/libmkv_plugin_la-virtual_segment.lo \
      modules/demux/mkv/libmkv_plugin_la-chapters.lo \
      modules/demux/mkv/libmkv_plugin_la-chapter_command.lo \
      modules/.libs/libmkv_plugin.so \
      modules/libmkv_plugin.la \
      modules/demux/libopen3dmkv_plugin_la-open3dmkv.lo \
      modules/.libs/libopen3dmkv_plugin.so \
      modules/libopen3dmkv_plugin.la \
      modules/demux/mpeg/libts_plugin_la-ts_psi.lo \
      modules/.libs/libts_plugin.so \
      modules/libts_plugin.la \
      modules/demux/mpeg/libtsmvc_plugin_la-ts.lo \
      modules/demux/mpeg/libtsmvc_plugin_la-ts_psi.lo \
      modules/.libs/libtsmvc_plugin.so \
      modules/libtsmvc_plugin.la \
      modules/demux/playlist/libplaylist_plugin_la-mpls.lo \
      modules/.libs/libplaylist_plugin.so \
      modules/libplaylist_plugin.la \
      modules/access/libopen3dbluraymvc_plugin_la-open3dbluraymvc.lo \
      modules/.libs/libopen3dbluraymvc_plugin.so \
      modules/libopen3dbluraymvc_plugin.la \
      modules/video_output/wayland/libxdg_shell_plugin_la-xdg-shell.lo \
      modules/.libs/libxdg_shell_plugin.so \
      modules/libxdg_shell_plugin.la
make -j"${BUILD_JOBS}" -C modules \
  "${module_make_args[@]}" \
  "${PLUGIN_TARGETS[@]}"

echo "Build complete. Typical artifact location:"
echo "  ${VLC_SRC}/modules/.libs/libopen3d_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libedge264mvc_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libopen3dannexb_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libmkv_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libts_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libtsmvc_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libplaylist_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libopen3dbluraymvc_plugin.so"
