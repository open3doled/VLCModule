#!/usr/bin/env python3
from pathlib import Path
import sys


REGISTER_NEEDLE = """    ADD( "chapter", STRING, Input )
    ADD( "chapter_n", VOID, Input )
    ADD( "chapter_p", VOID, Input )

    ADD( "fastforward", VOID, Input )
"""

REGISTER_REPLACEMENT = """    ADD( "chapter", STRING, Input )
    ADD( "chapter_n", VOID, Input )
    ADD( "chapter_p", VOID, Input )
    ADD( "menu-popup", VOID, Input )

    ADD( "fastforward", VOID, Input )
"""

INPUT_NEEDLE = """    else if ( !strcmp( psz_cmd, "frame" ) )
    {
        var_TriggerCallback( p_input, "frame-next" );
        i_error = VLC_SUCCESS;
    }
    else if( !strcmp( psz_cmd, "chapter" ) ||
"""

INPUT_REPLACEMENT = """    else if ( !strcmp( psz_cmd, "frame" ) )
    {
        var_TriggerCallback( p_input, "frame-next" );
        i_error = VLC_SUCCESS;
    }
    else if( !strcmp( psz_cmd, "menu-popup" ) )
    {
        if( var_Type( p_input, "menu-popup" ) != 0 )
        {
            var_TriggerCallback( p_input, "menu-popup" );
            i_error = VLC_SUCCESS;
        }
    }
    else if( !strcmp( psz_cmd, "chapter" ) ||
"""

HELP_NEEDLE = """    msg_rc("%s", _("| strack [X] . . . . . . . . .  set/get subtitle track"));
    msg_rc("%s", _("| key [hotkey name] . . . . . .  simulate hotkey press"));
"""

HELP_REPLACEMENT = """    msg_rc("%s", _("| strack [X] . . . . . . . . .  set/get subtitle track"));
    msg_rc("%s", _("| key [hotkey name] . . . . . .  simulate hotkey press"));
    msg_rc("%s", _("| menu-popup . . . . . . . . trigger Blu-ray popup navigation"));
"""

CAPABILITY_NEEDLE = '    set_capability( "interface", 20 )\n'
CAPABILITY_REPLACEMENT = '    set_capability( "interface", 21 )\n'


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"oldrc.c: expected {label} anchor not found")
    return text.replace(old, new, 1)


def patch_tree(tree_root: Path) -> None:
    path = tree_root / "modules/control/oldrc.c"
    text = path.read_text()

    if 'ADD( "menu-popup", VOID, Input )' not in text:
        text = replace_once(text, REGISTER_NEEDLE, REGISTER_REPLACEMENT, "register block")

    if '!strcmp( psz_cmd, "menu-popup" )' not in text:
        text = replace_once(text, INPUT_NEEDLE, INPUT_REPLACEMENT, "input block")

    if "| menu-popup " not in text:
        text = replace_once(text, HELP_NEEDLE, HELP_REPLACEMENT, "help block")

    if CAPABILITY_REPLACEMENT not in text:
        text = replace_once(text, CAPABILITY_NEEDLE, CAPABILITY_REPLACEMENT, "capability line")

    path.write_text(text)


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: patch_vlc3_oldrc_popup.py /path/to/vlc-source", file=sys.stderr)
        return 2

    tree_root = Path(sys.argv[1]).resolve()
    patch_tree(tree_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
