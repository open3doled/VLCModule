#!/usr/bin/env python3
from pathlib import Path
import sys


INPUT_C_INCLUDE_ANCHOR = '#include <vlc_renderer_discovery.h>\n'
INPUT_C_EXTRA_INCLUDE = '#include <strings.h>\n'

INPUT_C_HELPERS = r"""
static bool Open3DPathEndsWithIcase(const char *psz_path, const char *psz_suffix)
{
    if (psz_path == NULL || psz_suffix == NULL)
        return false;

    const size_t path_len = strlen(psz_path);
    const size_t suffix_len = strlen(psz_suffix);
    if (path_len < suffix_len)
        return false;

    return strcasecmp(psz_path + path_len - suffix_len, psz_suffix) == 0;
}

static char *Open3DNormalizeBlurayPath(const char *psz_path)
{
    if (psz_path == NULL || *psz_path == '\0')
        return NULL;

    char *psz_real = realpath(psz_path, NULL);
    if (psz_real == NULL)
        return NULL;

    struct stat st;
    if (vlc_stat(psz_real, &st) != 0)
    {
        free(psz_real);
        return NULL;
    }

    if (S_ISREG(st.st_mode))
    {
        if (Open3DPathEndsWithIcase(psz_real, ".iso"))
            return psz_real;

        if (Open3DPathEndsWithIcase(psz_real, "/BDMV/index.bdmv"))
        {
            psz_real[strlen(psz_real) - strlen("/BDMV/index.bdmv")] = '\0';
            return psz_real;
        }
        return NULL;
    }

    if (!S_ISDIR(st.st_mode))
    {
        free(psz_real);
        return NULL;
    }

    if (Open3DPathEndsWithIcase(psz_real, "/BDMV"))
    {
        psz_real[strlen(psz_real) - strlen("/BDMV")] = '\0';
        return psz_real;
    }

    char *psz_probe = NULL;
    if (asprintf(&psz_probe, "%s/BDMV/index.bdmv", psz_real) < 0)
    {
        free(psz_real);
        return NULL;
    }

    const bool has_index = (vlc_stat(psz_probe, &st) == 0 && S_ISREG(st.st_mode));
    free(psz_probe);
    if (has_index)
        return psz_real;

    free(psz_real);
    return NULL;
}

static bool Open3DMaybeRouteDirectBlurayPath(input_thread_t *p_input,
                                             const char **ppsz_access,
                                             const char **ppsz_demux,
                                             const char **ppsz_path)
{
    if (ppsz_access == NULL || ppsz_demux == NULL || ppsz_path == NULL ||
        *ppsz_access == NULL || *ppsz_path == NULL)
        return false;

    if (strcasecmp(*ppsz_access, "file") != 0)
        return false;

    char *psz_bluray_path = Open3DNormalizeBlurayPath(*ppsz_path);
    if (psz_bluray_path == NULL)
        return false;

    *ppsz_access = "open3dbluraymvc";
    *ppsz_demux = "any";
    *ppsz_path = psz_bluray_path;
    var_Create(p_input, "bluray-menu", VLC_VAR_BOOL);
    var_SetBool(p_input, "bluray-menu", false);
    msg_Dbg(p_input, "open3d direct path routing '%s' -> access=open3dbluraymvc",
            psz_bluray_path);
    return true;
}

"""

INPUT_SOURCE_SPLIT_ANCHOR = """    if( psz_forced_demux != NULL )\n        psz_demux = psz_forced_demux;\n\n    if( psz_demux == NULL )\n        psz_demux = \"any\";\n"""
INPUT_SOURCE_SPLIT_INSERT = """    if( psz_forced_demux != NULL )\n        psz_demux = psz_forced_demux;\n\n    if( psz_demux == NULL )\n        psz_demux = \"any\";\n\n    char *psz_open3d_bluray_path = NULL;\n    if( Open3DMaybeRouteDirectBlurayPath( p_input, &psz_access, &psz_demux,\n                                          &psz_path ) )\n        psz_open3d_bluray_path = (char *)psz_path;\n"""

INPUT_SOURCE_FREE_OLD = """    free( psz_demux_var );\n    free( psz_dup );\n"""
INPUT_SOURCE_FREE_NEW = """    free( psz_demux_var );\n    free( psz_dup );\n    free( psz_open3d_bluray_path );\n"""


def ensure_replace(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old in text:
        return text.replace(old, new, 1)
    raise SystemExit(label)


def patch_tree(tree_root: Path) -> None:
    path = tree_root / "src/input/input.c"
    text = path.read_text()

    if INPUT_C_EXTRA_INCLUDE not in text:
        if INPUT_C_INCLUDE_ANCHOR not in text:
            raise SystemExit(f"{path}: input.c include anchor not found")
        text = text.replace(INPUT_C_INCLUDE_ANCHOR,
                            INPUT_C_INCLUDE_ANCHOR + INPUT_C_EXTRA_INCLUDE, 1)

    if "Open3DMaybeRouteDirectBlurayPath" not in text:
        anchor = "/*****************************************************************************\n * InputSourceNew:\n *****************************************************************************/\n"
        if anchor not in text:
            raise SystemExit(f"{path}: InputSourceNew anchor not found")
        text = text.replace(anchor, INPUT_C_HELPERS + "\n" + anchor, 1)

    text = ensure_replace(
        text, INPUT_SOURCE_SPLIT_ANCHOR, INPUT_SOURCE_SPLIT_INSERT,
        f"{path}: InputSourceNew split block not found",
    )
    text = ensure_replace(
        text, INPUT_SOURCE_FREE_OLD, INPUT_SOURCE_FREE_NEW,
        f"{path}: InputSourceNew free block not found",
    )

    path.write_text(text)


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: patch_vlc3_open3d_input_routing.py /path/to/vlc-source", file=sys.stderr)
        return 2

    patch_tree(Path(sys.argv[1]).resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
