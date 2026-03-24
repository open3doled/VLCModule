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
PATCH_T64_HOST_PLUGINS_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_t64_host_plugins.py"
PATCH_MKV_EDGE264MVC_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_mkv_edge264mvc.py"
PATCH_INPUT_ROUTING_SCRIPT="${REPO_DIR}/scripts/patch_vlc3_open3d_input_routing.py"

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

qt_plugin_enabled_in_makefile() {
  grep -q '^am__append_145 = libqt_plugin.la' modules/Makefile 2>/dev/null
}

mkv_plugin_deps_configured() {
  grep -Eq '^MATROSKA_LIBS = .*(-lmatroska|libmatroska)' modules/Makefile 2>/dev/null
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

DVBPSI_BUILD_CFLAGS="${DVBPSI_BUILD_CFLAGS:-}"
DVBPSI_BUILD_LIBS="${DVBPSI_BUILD_LIBS:-}"
LOCAL_DVBPSI_DEVROOT="${REPO_DIR}/local/out/devpkgs/dvbpsi_root"
if [[ -f "${LOCAL_DVBPSI_DEVROOT}/usr/include/dvbpsi/dvbpsi.h" ]]; then
  DVBPSI_BUILD_CFLAGS="-I${LOCAL_DVBPSI_DEVROOT}/usr/include"
  DVBPSI_BUILD_LIBS="-L${LOCAL_DVBPSI_DEVROOT}/usr/lib/x86_64-linux-gnu -ldvbpsi"
fi
export DVBPSI_CFLAGS="${DVBPSI_BUILD_CFLAGS}"
export DVBPSI_LIBS="${DVBPSI_BUILD_LIBS}"

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
if [[ -x "${PATCH_T64_HOST_PLUGINS_SCRIPT}" ]]; then
  python3 "${PATCH_T64_HOST_PLUGINS_SCRIPT}" "${VLC_SRC}"
fi
if [[ -x "${PATCH_MKV_EDGE264MVC_SCRIPT}" ]]; then
  python3 "${PATCH_MKV_EDGE264MVC_SCRIPT}" "${VLC_SRC}"
fi
if [[ -x "${PATCH_INPUT_ROUTING_SCRIPT}" ]]; then
  python3 "${PATCH_INPUT_ROUTING_SCRIPT}" "${VLC_SRC}"
fi

if [[ -f "${MODULE_SRC}" ]]; then
  cp "${MODULE_SRC}" "${VLC_SRC}/modules/video_output/open3d_display.c"
fi
if [[ -f "${MODULE_FONT_SRC}" ]]; then
  cp "${MODULE_FONT_SRC}" "${VLC_SRC}/modules/video_output/open3d_font8x16_basic.h"
fi
if [[ -f "${MODULE_SUBTITLE_BRIDGE_SRC}" ]]; then
  cp "${MODULE_SUBTITLE_BRIDGE_SRC}" "${VLC_SRC}/modules/video_output/open3d_subtitle_bridge.h"
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
prepare_matroska_overlay

if [[ ! -f ./config.status ]] || ! qt_plugin_enabled_in_makefile || ! mkv_plugin_deps_configured; then
  echo "Running configure ${CONFIGURE_ARGS[*]:-(default options)}"
  DVBPSI_CFLAGS="${DVBPSI_BUILD_CFLAGS}" \
  DVBPSI_LIBS="${DVBPSI_BUILD_LIBS}" \
  ./configure "${CONFIGURE_ARGS[@]}"
fi

# We patch/copy autotools inputs inside a release tarball. Preserve the
# tarball's generated autotools outputs so GNU make does not try to rerun
# autoconf/automake with a different distro toolchain version.
refresh_release_autotools_outputs

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
ABI_ALIAS_MODE="${OPEN3D_VLC_ABI_ALIAS_T64:-auto}"
SOURCE_HAS_T64_SYMBOLS=0
if grep -q '^# define MODULE_SYMBOL 3_0_0ft64' include/vlc_plugin.h; then
  SOURCE_HAS_T64_SYMBOLS=1
fi

ENABLE_ABI_ALIAS=0
case "${ABI_ALIAS_MODE}" in
  1|true|TRUE|yes|YES)
    if [[ "${SOURCE_HAS_T64_SYMBOLS}" == "1" ]]; then
      echo "Error: OPEN3D_VLC_ABI_ALIAS_T64=${ABI_ALIAS_MODE} was requested, but this VLC source already uses t64 module symbols." >&2
      echo "Disable OPEN3D_VLC_ABI_ALIAS_T64 (or set to auto) for this source tree." >&2
      exit 6
    fi
    ENABLE_ABI_ALIAS=1
    ;;
  0|false|FALSE|no|NO)
    ENABLE_ABI_ALIAS=0
    ;;
  auto|AUTO)
    if [[ "${SOURCE_HAS_T64_SYMBOLS}" == "0" ]]; then
      ENABLE_ABI_ALIAS=1
    fi
    ;;
  *)
    echo "Error: invalid OPEN3D_VLC_ABI_ALIAS_T64=${ABI_ALIAS_MODE} (expected auto|1|0|true|false|yes|no)" >&2
    exit 7
    ;;
esac

if [[ "${ENABLE_ABI_ALIAS}" == "1" ]]; then
  if [[ -n "${OPEN3D_PLUGIN_CFLAGS}" ]]; then
    OPEN3D_PLUGIN_CFLAGS+=" "
  fi
  OPEN3D_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${EDGE264MVC_PLUGIN_CFLAGS}" ]]; then
    EDGE264MVC_PLUGIN_CFLAGS+=" "
  fi
  EDGE264MVC_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${MVCASM_PLUGIN_CFLAGS}" ]]; then
    MVCASM_PLUGIN_CFLAGS+=" "
  fi
  MVCASM_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${OPEN3DANNEXB_PLUGIN_CFLAGS}" ]]; then
    OPEN3DANNEXB_PLUGIN_CFLAGS+=" "
  fi
  OPEN3DANNEXB_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${OPEN3DISO_PLUGIN_CFLAGS}" ]]; then
    OPEN3DISO_PLUGIN_CFLAGS+=" "
  fi
  OPEN3DISO_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${PLAYLIST_PLUGIN_CFLAGS}" ]]; then
    PLAYLIST_PLUGIN_CFLAGS+=" "
  fi
  PLAYLIST_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${OPEN3DBLURAYMVC_PLUGIN_CFLAGS}" ]]; then
    OPEN3DBLURAYMVC_PLUGIN_CFLAGS+=" "
  fi
  OPEN3DBLURAYMVC_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${QT_PLUGIN_CFLAGS}" ]]; then
    QT_PLUGIN_CFLAGS+=" "
  fi
  QT_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${DUMMY_PLUGIN_CFLAGS}" ]]; then
    DUMMY_PLUGIN_CFLAGS+=" "
  fi
  DUMMY_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${HOTKEYS_PLUGIN_CFLAGS}" ]]; then
    HOTKEYS_PLUGIN_CFLAGS+=" "
  fi
  HOTKEYS_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${XCB_HOTKEYS_PLUGIN_CFLAGS}" ]]; then
    XCB_HOTKEYS_PLUGIN_CFLAGS+=" "
  fi
  XCB_HOTKEYS_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  if [[ -n "${MKV_PLUGIN_CFLAGS}" ]]; then
    MKV_PLUGIN_CFLAGS+=" "
  fi
  MKV_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
  echo "Enabling Open3D t64 ABI compatibility alias for system VLC plugin loading (OPEN3D_VLC_ABI_ALIAS_T64=${ABI_ALIAS_MODE})."
fi

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
DEFAULT_PLUGIN_TARGETS="libdummy_plugin.la libhotkeys_plugin.la libxcb_hotkeys_plugin.la libqt_plugin.la libopen3d_plugin.la libedge264mvc_plugin.la libopen3dannexb_plugin.la libmkv_plugin.la libts_plugin.la libtsmvc_plugin.la libplaylist_plugin.la libopen3dbluraymvc_plugin.la"
read -r -a PLUGIN_TARGETS <<< "${OPEN3D_VLC_PLUGIN_TARGETS:-${DEFAULT_PLUGIN_TARGETS}}"
for plugin_target in "${PLUGIN_TARGETS[@]}"; do
  if [[ "${plugin_target}" == "libqt_plugin.la" ]]; then
    build_qt_prerequisites
    break
  fi
done
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
      modules/libopen3dbluraymvc_plugin.la
make -j"${BUILD_JOBS}" -C modules \
  OPEN3D_PLUGIN_CFLAGS="${OPEN3D_PLUGIN_CFLAGS}" \
  EDGE264MVC_PLUGIN_CFLAGS="${EDGE264MVC_PLUGIN_CFLAGS}" \
  OPEN3DANNEXB_PLUGIN_CFLAGS="${OPEN3DANNEXB_PLUGIN_CFLAGS}" \
  OPEN3DISO_PLUGIN_CFLAGS="${OPEN3DISO_PLUGIN_CFLAGS}" \
  OPEN3DBLURAYMVC_PLUGIN_CFLAGS="${OPEN3DBLURAYMVC_PLUGIN_CFLAGS}" \
  OPEN3DBLURAYMVC_PLUGIN_LIBADD="${OPEN3DBLURAYMVC_PLUGIN_LIBADD}" \
  DVBPSI_CFLAGS="${DVBPSI_BUILD_CFLAGS}" \
  DVBPSI_LIBS="${DVBPSI_BUILD_LIBS}" \
  PLAYLIST_PLUGIN_CFLAGS="${PLAYLIST_PLUGIN_CFLAGS}" \
  QT_PLUGIN_CFLAGS="${QT_PLUGIN_CFLAGS}" \
  DUMMY_PLUGIN_CFLAGS="${DUMMY_PLUGIN_CFLAGS}" \
  HOTKEYS_PLUGIN_CFLAGS="${HOTKEYS_PLUGIN_CFLAGS}" \
  XCB_HOTKEYS_PLUGIN_CFLAGS="${XCB_HOTKEYS_PLUGIN_CFLAGS}" \
  MKV_PLUGIN_CFLAGS="${MKV_PLUGIN_CFLAGS}" \
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
