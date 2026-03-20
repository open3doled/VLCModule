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
EDGE264MVC_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/codec/edge264mvc.c"
OPEN3DMKV_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/open3dmkv.c"
TS_PSI_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/mpeg/ts_psi.c"
TS_C_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/mpeg/ts.c"
DEMUX_MAKEFILE_SRC="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/Makefile.am"
PLAYLIST_SRC_DIR="${REPO_DIR}/renderer/vlc-3.0.23/modules/demux/playlist"
ACCESS_SRC_DIR="${REPO_DIR}/renderer/vlc-3.0.23/modules/access"
APPLY_PATCH_SCRIPT="${REPO_DIR}/scripts/apply_vlc3_patch.sh"

if [[ ! -d "${VLC_SRC}/modules/video_output" ]]; then
  echo "Error: ${VLC_SRC} does not look like a VLC source tree" >&2
  exit 2
fi

cd "${VLC_SRC}"

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

if [[ -f "${MODULE_SRC}" ]]; then
  cp "${MODULE_SRC}" "${VLC_SRC}/modules/video_output/open3d_display.c"
fi
if [[ -f "${MODULE_FONT_SRC}" ]]; then
  cp "${MODULE_FONT_SRC}" "${VLC_SRC}/modules/video_output/open3d_font8x16_basic.h"
fi
if [[ -f "${EDGE264MVC_SRC}" ]]; then
  cp "${EDGE264MVC_SRC}" "${VLC_SRC}/modules/codec/edge264mvc.c"
fi
if [[ -f "${OPEN3DMKV_SRC}" ]]; then
  cp "${OPEN3DMKV_SRC}" "${VLC_SRC}/modules/demux/open3dmkv.c"
fi
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
libopen3dmkv_plugin_la_SOURCES = demux/open3dmkv.c
libopen3dmkv_plugin_la_CFLAGS = $(AM_CFLAGS) $(OPEN3DMKV_PLUGIN_CFLAGS)
demux_LTLIBRARIES += libopen3dmkv_plugin.la
""".strip()

skip_prefixes = (
    "libopen3dmkv_plugin_la_SOURCES",
    "libopen3dmkv_plugin_la_CFLAGS",
    "demux_LTLIBRARIES += libopen3dmkv_plugin.la",
)

if "libopen3dmkv_plugin_la_SOURCES" in text:
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

if [[ ! -f ./config.status ]]; then
  echo "Running configure ${CONFIGURE_ARGS[*]:-(default options)}"
  ./configure "${CONFIGURE_ARGS[@]}"
fi

OPEN3D_PLUGIN_CFLAGS="${OPEN3D_PLUGIN_CFLAGS:-}"
EDGE264MVC_PLUGIN_CFLAGS="${EDGE264MVC_PLUGIN_CFLAGS:-}"
MVCASM_PLUGIN_CFLAGS="${MVCASM_PLUGIN_CFLAGS:-}"
OPEN3DMKV_PLUGIN_CFLAGS="${OPEN3DMKV_PLUGIN_CFLAGS:-}"
OPEN3DISO_PLUGIN_CFLAGS="${OPEN3DISO_PLUGIN_CFLAGS:-}"
OPEN3DBLURAYMVC_PLUGIN_CFLAGS="${OPEN3DBLURAYMVC_PLUGIN_CFLAGS:-}"
OPEN3DBLURAYMVC_PLUGIN_LIBADD="${OPEN3DBLURAYMVC_PLUGIN_LIBADD:-}"
PLAYLIST_PLUGIN_CFLAGS="${PLAYLIST_PLUGIN_CFLAGS:-}"
DVBPSI_BUILD_CFLAGS="${DVBPSI_BUILD_CFLAGS:-}"
DVBPSI_BUILD_LIBS="${DVBPSI_BUILD_LIBS:-}"
BUILD_JOBS="${OPEN3D_VLC_BUILD_JOBS:-$(nproc)}"
OPEN3D_BUILD_GIT_SHA="$(git -C "${REPO_DIR}" rev-parse --short HEAD 2>/dev/null || printf 'unknown')"
if ! git -C "${REPO_DIR}" diff --quiet --ignore-submodules -- . 2>/dev/null || \
   ! git -C "${REPO_DIR}" diff --cached --quiet --ignore-submodules -- . 2>/dev/null; then
  OPEN3D_BUILD_GIT_SHA+="-dirty"
fi
OPEN3D_BUILD_TIME="$(date -Iseconds)"
LOCAL_DEVROOT="${REPO_DIR}/local/out/devpkgs/root"
LOCAL_DVBPSI_DEVROOT="${REPO_DIR}/local/out/devpkgs/dvbpsi_root"
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
  if [[ -n "${OPEN3DMKV_PLUGIN_CFLAGS}" ]]; then
    OPEN3DMKV_PLUGIN_CFLAGS+=" "
  fi
  OPEN3DMKV_PLUGIN_CFLAGS+="-DOPEN3D_VLC_ABI_ALIAS_T64=1"
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
if [[ -f "${LOCAL_DVBPSI_DEVROOT}/usr/include/dvbpsi/dvbpsi.h" ]]; then
  DVBPSI_BUILD_CFLAGS="-I${LOCAL_DVBPSI_DEVROOT}/usr/include"
  DVBPSI_BUILD_LIBS="-L${LOCAL_DVBPSI_DEVROOT}/usr/lib/x86_64-linux-gnu -ldvbpsi"
fi

OPEN3D_VENDOR_LIBBLURAY_STAGE="${OPEN3D_VENDOR_LIBBLURAY_STAGE:-}"
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
make -j"${BUILD_JOBS}" -C src libvlccore.la

echo "Building open3d plugin target..."
DEFAULT_PLUGIN_TARGETS="libopen3d_plugin.la libedge264mvc_plugin.la libopen3dmkv_plugin.la libts_plugin.la libtsmvc_plugin.la libplaylist_plugin.la libopen3dbluraymvc_plugin.la"
read -r -a PLUGIN_TARGETS <<< "${OPEN3D_VLC_PLUGIN_TARGETS:-${DEFAULT_PLUGIN_TARGETS}}"
# Ensure ABI-flag changes take effect when reusing a build tree.
rm -f modules/video_output/libopen3d_plugin_la-open3d_display.lo \
      modules/.libs/libopen3d_plugin.so \
      modules/libopen3d_plugin.la \
      modules/codec/libedge264mvc_plugin_la-edge264mvc.lo \
      modules/.libs/libedge264mvc_plugin.so \
      modules/libedge264mvc_plugin.la \
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
  OPEN3DMKV_PLUGIN_CFLAGS="${OPEN3DMKV_PLUGIN_CFLAGS}" \
  OPEN3DISO_PLUGIN_CFLAGS="${OPEN3DISO_PLUGIN_CFLAGS}" \
  OPEN3DBLURAYMVC_PLUGIN_CFLAGS="${OPEN3DBLURAYMVC_PLUGIN_CFLAGS}" \
  OPEN3DBLURAYMVC_PLUGIN_LIBADD="${OPEN3DBLURAYMVC_PLUGIN_LIBADD}" \
  DVBPSI_CFLAGS="${DVBPSI_BUILD_CFLAGS}" \
  DVBPSI_LIBS="${DVBPSI_BUILD_LIBS}" \
  PLAYLIST_PLUGIN_CFLAGS="${PLAYLIST_PLUGIN_CFLAGS}" \
  "${PLUGIN_TARGETS[@]}"

echo "Build complete. Typical artifact location:"
echo "  ${VLC_SRC}/modules/.libs/libopen3d_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libedge264mvc_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libopen3dmkv_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libts_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libtsmvc_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libplaylist_plugin.so"
echo "  ${VLC_SRC}/modules/.libs/libopen3dbluraymvc_plugin.so"
