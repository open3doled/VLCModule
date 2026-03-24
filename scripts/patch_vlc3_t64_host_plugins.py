#!/usr/bin/env python3
from pathlib import Path
import re
import sys


ALIAS_BLOCK = """/*
 * Host distro VLC 3.0.x builds may look for the Debian/Ubuntu t64 module
 * entry symbol even when building against the upstream 3.0.23 source tree.
 * Export both entry names so the staged plugin can be loaded by the host
 * launcher without affecting the AppImage runtime.
 */
#ifdef __cplusplus
extern "C" {
#endif
extern int CDECL_SYMBOL vlc_entry__3_0_0f(vlc_set_cb, void *);
extern const char *CDECL_SYMBOL vlc_entry_copyright__3_0_0f(void);
extern const char *CDECL_SYMBOL vlc_entry_license__3_0_0f(void);
EXTERN_SYMBOL DLL_SYMBOL int CDECL_SYMBOL vlc_entry__3_0_0ft64(vlc_set_cb, void *);
EXTERN_SYMBOL DLL_SYMBOL const char *CDECL_SYMBOL vlc_entry_copyright__3_0_0ft64(void);
EXTERN_SYMBOL DLL_SYMBOL const char *CDECL_SYMBOL vlc_entry_license__3_0_0ft64(void);

EXTERN_SYMBOL DLL_SYMBOL int CDECL_SYMBOL
vlc_entry__3_0_0ft64(vlc_set_cb vlc_set, void *opaque)
{
    return vlc_entry__3_0_0f(vlc_set, opaque);
}

EXTERN_SYMBOL DLL_SYMBOL const char *CDECL_SYMBOL
vlc_entry_copyright__3_0_0ft64(void)
{
    return vlc_entry_copyright__3_0_0f();
}

EXTERN_SYMBOL DLL_SYMBOL const char *CDECL_SYMBOL
vlc_entry_license__3_0_0ft64(void)
{
    return vlc_entry_license__3_0_0f();
}
#ifdef __cplusplus
}
#endif

"""

GUARDED_ALIAS_BLOCK = "#if defined(OPEN3D_VLC_ABI_ALIAS_T64)\n" + ALIAS_BLOCK + "#endif\n\n"
ALIAS_COMMENT_MARKER = "/*\n * Host distro VLC 3.0.x builds may look for the Debian/Ubuntu t64 module\n"
ALIAS_DECL_MARKER = "extern int CDECL_SYMBOL vlc_entry__3_0_0f(vlc_set_cb, void *);\n"
ALIAS_END_MARKER = "    return vlc_entry_license__3_0_0f();\n}\n"


def normalize_guarded_alias_block(text: str) -> str:
    guard = "#if defined(OPEN3D_VLC_ABI_ALIAS_T64)\n"
    start = text.find(guard)
    while start >= 0:
        end = text.find("#endif", start)
        if end < 0:
            break
        end = text.find("\n", end)
        if end < 0:
            end = len(text)
        else:
            end += 1
        block = text[start:end]
        if "vlc_entry__3_0_0ft64" in block and "vlc_entry__3_0_0f" in block:
            text = text[:start] + ALIAS_BLOCK + text[end:]
            start = text.find(guard, start + len(ALIAS_BLOCK))
            continue
        start = text.find(guard, end)
    return text


def strip_existing_alias_block(text: str) -> str:
    if "vlc_entry__3_0_0ft64" not in text:
        return text

    start = text.find(ALIAS_COMMENT_MARKER)
    if start < 0:
        start = text.find(ALIAS_DECL_MARKER)
    if start < 0:
        return text

    end = text.find(ALIAS_END_MARKER, start)
    if end < 0:
        return text
    end += len(ALIAS_END_MARKER)

    for trailer in ("#ifdef __cplusplus\n}\n#endif\n", "#endif\n"):
        if text.startswith(trailer, end):
            end += len(trailer)

    while end < len(text) and text[end] == "\n":
        end += 1

    return text[:start] + text[end:]


def patch_source(path: Path, anchor: str) -> None:
    text = path.read_text()
    text = text.replace(GUARDED_ALIAS_BLOCK, ALIAS_BLOCK)
    text = normalize_guarded_alias_block(text)
    text = strip_existing_alias_block(text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    if "vlc_entry__3_0_0ft64" not in text:
        if anchor not in text:
            raise SystemExit(f"{path}: anchor not found")
        text = text.replace(anchor, anchor + "\n" + ALIAS_BLOCK, 1)
    path.write_text(text)


def patch_control_makefile(path: Path) -> None:
    text = path.read_text()
    if "libdummy_plugin_la_CFLAGS = $(AM_CFLAGS) $(DUMMY_PLUGIN_CFLAGS)\n" not in text:
        text = text.replace(
            "libdummy_plugin_la_SOURCES = control/dummy.c control/intromsg.h\n",
            "libdummy_plugin_la_SOURCES = control/dummy.c control/intromsg.h\n"
            "libdummy_plugin_la_CFLAGS = $(AM_CFLAGS) $(DUMMY_PLUGIN_CFLAGS)\n",
            1,
        )
    if "libhotkeys_plugin_la_CFLAGS = $(AM_CFLAGS) $(HOTKEYS_PLUGIN_CFLAGS)\n" not in text:
        text = text.replace(
            "libhotkeys_plugin_la_SOURCES = control/hotkeys.c\n",
            "libhotkeys_plugin_la_SOURCES = control/hotkeys.c\n"
            "libhotkeys_plugin_la_CFLAGS = $(AM_CFLAGS) $(HOTKEYS_PLUGIN_CFLAGS)\n",
            1,
        )
    old = "libxcb_hotkeys_plugin_la_CFLAGS = $(AM_CFLAGS) \\\n\t$(XCB_KEYSYMS_CFLAGS) $(XCB_CFLAGS)\n"
    new = "libxcb_hotkeys_plugin_la_CFLAGS = $(AM_CFLAGS) $(XCB_HOTKEYS_PLUGIN_CFLAGS) \\\n\t$(XCB_KEYSYMS_CFLAGS) $(XCB_CFLAGS)\n"
    if old in text:
        text = text.replace(old, new, 1)
    path.write_text(text)


def patch_qt_makefile(path: Path) -> None:
    text = path.read_text()
    old = "libqt_plugin_la_CXXFLAGS = $(AM_CXXFLAGS) $(QT_CFLAGS) $(CXXFLAGS_qt)\n"
    new = "libqt_plugin_la_CXXFLAGS = $(AM_CXXFLAGS) $(QT_CFLAGS) $(QT_PLUGIN_CFLAGS) $(CXXFLAGS_qt)\n"
    if old in text:
        text = text.replace(old, new, 1)
    path.write_text(text)


def patch_tree(tree_root: Path) -> None:
    patch_source(
        tree_root / "modules/gui/qt/qt.cpp",
        "#include <vlc_vout_window.h>",
    )
    patch_source(
        tree_root / "modules/control/dummy.c",
        "#include <vlc_interface.h>",
    )
    patch_source(
        tree_root / "modules/control/hotkeys.c",
        '#include "math.h"',
    )
    patch_source(
        tree_root / "modules/control/globalhotkeys/xcb.c",
        "#include <poll.h>",
    )
    patch_control_makefile(tree_root / "modules/control/Makefile.am")
    patch_qt_makefile(tree_root / "modules/gui/qt/Makefile.am")


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: patch_vlc3_t64_host_plugins.py /path/to/vlc-source", file=sys.stderr)
        return 2

    tree_root = Path(sys.argv[1]).resolve()
    patch_tree(tree_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
