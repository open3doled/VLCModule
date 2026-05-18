# VLC Hotkey Conflict Analysis & Resolution Plan

## Executive Summary

The open3d_display VLC renderer uses many single-letter key bindings for calibration-mode controls. These directly conflict with VLC 3.0.23's default keyboard shortcuts. The AppImage's AppRun script uses `--ignore-config` (via `OPEN3D_MVC_IGNORE_CONFIG=1`), so config-file-based overrides won't work.

**Solution:** Add `--key-<action>=` command-line arguments to the AppRun script's `vlc_args` array, which overrides VLC's compiled-in defaults at runtime without any source code changes to VLC.

## How VLC Key Handling Works

1. **VLC's actions system** maps key presses to action IDs via config entries (compiled-in defaults from `libvlc-module.c`)
2. **hotkeys_plugin** receives `key-pressed` callbacks and dispatches matching actions
3. **open3d_display** also receives `key-pressed` callbacks via `var_AddCallback()`
4. **Both modules receive the same events** — if a key is bound to a VLC action, VLC executes it AND open3d_display processes it

## Critical Discovery: --ignore-config

The AppRun script (line 255) sets `OPEN3D_MVC_IGNORE_CONFIG=1` by default, which adds `--ignore-config` to VLC's args (line 337). This means:
- VLC **does NOT** read `~/.config/vlc/vlcrc` at runtime
- The user's existing customized vlcrc has **no effect** during normal AppImage operation
- The source defaults in open3d_display.c are the actual defaults used at runtime

## Complete Conflict Mapping

### open3d_display Source Defaults (from open3d_display.c)

| open3d_key | Source Default |
|-----------|---------------|
| open3d-key-toggle-enabled | Ctrl+Shift+F8 |
| open3d-key-toggle-trigger | Ctrl+Shift+F9 |
| open3d-key-toggle-calibration | Ctrl+Shift+F10 |
| open3d-key-settings | Ctrl+Shift+F7 |
| open3d-key-help | Ctrl+Shift+F11 |
| open3d-key-flip-eyes | Ctrl+Shift+F12 |
| open3d-key-emitter-read | r |
| open3d-key-emitter-apply | y |
| open3d-key-emitter-save | u |
| open3d-key-emitter-reconnect | j |
| open3d-key-emitter-firmware-update | (empty) |
| open3d-key-calib-g | g |
| open3d-key-calib-t | t |
| open3d-key-calib-b | b |
| open3d-key-calib-p | p |
| open3d-key-calib-w | w |
| open3d-key-calib-s | s |
| open3d-key-calib-a | a |
| open3d-key-calib-d | d |
| open3d-key-calib-q | q |
| open3d-key-calib-e | e |
| open3d-key-calib-n | n |
| open3d-key-calib-m | m |
| open3d-key-calib-z | z |
| open3d-key-calib-x | x |
| open3d-key-calib-i | i |
| open3d-key-calib-k | k |
| open3d-key-calib-o | o |
| open3d-key-calib-l | l |

### VLC Compiled-In Default Key Bindings

| Key | VLC Action |
|-----|-----------|
| Space | play-pause |
| a | aspect-ratio |
| b | audio-track |
| c | crop |
| d | deinterlace |
| e | frame-next |
| f | toggle-fullscreen |
| g | subdelay-down |
| h | subdelay-up |
| i | intf-show |
| j | audiodelay-down |
| k | audiodelay-up |
| l | loop |
| m | vol-mute |
| n | next |
| o | toggle-autoscale |
| p | prev |
| q | *(no VLC default)* |
| r | random |
| s | stop |
| t | position |
| v | subtitle-track |
| w | wallpaper |
| x | program-sid-next |
| z | zoom |
| Ctrl+q | quit |
| Ctrl+w | clear-playlist |
| Ctrl+Up | vol-up |
| Ctrl+Down | vol-down |
| Alt+1 | zoom-quarter |
| Alt+2 | zoom-half |
| Alt+3 | zoom-original |
| Alt+4 | zoom-double |
| Alt+f | crop-right |
| Alt+r | crop-top |
| Alt+d | crop-left |
| Alt+c | crop-bottom |
| Shift+a | audiodevice-cycle |
| Shift+n | chapter-next |
| Shift+p | chapter-prev |

### Conflict Matrix

| Calib Key | open3d Display Action | VLC Default Action | Conflict? |
|-----------|----------------------|-------------------|-----------|
| `g` | calib help toggle | subdelay-down | ✅ YES |
| `t` | calib drive toggle | position | ✅ YES |
| `b` | calib emitter save | audio-track | ✅ YES |
| `p` | calib optlog toggle | prev (next track) | ✅ YES |
| `w` | calib Y adjust | wallpaper | ✅ YES |
| `a` | calib X adjust | aspect-ratio | ✅ YES |
| `d` | calib X adjust | deinterlace | ✅ YES |
| `q` | calib spacing down | *(none)* | ❌ No |
| `e` | calib spacing up | frame-next | ✅ YES |
| `n` | calib border down | next (next track) | ✅ YES |
| `m` | calib border up | vol-mute | ✅ YES |
| `z` | calib size down | zoom | ✅ YES |
| `x` | calib size up | program-sid-next | ✅ YES |
| `i` | calib IR frame delay | intf-show | ✅ YES |
| `k` | calib IR frame delay up | audiodelay-up | ✅ YES |
| `o` | calib IR frame duration | toggle-autoscale | ✅ YES |
| `l` | calib IR frame duration up | loop | ✅ YES |
| `r` | emitter-read (lowercase; not a calib key) | random | ✅ YES |
| `s` | calib Y adjust | stop | ✅ YES |
| `f` | *(no Open3D binding; settings use Ctrl+Shift+F7)* | toggle-fullscreen | ❌ No |
| `j` | emitter-reconnect (lowercase) | audiodelay-down | ✅ YES |

### Modifier Combination Conflicts

Open3D single-letter bindings preserve Ctrl and Alt modifiers, so `Ctrl+q`,
`Ctrl+w`, `Alt+r`, `Alt+d`, `Alt+c`, and `Alt+f` do not match the bare Open3D
calibration keys and should remain available to VLC. Shift is intentionally
treated as the larger-step calibration modifier, so VLC defaults on
`Shift+a/b/d/k/m/n/o/p/s` conflict while calibration is active.

## Non-Conflicting Keys (Safe)

### Modifier Keys
- `Ctrl+Shift+F7/8/9/10/11/12` — open3d toggle keys
- `Ctrl+Alt+G/T/B/P/W/S/A/D/Q/E/N/M/Z/X/I/K/O/L` — no longer used by Open3D defaults

### Case-Sensitivity
- Lowercase emitter keys (r, y, u, j): r conflicts with VLC random, j conflicts with VLC audiodelay-down, y and u are free
- `Ctrl+Shift+F` combinations have no VLC conflict

### Total Conflicts
- **19 single-letter key conflicts** (17 calib + r, j)
- **9 Shift-modified VLC shortcuts overlap with Open3D larger-step calibration**
- **Total disabled VLC bindings in AppRun: 28**

## Resolution: Command-Line Key Overrides

### Why command-line (not vlcrc)?

The AppRun script sets `OPEN3D_MVC_IGNORE_CONFIG=1` (line 255), which adds `--ignore-config` to VLC's command line (line 337). This completely disables config file reading, making vlcrc-based overrides ineffective.

Command-line arguments to VLC are processed during config parsing, **before** the `--ignore-config` flag takes effect. So `--key-play-pause=Space` overrides the compiled-in default regardless of `--ignore-config`.

### Implementation: Add to AppRun script

Insert the following block after the `--ignore-config` block (around line 338) in `AppRun`:

```bash
# Disable open3d_display-calib-key conflicts with VLC defaults.
# These --key-* overrides are processed during config parsing before
# --ignore-config takes effect, so they work even when config files
# are completely ignored.
vlc_args+=(
  "--key-subdelay-down="       # g: subdelay-down <-> calib help toggle
  "--key-position="            # t: position <-> calib drive toggle
  "--key-audio-track="         # b: audio-track <-> calib emitter save
  "--key-prev="                # p: prev <-> calib optlog toggle
  "--key-wallpaper="           # w: wallpaper <-> calib Y adjust
  "--key-aspect-ratio="        # a: aspect-ratio <-> calib X adjust
  "--key-deinterlace="         # d: deinterlace <-> calib X adjust
  "--key-frame-next="          # e: frame-next <-> calib spacing up
  "--key-next="                # n: next <-> calib border down
  "--key-vol-mute="            # m: vol-mute <-> calib border up
  "--key-zoom="                # z: zoom <-> calib size down
  "--key-program-sid-next="    # x: program-sid-next <-> calib size up
  "--key-intf-show="           # i: intf-show <-> calib IR frame delay
  "--key-audiodelay-up="       # k: audiodelay-up <-> calib IR frame delay up
  "--key-toggle-autoscale="    # o: toggle-autoscale <-> calib IR frame duration
  "--key-loop="                # l: loop <-> calib IR frame duration up
  "--key-random="              # r: frees VLC random for emitter-read
  "--key-stop="                # s: stop <-> calib Y adjust
  "--key-audiodelay-down="     # j: frees VLC audiodelay-down for emitter-reconnect
  "--key-title-next="          # Shift+b: title-next <-> large-step calib-b
  "--key-deinterlace-mode="    # Shift+d: deinterlace-mode <-> large-step calib-d
  "--key-snapshot="            # Shift+s: snapshot <-> large-step calib-s
  "--key-subsync-apply="       # Shift+k: subsync-apply <-> large-step calib-k
  "--key-disc-menu="           # Shift+m: disc-menu <-> large-step calib-m
  "--key-chapter-next="        # Shift+n: chapter-next <-> large-step calib-n
  "--key-title-prev="          # Shift+o: title-prev <-> large-step calib-o
  "--key-chapter-prev="        # Shift+p: chapter-prev <-> large-step calib-p
  "--key-audiodevice-cycle="   # Shift+a: audiodevice-cycle <-> large-step calib-a
)
```

### Why This Works

1. VLC processes `--key-<action>=<value>` arguments during config parsing
2. Config parsing happens **before** the `--ignore-config` flag is applied
3. Empty values (`--key-<action>=`) disable the compiled-in default
4. The hotkeys module still receives `key-pressed` events but takes no action for these keys
5. open3d_display's `Open3DHotkeyVarCallback` still processes them normally
6. No VLC source code changes and no runtime vlcrc dependency; the AppImage
   must still be rebuilt to include AppRun changes

### Verification

After applying, verify by:
1. Launching the AppImage
2. Pressing calib keys (g, t, b, p, w, a, d, e, n, m, z, x, i, k, o, l, s) and emitter keys (r, y, u, j)
3. Verifying NO unintended VLC actions occur (no deinterlace toggle, no volume mute, etc.)
4. Verifying open3d_display calibration controls respond correctly

## Edge Cases & Considerations

1. **User's existing vlcrc** — Already customized to use Ctrl+Alt/Shift combinations. Not used at runtime due to `--ignore-config`, but user may rely on it for non-AppImage VLC installations.

2. **Non-AppImage VLC installations** — Users who build/install VLC from source or package managers will NOT have these overrides. They need to add the `--key-*=` arguments manually or customize their vlcrc.

3. **Wayland support** — VLC's config system is display-server-independent. Key overrides work the same on Wayland.

4. **`q` key** — Single `q` has NO VLC default (not listed in s_names2actions), so calib `q` doesn't conflict.

5. **`h`, `j`, `f`, and `q` keys** — `h` = subdelay-up in VLC but is not used by Open3D, so it should not be cleared. `j` IS used as emitter-reconnect and conflicts with VLC's audiodelay-down. `f` is VLC fullscreen and is not used by Open3D, so it should remain available. `q` has NO VLC default as a single key, so calib `q` does not conflict.

6. **Future calib key additions** — Any new single-letter calib key should be checked against the VLC default table above before being added.

## Files to Modify

1. **`packaging/appimage/AppRun`** — Add `--key-*=` overrides in `vlc_args` (around line 338)
2. **`vlc_hotkey_conflict_analysis.md`** — This analysis (created)

## Files Created (Reference Only)

1. **`vlc_hotkey_conflict_analysis.md`** — This analysis document
2. **`packaging/appimage/vlcrc-open3d-defaults`** — Reference vlcrc (not used at runtime due to `--ignore-config`, but useful for documentation and non-AppImage builds)
