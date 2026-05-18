# KMS Direct Playback Future Path

Date: 2026-05-18

Status: deferred. This is a future development line, not current active work.

## Purpose

Open3D VLC playback currently has three practical Linux display paths:

- X11/OpenGL playback, which is still the most reliable high-refresh software page-flipping path.
- Native Wayland/OpenGL playback, which works but can still suffer physical duplicate frames because presentation is mediated by the compositor.
- Standalone Wayland scanout probes, which help characterize compositor behavior but do not provide guaranteed display ownership.

KMS direct playback is the future path for deterministic fullscreen timing when normal desktop compositors cannot provide stable physical frame cadence.

## Terminology

KMS means Kernel Mode Setting through Linux DRM devices such as `/dev/dri/card0`. This is the display ownership path used by compositors, display managers, and gamescope-style embedded sessions.

DKMS is unrelated. DKMS packages kernel modules. Open3D does not currently need an Open3D-specific kernel module.

## Why This Is Not AppImage-Only

An AppImage can bundle userspace playback code, helper binaries, UI code, and some libraries. It cannot bundle or replace the host machine's active display ownership model.

KMS direct playback needs cooperation from the host OS because the playback process needs access to restricted devices and, for the selected display, must avoid racing the desktop compositor for DRM master/display ownership.

Host-owned requirements include:

- GPU kernel driver and KMS support, for example `amdgpu`, `i915`, `nouveau`, or `nvidia-drm`.
- DRM device nodes such as `/dev/dri/card*` and `/dev/dri/renderD*`.
- A local user session managed by `systemd-logind`, `elogind`, or `seatd/libseat`.
- Session/device handoff for DRM and, if needed, input devices under `/dev/input/event*`.
- Optional realtime scheduling privileges such as `rtprio`, `memlock`, or `CAP_SYS_NICE` for best presenter timing.

## Expected User-Visible Models

### Developer/Test Model

The simplest future test model is:

1. Switch to a TTY.
2. Run an Open3D KMS playback command from that local session.
3. Let the KMS backend take the display directly.
4. Exit playback to return to the normal desktop session.

This is the least polished path but is likely the most reliable first validation target.

### Optional Installed Session Model

A more user-friendly future model is a small host integration helper, for example:

- `install-open3d-kms-session`
- an Open3D playback `.desktop` session entry
- a wrapper that launches the AppImage in a dedicated KMS-capable session
- preflight checks for DRM devices, logind/libseat access, Vulkan/GBM/EGL support, and realtime limits

The AppImage would still carry most Open3D code. The session registration and host permission checks would live outside the AppImage because they are host policy.

### GNOME Desktop Model

Running inside the active GNOME Wayland desktop should not be treated as a true KMS direct mode. GNOME/Mutter owns the active monitor. A normal application window can request fullscreen and may sometimes get direct scanout, but that is opportunistic and not equivalent to display ownership.

### DRM Lease Model

DRM leasing may become useful for a separate display that is not the active desktop monitor. It should not be assumed to solve taking over the same monitor that GNOME is actively using.

## What Users Should Not Need

Users should not need:

- an Open3D kernel module
- an Open3D DKMS package
- a custom kernel in normal cases
- special Open3D GPU drivers
- emitter firmware changes specifically for KMS playback

They may still need normal distro GPU drivers and firmware that make KMS work on their system.

## Future Architecture Direction

The current code has started moving toward explicit display backend policy in `open3d_display.c`. That is the right direction for KMS.

Long-term backend separation should keep these concerns shared:

- stereo decode/layout decisions
- prepared left/right surface generation
- trigger box and OSD snapshot generation
- emitter settings and control
- JSON settings import/export
- general telemetry structures

Backend-specific code should own:

- window or display acquisition
- display timing model
- present/flip mechanism
- physical presentation feedback
- refresh-mode selection
- input/session lifecycle
- failure diagnostics

Likely backend names:

- `OPEN3D_PRESENT_BACKEND_X11_GL`
- `OPEN3D_PRESENT_BACKEND_WAYLAND_GL`
- `OPEN3D_PRESENT_BACKEND_KMS_DIRECT`
- optionally later `OPEN3D_PRESENT_BACKEND_DRM_LEASE`

## First Future Implementation Slice

When this line of work becomes active, the first useful implementation should be a small standalone KMS probe, not the full VLC path.

Minimum probe behavior:

- open a selected DRM card/connector through logind/libseat or a controlled test path
- select a target mode such as true 240 Hz or the reported native fractional mode
- alternate red/blue fullscreen buffers with Open3D trigger boxes
- optionally drive BFI patterns
- log atomic commit/vblank timing
- optionally pair with optical sensor capture
- exit cleanly and restore the previous mode/session state

Success criterion:

- optical capture shows stable physical cadence with no recurring duplicate frame pattern under the same monitor/emitter configuration that flickers under normal Wayland playback.

Only after that probe is stable should the KMS backend be integrated into the VLC/Open3D presenter path.

## Risks And Open Questions

- Taking the active desktop monitor away from GNOME is not expected to be seamless without a separate session/VT model.
- Input handling may need logind/libseat integration rather than raw `/dev/input` access.
- NVIDIA behavior may differ from Mesa/GBM systems.
- Vulkan direct-display support is not enough if the desktop compositor owns the active display.
- A polished user experience may require a small installed session helper even if the main player remains an AppImage.
- The exact host integration should be validated before promising this as an end-user feature.

## Current Decision

Do not implement KMS direct playback now.

Keep the topic documented as the likely future route for deterministic fullscreen timing if X11 becomes unacceptable or unsupported and normal Wayland remains too jittery for optical 3D page flipping.

