# Open3D VLC Usability and Settings Plan

## Purpose

Bring the VLC AppImage path closer to the practical usability of MPCVideoRenderer and
3DPlayer for normal 3D playback. This plan intentionally prioritizes non-menu playback
and calibration usability over the Blu-ray menu defect backlog.

## Current Findings

- The VLC AppImage launcher runs VLC with `--ignore-config`, so standard VLC
  Preferences are not a reliable persistence mechanism for Open3D defaults.
- The AppImage isolates XDG state under `open3doled-vlc-appimage`, so Open3D-owned
  settings files should live there instead of depending on VLC's global profile.
- `open3d_display.c` already exposes most low-level runtime options:
  Open3D enable, layout, eye swap, target flip rate, presenter cadence, trigger box
  geometry, calibration overlay, hotkeys, serial emitter connection, emitter read/apply,
  EEPROM save, firmware helper, and emitter JSON backup/update support.
- The current low-level options are technically present but not usable as a product UI.
  They are spread across VLC Preferences and command-line flags, and many values are
  only loaded at vout creation.
- MPCVideoRenderer has the best separation of concerns:
  display settings are saved locally and applied to the active renderer; emitter
  connection preferences are local; full emitter tuning is applied/read/saved on the
  emitter, with JSON backup/restore available as an explicit workflow.
- MPCVideoRenderer is also the best native-code timing reference for fixed-rate
  page flipping: `target_framerate` is stored as `PageFlipConfig.rateHz`, resolves to
  the display refresh rate when zero, and drives a dedicated high-priority pageflip
  thread.
- 3DPlayer confirms the same split and additionally has explicit display profiles,
  target-framerate timer flipping, and optional BFI-frame insertion concepts.

## Settings Ownership Model

### Display Settings: Local, Auto-Persisted

These should be loaded from a default active profile at VLC startup and autosaved when
changed by the GUI or calibration hotkeys:

- `open3d-enable`
- `open3d-layout`
- `open3d-default-half-layout`
- `open3d-flip-eyes`
- `open3d-target-flip-hz`
- `open3d-presenter-hz`
- `open3d-trigger-enable`
- `open3d-trigger-size`
- `open3d-trigger-padding`
- `open3d-trigger-spacing`
- `open3d-trigger-corner`
- `open3d-trigger-offset-x`
- `open3d-trigger-offset-y`
- `open3d-trigger-alpha`
- `open3d-trigger-brightness`
- `open3d-trigger-black-border`
- `open3d-trigger-invert`
- `open3d-calibration-enable`
- `open3d-status-osd-enable`
- `open3d-status-osd-duration-ms`
- `open3d-status-help-duration-ms`
- non-debug subtitle/interactive graphics depth defaults where useful for MKV playback

Default path:

`$XDG_CONFIG_HOME/open3doled/vlc/open3d_display_settings.json`

With the current AppImage defaults this resolves to:

`~/.config/open3doled-vlc-appimage/open3doled/vlc/open3d_display_settings.json`

### Local Emitter Settings: Local, Auto-Persisted

These are host/workstation preferences, not device tuning:

- emitter enabled
- tty path or auto-detect
- baud rate
- auto reconnect
- reconnect interval
- optional firmware helper path
- optional firmware hex path
- optional backup JSON path

Default path:

`$XDG_CONFIG_HOME/open3doled/vlc/local_emitter_connection.json`

### Emitter Tuning Settings: Device-Owned

These should remain device-owned. The GUI should read/apply/save EEPROM explicitly, and
backup/restore JSON should be an explicit user action:

- IR protocol
- IR frame delay
- IR frame duration
- IR signal spacing
- optical block delay
- optical thresholds
- duplicate/ignore/filter settings
- IR eye flip
- average timing mode
- emitter target frametime
- emitter drive mode

The existing VLC emitter settings JSON is useful for desired/device/dirty state and
firmware backup, but it should not be treated as the primary persistence source for
normal emitter tuning unless the user explicitly loads/restores it.

## Recommended Architecture

### 1. Open3D Settings Core

Add a small settings layer for `open3d_display.c`:

- read Open3D display JSON during vout `Open()`
- merge built-in defaults, saved Open3D settings, and explicit user CLI values using
  the precedence below
- write display JSON atomically after runtime changes
- keep schema versioned and tolerant of missing keys
- avoid depending on VLC's `vlcrc`

The implementation can start inside `open3d_display.c` to limit build churn, then move
to `open3d_settings.c/.h` once stable.

Precedence rule:

- explicit user CLI flags should win
- saved Open3D settings should win over launcher-injected defaults
- built-in defaults should only apply when no saved setting exists

This matters because AppRun currently injects Open3D defaults such as emitter enable and
auto tty. Those launcher defaults should eventually move into the Open3D settings layer
or only be passed when no saved settings file exists; otherwise saved user preferences
cannot reliably override them.

AppRun already has a user-argument scanner for options such as vout, demux, codec,
subtitle routing, presenter RT, and Blu-ray menu flags. Extend that scanner for
persisted Open3D settings rather than inventing a second precedence mechanism.

Initial display JSON schema direction:

- use a flat schema for the first implementation so the C parser stays simple
- use the VLC option names as JSON keys where practical, for example
  `"open3d-trigger-offset-x": 12`
- store numbers as JSON numbers, booleans as JSON booleans, and enum values as strings
- include `"schema": 1` and tolerate missing keys
- write atomically with `*.tmp` and `rename()`, matching the existing emitter JSON
  pattern

Initial local emitter connection JSON schema direction:

- keep it separate from display settings and emitter tuning
- store only host-local connection values such as enabled, tty, baud, auto reconnect,
  reconnect interval, firmware helper path, firmware hex path, and backup path
- do not store device tuning as local defaults

### 2. Runtime Apply API

Create one internal runtime apply path that both hotkeys and UI commands use:

- update in-memory `vout_display_sys_t`
- publish presenter state when eye/layout/timing changes
- bump overlay state when trigger/calibration/status geometry changes
- queue emitter apply/read/save/reconnect actions when needed
- mark display settings dirty and autosave only display/local-emitter settings

This avoids the current split where some settings are inherited at startup, some are
callbacks, and hotkeys directly mutate fields.

### 3. Control Surface

Prefer a dedicated Open3D GUI over VLC Preferences.

Recommended first implementation path:

- first add a small command/control client for validation and scripting
- then add an external companion GUI bundled in the AppImage
- communicates with the running Open3D vout through a Unix-domain socket under
  `$XDG_RUNTIME_DIR` or a fallback path under `$XDG_STATE_HOME/open3doled/vlc`
- command protocol can be line-delimited JSON:
  `get_state`, `set_display`, `set_local_emitter`, `read_emitter`,
  `apply_emitter`, `save_emitter_eeprom`, `load_display_profile`,
  `save_display_profile`

Reasons:

- VLC Qt preference pages are a poor fit for custom low-latency controls.
- An external GUI can be improved visually later without risking the video output path.
- The vout still owns the low-latency runtime state and emitter queue.
- A command/control client gives automated validation before the full GUI exists.

Packaging constraint:

- The AppImage already carries Qt5 libraries and plugins for VLC's Qt interface, so a
  small C++/Qt companion GUI is the most plausible packaged GUI path.
- Python/Tk is useful as a development prototype but should not be assumed present in
  the AppImage runtime unless explicitly bundled.
- A small native `open3dctl`-style socket client should come before the GUI so tests and
  scripts do not depend on GUI automation.

IPC constraint:

- Use a Unix-domain socket, not TCP.
- Create it with owner-only permissions under `$XDG_RUNTIME_DIR` when available.
- Include pid/instance information in the socket or registry path so multiple VLC
  instances do not collide.
- Mutate renderer state on the Open3D control thread, not directly from an IPC thread.
  This keeps GUI commands aligned with hotkey behavior and avoids cross-thread renderer
  state races.

### 4. Keyboard Calibration Compatibility

Preserve focused-window hotkeys for calibration:

- `Ctrl+Shift+F8`: Open3D on/off
- `Ctrl+Shift+F9`: status OSD
- `Ctrl+Shift+F10`: calibration mode
- `Ctrl+Shift+F12`: flip eyes
- calibration movement keys for trigger box offsets, size, spacing, border
- emitter timing hotkeys for IR frame delay/duration
- emitter read/apply/save/reconnect hotkeys

After this refactor, hotkeys should call the same runtime apply/autosave path used by
the GUI.

### 5. 240 Hz / Frame-Doubling Audit

VLC already has `open3d-target-flip-hz` and `open3d-presenter-hz`.
MPCVideoRenderer should be the primary reference for this path because it already
implements fixed-rate page flipping in the renderer. 3DPlayer should remain the
secondary reference for elapsed-time eye selection and the optional BFI-frame behavior.

The required usability target is:

- on a 240 Hz monitor, allow a target eye flip cadence such as 120 Hz
- use a timer-based eye selection path when target flip rate is set
- keep optical trigger boxes synchronized with that timer cadence
- clearly show active cadence in the OSD and GUI

Initial work should verify current behavior before adding new modes. The comparison
points are:

- MPCVideoRenderer: `PageFlipConfig.rateHz`, `UpdatePageFlipRate()`, and the
  high-priority pageflip thread that queues emitter signals and renders on cadence.
- 3DPlayer: `target_framerate` chooses the eye from elapsed time, with optional BFI
  insertion when configured.

If VLC's existing target-rate path is insufficient, port the MPCVideoRenderer cadence
model first and only add the 3DPlayer BFI behavior later if it is still needed:

- target framerate > 0 computes a fixed presentation period from configured Hz
- target framerate of 0 resolves to the active display/presenter cadence
- eye state, optical trigger boxes, emitter signals, and rendering advance on the
  same cadence
- optional BFI insertion is a separate setting and can remain future work

## Implementation Order

### Phase 1: Persistence Foundation

- Add Open3D display settings JSON load/save.
- Load JSON during vout creation.
- Autosave display settings changed by hotkeys.
- Adjust AppRun/plugin precedence so saved settings are not overwritten by launcher
  defaults.
- Keep AppRun `--ignore-config`.
- Validate by changing trigger position, closing VLC, relaunching, and verifying the
  trigger position returns.

### Phase 2: Runtime Apply Consolidation and Control API

- Replace direct hotkey field mutation with a shared apply function.
- Add dirty tracking for display settings.
- Keep emitter tuning dirty tracking separate from display dirty tracking.
- Add the Unix-domain socket control endpoint.
- Add a minimal `open3dctl` client for `get_state`, display `set`, and emitter actions.
- Validate hotkeys still work during playback and changes persist.

### Phase 3: Minimal Companion GUI

- Add a small functional GUI for display settings and local emitter connection.
- GUI can start plain; usability is more important than visual polish initially.
- Support connect/disconnect/read/apply/save EEPROM actions.
- Show current emitter dirty state and connected tty.

### Phase 4: Profile Management

- Keep one default active profile.
- Add load/save display profile buttons.
- Store profile files as `*.display_settings.json`.
- Do not automatically overwrite arbitrary profile files unless explicitly saved.

### Phase 5: 240 Hz Target-Rate Validation

- Add GUI fields for target flip Hz and presenter Hz.
- Validate 120 Hz target flipping on a 240 Hz display.
- If needed, adjust the presenter/eye-advance path to avoid present-limited toggling.

### Phase 6: Polish and Packaging

- Add AppRun launcher option/menu entry for Open3D settings.
- Bundle the GUI in the AppImage.
- Add a smoke script that confirms JSON persistence and GUI command reachability.

## Current Non-Goals

- Do not resume Blu-ray menu defect fixing as part of this plan.
- Do not add firmware update UI unless the existing helper path needs basic exposure.
- Do not make emitter tuning silently persist locally as the source of truth.
- Do not depend on standard VLC Preferences for Open3D usability.

## Implementation Status

### 2026-05-12: Phase 1 Settings Core Implemented

- Added Open3D-owned display settings persistence in `open3d_display.c`.
- Added local emitter connection persistence separate from device-owned emitter tuning.
- Added bool/string/double JSON helpers and atomic writes for the new settings files.
- Added startup load/save for:
  - `$XDG_CONFIG_HOME/open3doled/vlc/open3d_display_settings.json`
  - `$XDG_CONFIG_HOME/open3doled/vlc/local_emitter_connection.json`
- AppImage-isolated default paths resolve under:
  - `~/.config/open3doled-vlc-appimage/open3doled/vlc/`
- Calibration hotkeys now autosave changed display settings such as Open3D enable,
  eye swap, calibration enable, trigger size, trigger spacing, trigger offsets, and
  trigger black border.
- Expanded live VLC variable callbacks for most display settings and local emitter
  connection settings, so preference/control changes are no longer limited to the
  previous small subset.
- Changed AppRun so it no longer injects `--open3d-enable`,
  `--open3d-emitter-enable`, or `--open3d-emitter-tty=auto`; the Open3D module owns
  those defaults and persisted values now.
- Added AppRun tracking for explicit user `--open3d-*` / `--no-open3d-*` arguments so
  saved JSON does not override explicitly supplied AppImage command-line settings.

Validation:

- Official Docker/AppImage build:
  `./scripts/open3d_appimage_docker.sh exec -- ./scripts/build_open3d_appimage_artifact.sh`
- AppImage artifact:
  `local/out/appimage/appimage-builds/20260512_143818/Open3DOLED-VLC-3.0.23-x86_64.AppImage`
- AppDir artifact:
  `local/out/appimage/appdir-builds/20260512_143818/AppDir`
- Settings smoke:
  `HOME=local/out/appimage/settings-smoke/20260512_234057/home XDG_RUNTIME_DIR=local/out/appimage/settings-smoke/20260512_234057/runtime OPEN3D_PROCESS_CPU_BACKEND=none OPEN3D_APPIMAGE_ISOLATE_STATE=1 xvfb-run -a timeout 25 local/out/appimage/appdir-builds/20260512_143818/AppDir/AppRun --intf dummy --play-and-exit --run-time=1 --no-audio --open3d-trigger-size=31 local/out/appimage/settings-smoke/20260512_234057/smoke.mp4`
- Verified `open3d_display_settings.json` persisted `open3d-trigger-size: 31` and
  `local_emitter_connection.json` persisted the default `auto` tty.

Remaining next slice:

- Add a proper runtime command surface (`open3dctl`/Unix socket or equivalent) so an
  external GUI can change settings without abusing VLC Preferences.
- Add an actual settings GUI around that command surface.
- Harden thread ownership for live emitter connection mutation before exposing tty/baud
  edits as high-frequency GUI controls.

## Runtime Setting Classes

### Live-Safe Display Settings

These should apply immediately during playback through the shared runtime apply path:

- Open3D enabled/disabled
- eye swap
- forced layout and default half-layout
- trigger enable
- trigger size, padding, spacing, corner, offsets, alpha, brightness, black border,
  invert
- calibration enable
- status OSD visibility and durations
- non-debug subtitle/interactive-graphics depth defaults when they can be safely
  republished to the active vout

### Live-With-Recompute Settings

These can be live, but must reset or republish timing state carefully:

- target flip Hz
- presenter Hz
- presenter lead
- GPU overlay enable

Changing these should reset flip deadlines, publish presenter state where needed, wake
the presenter/control threads, and write a status message that clearly reports the new
effective cadence.

### Startup/Tuning Settings

These should not be part of the first live GUI unless there is a specific need:

- GL provider
- real-time scheduling policy
- presenter CPU affinity
- memory locking and prefault size
- deep debug/telemetry/stage profiling options

They can stay as command-line or advanced config options initially.

## Emitter State Rules

- Local connection preferences are local settings.
- Emitter tuning remains device-owned.
- The GUI can show desired, device, and dirty state, but read/apply/save EEPROM must be
  explicit user actions.
- Existing VLC emitter JSON should be treated as a runtime mirror or backup/restore
  aid, not as the normal source of truth for emitter tuning.
- Hotkeys and GUI commands that change emitter timing should update desired state,
  apply to the emitter if connected, mark dirty until EEPROM save/readback confirms,
  and avoid silently saving those values as a new local default.

## Implementation Risks

- AppRun currently injects some Open3D settings as command-line flags. If not adjusted,
  this can defeat saved settings.
- AppRun's existing user-argument scanner can be extended to detect explicit Open3D
  overrides, but it does not currently track the general display/emitter settings that
  need persistence.
- `open3d_display.c` currently has live callbacks for only a small subset of options:
  layout, default half-layout, hotkey profile, calibration enable, and trigger drive
  mode. Most settings are copied once at vout creation.
- The presenter snapshot currently publishes enable, eye swap, and layout only. Timing
  changes need explicit recompute/wake handling.
- The existing code mutates many runtime fields from the control/hotkey path. IPC
  commands should enqueue work to that same control path rather than mutate fields from
  an arbitrary socket thread.
- JSON parsing in `open3d_display.c` is currently minimal and integer-oriented. The
  display settings parser needs bool/string/double support or a deliberately simple
  schema that is easy to parse safely in C.

## Open Questions

- Exact mechanism for distinguishing explicit user Open3D CLI flags from AppRun's
  launcher defaults.
- Whether the first GUI should be a native Qt utility immediately, or whether the first
  packaged step should stop at `open3dctl` plus JSON persistence.
- Whether target-rate changes should apply live in the first implementation slice or
  initially require a clear restart warning while the recompute path is hardened.
- Whether calibration mode itself should persist by default, or reset to off after launch.
  MPCVideoRenderer persists it; defaulting to persistence is consistent but can be
  revisited if annoying.

## Implementation Status Updates

### 2026-05-13: Runtime Socket Handoff Robustness

- Hardened `open3dctl` so auto-discovery retries the newest socket instead of resolving
  once and failing if VLC replaces the Open3D vout during startup or title changes.
- Added `--retries` and `--retry-delay` to `open3dctl` for scripted calibration runs.
- Changed the control panel to start in auto-socket mode by default, keep discovering
  the active newest socket on refresh, and show the currently active socket separately
  from the optional pinned socket entry.
- Kept manual pinning available through `Use Newest` plus a fixed socket entry for
  debugging multiple VLC instances.

Validation:

- `python3 -m py_compile scripts/open3dctl scripts/open3d_control_panel.py`

### 2026-05-13: Display Profile Save/Load

- Added display-profile management to the Open3D control panel.
- Profiles are stored under:
  - `$XDG_CONFIG_HOME/open3doled/vlc/profiles/*.display_settings.json`
  - or `~/.config/open3doled/vlc/profiles/*.display_settings.json`
- Profiles intentionally cover display settings only. Local emitter connection settings
  continue to auto-persist in the active host config, and emitter tuning remains
  device-owned/read-apply-save explicit.
- Profile names are sanitized for filesystem safety, so names such as `Living Room /
  240 Hz` become `Living_Room_240_Hz.display_settings.json`.

Validation:

- `python3 -m py_compile scripts/open3d_control_panel.py`
- `HOME="$(mktemp -d)" python3 - <<'PY' ...` profile helper smoke for profile-name
  normalization, path generation, and profile discovery.

### 2026-05-13: Control Panel Ergonomics and Timing Presets

- Made the Display and Emitter pages scrollable so the full setting set is reachable
  on normal laptop-sized windows.
- Added display preset buttons:
  - `240 Hz panel / 120 Hz flip`: sets presenter cadence to `240` and target eye-flip
    cadence to `120`.
  - `120 Hz presenter / auto flip`: returns to per-presenter-frame eye alternation
    with the existing `120` Hz presenter default.
  - `Calibration OSD`: enables trigger boxes, calibration mode, and status OSD
    together for optical-trigger setup.
- Reused one display-setting queue path for profile loads and presets so both surfaces
  encode booleans and numbers consistently before sending socket commands.

Validation:

- `python3 -m py_compile scripts/open3d_control_panel.py`
- `timeout 5s xvfb-run -a python3 scripts/open3d_control_panel.py` reached the Tk
  main loop and exited by timeout as expected with no startup crash.

### 2026-05-13: Scriptable Profiles and Presets

- Extended `open3dctl` with display-profile commands:
  - `profile-list`
  - `profile-save NAME`
  - `profile-load NAME`
- Added CLI preset commands matching the control-panel buttons:
  - `preset 240hz-120flip`
  - `preset 120hz-auto`
  - `preset calibration-osd`
- Kept CLI profile files compatible with the control panel:
  `$XDG_CONFIG_HOME/open3doled/vlc/profiles/*.display_settings.json`.

Validation:

- `python3 -m py_compile scripts/open3dctl`
- `HOME="$(mktemp -d)" scripts/open3dctl profile-list`
- Python helper smoke for `save_profile`, `list_profiles`, and `load_profile`.

### 2026-05-13: Shared Control Helper Module

- Factored profile and preset helpers into `scripts/open3d_control_common.py`.
- Updated `open3dctl` and the Tk control panel to use the same profile path,
  filename sanitization, JSON schema, boolean command encoding, and preset maps.
- Updated the AppDir builder to install the shared helper next to the packaged CLI
  and control panel so the AppImage/AppDir entrypoints keep working.

Validation:

- `python3 -m py_compile scripts/open3d_control_common.py scripts/open3dctl scripts/open3d_control_panel.py`
- `HOME="$(mktemp -d)" scripts/open3dctl profile-list`
- `bash -n scripts/build_open3d_appimage_appdir.sh`
- `git diff --check`

### 2026-05-13: Control Surface Smoke Harness

- Added `scripts/run_open3d_control_surface_smoke.sh`.
- The harness auto-discovers the newest staged AppDir unless `OPEN3D_APPIMAGE_APPDIR`
  or `OPEN3D_CONTROL_SMOKE_APPRUN` is supplied.
- It validates:
  - packaged `--open3dctl --help`
  - packaged `--open3dctl profile-list`
  - packaged `--open3d-control-panel --help`
  - optional Xvfb startup for the Tk control panel, expecting timeout after the panel
    reaches its main loop.
- It writes logs under `local/out/appimage/control-surface-smoke/<timestamp>/`.

Validation:

- `bash -n scripts/run_open3d_control_surface_smoke.sh`

### 2026-05-13: Settings Launcher Polish

- Added AppRun aliases for the companion control panel:
  - `--open3d-control-panel`
  - `--open3d-settings`
  - `--open3d-controls`
- Added a desktop-file action named `Open3D Settings`, so desktop integration can
  expose the control panel without requiring the user to remember the CLI flag.

Validation:

- `bash -n packaging/appimage/AppRun`
- `desktop-file-validate packaging/appimage/open3doled-vlc.desktop`
- `git diff --check`

### 2026-05-13: Deterministic Control-Apply Verification

- Added `open3dctl --wait-applied` for `set`, `set-display`, `set-emitter`,
  `profile-load`, and `preset`.
- The wait path polls `get_state` until the requested display or local-emitter values
  are visible, which makes scripted calibration/profile operations less dependent on
  arbitrary sleeps after queueing control commands.
- Added `scripts/run_open3dctl_protocol_smoke.py`, a fake Unix-socket protocol smoke
  that validates source `open3dctl` command routing, state discovery, display preset
  application, and emitter state-key mapping without needing a live VLC/OpenGL vout.

Validation:

- `python3 -m py_compile scripts/open3dctl scripts/run_open3dctl_protocol_smoke.py`
- `scripts/run_open3dctl_protocol_smoke.py`

### 2026-05-13: Target-Rate Timing Visibility and Cadence Reset

- Changing `open3d-target-flip-hz` or `open3d-presenter-hz` now resets the internal
  flip deadline and serial-emitter eye dedupe state. This avoids stale phase/deadline
  state after applying the 240 Hz / 120 flip preset at runtime.
- The Open3D OSD now reports the active presenter cadence and target flip cadence, so
  240 Hz / 120 flip testing no longer requires guessing which timing mode is active.
- `get_state` now includes a `timing` object with presenter period, flip period, flip
  mode, current eye, late-flip counters, and presenter deadline-miss counters.
- The companion control panel shows the reported timing summary near the active socket
  line.

Validation:

- `python3 -m py_compile scripts/open3d_control_panel.py`
- Official Docker/AppImage rebuild:
  `./scripts/open3d_appimage_docker.sh exec -- ./scripts/build_open3d_appimage_artifact.sh`
- AppDir artifact:
  `local/out/appimage/appdir-builds/20260512_163035/AppDir`
- AppImage artifact:
  `local/out/appimage/appimage-builds/20260512_163035/Open3DOLED-VLC-3.0.23-x86_64.AppImage`
- Packaged control-surface smoke:
  `OPEN3D_APPIMAGE_APPDIR=local/out/appimage/appdir-builds/20260512_163035/AppDir ./scripts/run_open3d_control_surface_smoke.sh`
- `scripts/run_open3dctl_protocol_smoke.py`

### 2026-05-13: Packaged open3dctl Protocol Smoke

- Extended `scripts/run_open3dctl_protocol_smoke.py` with `--client-arg`, allowing the
  same fake-socket protocol test to exercise `AppRun --open3dctl`.
- Extended `scripts/run_open3d_control_surface_smoke.sh` to run that protocol smoke
  against the packaged AppDir entrypoint, so future AppImage/AppDir builds verify
  wait-applied routing through the real launcher rather than source scripts only.
- Expanded the fake-socket protocol smoke to cover display profile save/load with
  `--wait-applied`, including profile-name normalization and restoration of the
  240 Hz / 120 flip preset values.

Validation:

- `python3 -m py_compile scripts/run_open3dctl_protocol_smoke.py`
- `bash -n scripts/run_open3d_control_surface_smoke.sh`
- `scripts/run_open3dctl_protocol_smoke.py --client local/out/appimage/appdir-builds/20260512_163035/AppDir/AppRun --client-arg=--open3dctl`
- `OPEN3D_APPIMAGE_APPDIR=local/out/appimage/appdir-builds/20260512_163035/AppDir ./scripts/run_open3d_control_surface_smoke.sh`

### 2026-05-13: Legacy Display Profile Import

- Added shared import helpers for 3DPlayer and MPCVideoRenderer
  `*.display_settings.json` files.
- Added `open3dctl profile-import PATH` to save those legacy display settings as
  Open3D VLC display profiles.
- Supported deterministic frame-doubling imports through
  `open3dctl profile-import PATH --presenter-hz 240`, avoiding filename-based refresh
  inference.
- Supported `--apply` plus global `--wait-applied` so imported profiles can be saved
  and immediately verified against the running Open3D vout.
- Mapped legacy target/pageflip/whitebox/blackbox/calibration fields to Open3D VLC
  display keys while intentionally leaving emitter tuning and unsupported display-size
  metadata out of display profiles.

Validation:

- `python3 -m py_compile scripts/open3d_control_common.py scripts/open3dctl scripts/run_open3dctl_protocol_smoke.py`
- `scripts/run_open3dctl_protocol_smoke.py`

### 2026-05-13: GUI Legacy Profile Import

- Added an `Import Legacy` button to the Open3D control panel profile row.
- The GUI importer uses the same shared 3DPlayer/MPCVideoRenderer mapping as
  `open3dctl profile-import`.
- Added an optional `Import presenter Hz` field for deterministic 240 Hz panel /
  120 Hz flip-profile migration without inferring refresh from filenames.
- Imported profiles are saved under the normal Open3D VLC profile directory and the
  profile selector is moved to the imported profile name; the existing `Load` button
  remains the explicit apply step.

Validation:

- `python3 -m py_compile scripts/open3d_control_panel.py scripts/open3d_control_common.py`
- `timeout 5s xvfb-run -a python3 scripts/open3d_control_panel.py`

### 2026-05-13: Settings Path Discovery

- Added `open3dctl paths`.
- The command reports the active Open3D VLC config home, display settings JSON path,
  local emitter connection JSON path, display profile directory, and candidate state
  directories for control sockets.
- Added packaged smoke coverage for `AppRun --open3dctl paths` so future AppDir builds
  verify path discovery through the real launcher.

Validation:

- `python3 -m py_compile scripts/open3d_control_common.py scripts/open3dctl`
- `bash -n scripts/run_open3d_control_surface_smoke.sh`
- `HOME=/tmp/open3dctl-paths-smoke scripts/open3dctl paths`

### 2026-05-13: Packaged Usability Rebuild

- Rebuilt the official AppDir/AppImage after the legacy import, GUI import, and path
  discovery slices.
- New AppDir:
  `local/out/appimage/appdir-builds/20260512_164921/AppDir`
- New AppImage:
  `local/out/appimage/appimage-builds/20260512_164921/Open3DOLED-VLC-3.0.23-x86_64.AppImage`

Validation:

- Official Docker/AppImage rebuild:
  `./scripts/open3d_appimage_docker.sh exec -- ./scripts/build_open3d_appimage_artifact.sh`
- Packaged control-surface smoke:
  `OPEN3D_APPIMAGE_APPDIR=local/out/appimage/appdir-builds/20260512_164921/AppDir ./scripts/run_open3d_control_surface_smoke.sh`

### 2026-05-13: In-Player Settings Launch Hotkey

- Added `Ctrl+Shift+F7` as the default Open3D settings/control-panel launcher hotkey.
- Added the settings hotkey to both CPU and GPU Open3D OSD hotkey summaries.
- Updated the status/help message to direct users toward the settings panel for
  detailed controls.
- The AppImage launcher now exports:
  - `OPEN3D_CONTROL_PANEL=$APPDIR/usr/bin/open3d-control-panel`
  - `OPEN3D_APPIMAGE_APPRUN=$APPDIR/AppRun`
- The vout launcher prefers the direct control-panel path, falls back to
  `AppRun --open3d-settings`, then falls back to `open3d-control-panel` on `PATH` for
  development runs.

Validation:

- `bash -n packaging/appimage/AppRun`
- `git diff --check`

### 2026-05-13: Native VLC Preferences Cleanup

- Marked Open3D-specific video-output module options private with VLC
  `change_private()`.
- This keeps the option names usable for CLI/AppRun/runtime/config plumbing while
  hiding the large low-level Open3D option set from VLC Preferences/help.
- The native VLC output module selection remains available; the intended user-facing
  settings surface is now the standalone Open3D control panel launched from
  `Ctrl+Shift+F7`, `AppRun --open3d-settings`, or the desktop `Open3D Settings`
  action.

Validation:

- `git diff --check`
- `scripts/run_open3dctl_protocol_smoke.py`

### 2026-05-13: Settings Hotkey / Preferences Cleanup Rebuild

- Rebuilt the official AppDir/AppImage after the in-player settings hotkey and
  native VLC preferences cleanup slices.
- New AppDir:
  `local/out/appimage/appdir-builds/20260512_213022/AppDir`
- New AppImage:
  `local/out/appimage/appimage-builds/20260512_213022/Open3DOLED-VLC-3.0.23-x86_64.AppImage`

Validation:

- Official Docker/AppImage rebuild:
  `./scripts/open3d_appimage_docker.sh exec -- ./scripts/build_open3d_appimage_artifact.sh`
- Packaged control-surface smoke:
  `OPEN3D_APPIMAGE_APPDIR=local/out/appimage/appdir-builds/20260512_213022/AppDir ./scripts/run_open3d_control_surface_smoke.sh`

### 2026-05-13: Presenter OSD Startup Regression Fix

- Investigated the reported 120 Hz presenter regression after the settings/control
  panel work.
- Found that the current persisted display settings contained
  `open3d-calibration-enable=true`, and the vout also defaulted the persistent status
  overlay visible. That combination can force text/status OSD generation and GPU
  overlay drawing through the presenter path during normal playback.
- Changed normal startup so the persistent status overlay is off by default; hotkey
  messages and `Ctrl+Shift+F11` still show transient OSD, and `Ctrl+Shift+F9` still
  enables the persistent status overlay when explicitly requested.
- Made calibration mode runtime-only for display persistence and display profiles.
  Trigger geometry, timing, layout, and other display settings still persist, but
  startup no longer inherits an old calibration-mode flag from JSON/profile state.
- Changed entering calibration mode to show a short help hint instead of turning the
  full calibration help overlay on by default. `Ctrl+Alt+G` remains the explicit
  calibration-help toggle.
- New AppDir:
  `local/out/appimage/appdir-builds/20260512_214026/AppDir`
- New AppImage:
  `local/out/appimage/appimage-builds/20260512_214026/Open3DOLED-VLC-3.0.23-x86_64.AppImage`

Validation:

- `python3 -m py_compile scripts/open3d_control_common.py scripts/open3d_control_panel.py scripts/open3dctl scripts/run_open3dctl_protocol_smoke.py`
- `scripts/run_open3dctl_protocol_smoke.py`
- `git diff --check`
- Official Docker/AppImage rebuild:
  `./scripts/open3d_appimage_docker.sh exec -- ./scripts/build_open3d_appimage_artifact.sh`
- Packaged control-surface smoke:
  `OPEN3D_APPIMAGE_APPDIR=local/out/appimage/appdir-builds/20260512_214026/AppDir ./scripts/run_open3d_control_surface_smoke.sh`
