#!/usr/bin/env python3
"""Functional Open3D VLC runtime control panel."""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import socket
import subprocess
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk
from typing import Any

from open3d_control_common import (
    OPEN3D_DISPLAY_3DPLAYER_KEYS,
    OPEN3D_EMITTER_3DPLAYER_KEYS,
    PROFILE_SCHEMA,
    command_value,
    import_legacy_display_settings,
    legacy_profile_default_name,
    list_profiles,
    load_3dplayer_display_settings,
    load_3dplayer_emitter_settings,
    load_display_profile,
    normalize_profile_name,
    parse_bool,
    profile_path,
    save_display_profile,
    write_3dplayer_display_settings,
    write_3dplayer_emitter_settings,
)


DISPLAY_FIELDS = [
    ("open3d-enable", "Open3D enabled", "bool", None),
    ("open3d-layout", "Layout", "choice", ["auto", "sbs", "tb", "sbs-full", "sbs-half", "tb-full", "tb-half"]),
    ("open3d-default-half-layout", "Default half layout", "choice", ["sbs", "tb"]),
    ("open3d-flip-eyes", "Swap eyes", "bool", None),
    ("open3d-target-flip-hz", "Target flip Hz", "float", None),
    ("open3d-presenter-hz", "Presenter Hz", "float", None),
    ("open3d-gpu-overlay-enable", "GPU overlay", "bool", None),
    ("open3d-bfi-enable", "Black-frame insertion", "bool", None),
    ("open3d-bfi-visible-frames", "BFI visible frames", "int", None),
    ("open3d-bfi-black-frames", "BFI black frames", "int", None),
    ("open3d-trigger-enable", "Trigger boxes", "bool", None),
    ("open3d-trigger-size", "Trigger size", "int", None),
    ("open3d-trigger-padding", "Trigger padding", "int", None),
    ("open3d-trigger-spacing", "Trigger spacing", "int", None),
    ("open3d-trigger-corner", "Trigger corner", "choice", ["top-left", "top-right", "bottom-left", "bottom-right"]),
    ("open3d-trigger-offset-x", "Trigger X offset", "int", None),
    ("open3d-trigger-offset-y", "Trigger Y offset", "int", None),
    ("open3d-trigger-alpha", "Trigger alpha", "float", None),
    ("open3d-trigger-brightness", "Trigger brightness", "int", None),
    ("open3d-trigger-black-border", "Black border", "int", None),
    ("open3d-trigger-invert", "Invert trigger", "bool", None),
    ("open3d-calibration-enable", "Calibration mode (runtime)", "bool", None),
    ("open3d-status-osd-enable", "Status OSD", "bool", None),
    ("open3d-status-osd-duration-ms", "Status OSD ms", "int", None),
    ("open3d-status-help-duration-ms", "Help OSD ms", "int", None),
]

DISPLAY_FIELD_MAP = {field[0]: field for field in DISPLAY_FIELDS}

EMITTER_FIELDS = [
    ("open3d-emitter-enable", "Emitter enabled", "bool", "open3d-emitter-enable", None),
    ("open3d-emitter-tty", "Emitter TTY", "string", "open3d-emitter-tty", None),
    ("open3d-emitter-baud", "Emitter baud", "int", "open3d-emitter-baud", None),
    ("open3d-emitter-auto-reconnect", "Auto reconnect", "bool", "open3d-emitter-auto-reconnect", None),
    ("open3d-emitter-reconnect-ms", "Reconnect ms", "int", "open3d-emitter-reconnect-ms", None),
    ("open3d-emitter-log-io", "Log emitter I/O", "bool", "open3d-emitter-log-io", None),
    ("open3d-emitter-read-on-connect", "Read on connect", "bool", "open3d-emitter-read-on-connect", None),
    ("open3d-emitter-apply-on-connect", "Apply on connect", "bool", "open3d-emitter-apply-on-connect", None),
    ("open3d-emitter-save-on-apply", "Save on apply", "bool", "open3d-emitter-save-on-apply", None),
    (
        "open3d-emitter-opt-serial-debug-enable",
        "Optical debug stream",
        "bool",
        "open3d-emitter-opt-serial-debug-enable",
        None,
    ),
    ("open3d-emitter-opt-csv-enable", "Optical debug CSV", "bool", "open3d-emitter-opt-csv-enable", None),
    ("open3d-emitter-opt-csv-path", "Optical debug CSV path", "string", "open3d-emitter-opt-csv-path", None),
    ("open3d-emitter-opt-csv-flush", "Flush optical CSV", "bool", "open3d-emitter-opt-csv-flush", None),
    ("open3d-trigger-drive-mode", "Drive mode", "choice", "ir_drive_mode", ["optical", "serial"]),
    ("open3d-emitter-ir-protocol", "IR protocol", "int", "ir_protocol", None),
    ("open3d-emitter-ir-frame-delay", "IR frame delay", "int", "ir_frame_delay", None),
    ("open3d-emitter-ir-frame-duration", "IR frame duration", "int", "ir_frame_duration", None),
    ("open3d-emitter-ir-signal-spacing", "IR signal spacing", "int", "ir_signal_spacing", None),
    ("open3d-emitter-target-frametime", "Target frametime", "int", "target_frametime", None),
    ("open3d-emitter-ir-flip-eyes", "IR flip eyes", "int", "ir_flip_eyes", None),
    ("open3d-emitter-ir-avg-timing", "IR average timing", "int", "ir_average_timing_mode", None),
    ("open3d-emitter-opt-block-delay", "Optical block delay", "int", "opt_block_signal_detection_delay", None),
    ("open3d-emitter-opt-min-threshold", "Optical min threshold", "int", "opt_min_threshold_value_to_activate", None),
    ("open3d-emitter-opt-threshold-high", "Optical high threshold", "int", "opt_detection_threshold_high", None),
    ("open3d-emitter-opt-threshold-low", "Optical low threshold", "int", "opt_detection_threshold_low", None),
    ("open3d-emitter-opt-ignore-during-ir", "Ignore optical during IR", "int", "opt_enable_ignore_during_ir", None),
    ("open3d-emitter-opt-dup-realtime", "Duplicate realtime reports", "int", "opt_enable_duplicate_realtime_reporting", None),
    ("open3d-emitter-opt-output-stats", "Output optical stats", "int", "opt_output_stats", None),
    ("open3d-emitter-opt-ignore-duplicates", "Ignore duplicates", "int", "opt_ignore_all_duplicates", None),
    ("open3d-emitter-opt-sensor-filter", "Sensor filter", "int", "opt_sensor_filter_mode", None),
]

EMITTER_FIELD_MAP = {field[0]: field for field in EMITTER_FIELDS}


def coerce_value_for_profile(kind: str, value: Any) -> Any:
    if kind == "bool":
        return parse_bool(value)
    if kind == "int":
        return int(str(value).strip())
    if kind == "float":
        return float(str(value).strip())
    return str(value)


def display_command_value(kind: str, value: Any) -> str:
    return command_value(parse_bool(value) if kind == "bool" else value)


def default_3dplayer_settings_dir() -> Path:
    if os.environ.get("OPEN3D_3DPLAYER_SETTINGS_DIR"):
        return Path(os.environ["OPEN3D_3DPLAYER_SETTINGS_DIR"])
    home = Path(os.environ.get("HOME", ""))
    default_path = home / "Documents" / "open-3d-oled" / "3DPlayer" / "settings"
    return default_path if default_path.exists() else home


def format_preset_hz(value: float) -> str:
    return f"{value:.2f}"


def target_frametime_us(refresh_hz: float) -> int:
    if refresh_hz <= 0.0:
        return 8333
    return max(1000, min(100000, int(round(1_000_000.0 / refresh_hz))))


def probe_gnome_wayland_display_refresh_hz() -> float:
    if os.environ.get("XDG_SESSION_TYPE", "").lower() != "wayland":
        return 0.0
    try:
        result = subprocess.run(
            [
                "gdbus",
                "call",
                "--session",
                "--dest",
                "org.gnome.Mutter.DisplayConfig",
                "--object-path",
                "/org/gnome/Mutter/DisplayConfig",
                "--method",
                "org.gnome.Mutter.DisplayConfig.GetCurrentState",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=1.0,
        )
    except (OSError, subprocess.SubprocessError):
        return 0.0

    if result.returncode != 0:
        return 0.0

    refreshes: list[float] = []
    current_mode_pattern = re.compile(
        r"\('[^']+',\s*\d+,\s*\d+,\s*([0-9]+(?:\.[0-9]+)?),"
        r"\s*[0-9]+(?:\.[0-9]+)?,\s*\[[^\]]*\],\s*\{[^}]*'is-current': <true>[^}]*\}\)",
        re.S,
    )
    for match in current_mode_pattern.finditer(result.stdout):
        try:
            refresh = float(match.group(1))
        except ValueError:
            continue
        if refresh > 0.0:
            refreshes.append(refresh)

    if not refreshes:
        for chunk in result.stdout.split("),"):
            if "'is-current': <true>" not in chunk:
                continue
            match = re.search(r"@[0-9]+(?:\.[0-9]+)?'", chunk)
            if match is None:
                continue
            token = match.group(0).strip("@'")
            try:
                refresh = float(token)
            except ValueError:
                continue
            if refresh > 0.0:
                refreshes.append(refresh)

    return max(refreshes) if refreshes else 0.0


def probe_active_display_refresh_hz() -> float:
    wayland_hz = probe_gnome_wayland_display_refresh_hz()
    if wayland_hz > 0.0:
        return wayland_hz

    try:
        result = subprocess.run(
            ["xrandr", "--current"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=1.0,
        )
    except (OSError, subprocess.SubprocessError):
        return 0.0

    refreshes: list[float] = []
    for line in result.stdout.splitlines():
        if "*" not in line:
            continue
        for token in line.split():
            if "*" not in token:
                continue
            token = token.replace("*", "").replace("+", "")
            try:
                refresh = float(token)
            except ValueError:
                continue
            if refresh > 0.0:
                refreshes.append(refresh)
    return max(refreshes) if refreshes else 0.0


def quick_display_presets(refresh_hz: float) -> list[tuple[str, dict[str, Any]]]:
    if refresh_hz <= 0.0:
        refresh_hz = 120.0

    label_full = format_preset_hz(refresh_hz)
    presets: list[tuple[str, dict[str, Any]]] = []
    if refresh_hz >= 200.0:
        half_hz = refresh_hz / 2.0
        label_half = format_preset_hz(half_hz)
        half_frametime = target_frametime_us(half_hz)
        presets.extend(
            [
                (
                    f"{label_full}/{label_half}",
                    {
                        "open3d-presenter-hz": refresh_hz,
                        "open3d-target-flip-hz": half_hz,
                        "open3d-bfi-enable": False,
                        "open3d-emitter-target-frametime": half_frametime,
                    },
                ),
                (
                    f"{label_full}/{label_half} BFI",
                    {
                        "open3d-presenter-hz": refresh_hz,
                        "open3d-target-flip-hz": half_hz,
                        "open3d-bfi-enable": True,
                        "open3d-bfi-visible-frames": 1,
                        "open3d-bfi-black-frames": 1,
                        "open3d-emitter-target-frametime": half_frametime,
                    },
                ),
                (
                    f"{label_half}/{label_half}",
                    {
                        "open3d-presenter-hz": half_hz,
                        "open3d-target-flip-hz": half_hz,
                        "open3d-bfi-enable": False,
                        "open3d-emitter-target-frametime": half_frametime,
                    },
                ),
            ]
        )
    else:
        full_frametime = target_frametime_us(refresh_hz)
        presets.extend(
            [
                (
                    f"{label_full}/{label_full}",
                    {
                        "open3d-presenter-hz": refresh_hz,
                        "open3d-target-flip-hz": refresh_hz,
                        "open3d-bfi-enable": False,
                        "open3d-emitter-target-frametime": full_frametime,
                    },
                ),
                (
                    f"{label_full}/active",
                    {
                        "open3d-presenter-hz": refresh_hz,
                        "open3d-target-flip-hz": 0.0,
                        "open3d-bfi-enable": False,
                        "open3d-emitter-target-frametime": full_frametime,
                    },
                ),
            ]
        )

    presets.append(
        (
            "Calibration",
            {
                "open3d-trigger-enable": True,
                "open3d-calibration-enable": True,
                "open3d-status-osd-enable": True,
            },
        )
    )
    return presets


def state_dirs() -> list[Path]:
    paths: list[Path] = []
    if os.environ.get("XDG_RUNTIME_DIR"):
        paths.append(Path(os.environ["XDG_RUNTIME_DIR"]) / "open3doled" / "vlc")
    if os.environ.get("XDG_STATE_HOME"):
        paths.append(Path(os.environ["XDG_STATE_HOME"]) / "open3doled" / "vlc")
    if os.environ.get("HOME"):
        paths.append(Path(os.environ["HOME"]) / ".local" / "state" / "open3doled" / "vlc")
    return paths


def newest_socket() -> Path:
    sockets: list[Path] = []
    for directory in state_dirs():
        sockets.extend(Path(p) for p in glob.glob(str(directory / "open3d-vlc-*.sock")))
    sockets = [p for p in sockets if p.exists()]
    if not sockets:
        raise RuntimeError("no Open3D VLC control socket found")
    sockets.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return sockets[0]


class Open3DClient:
    def __init__(self, socket_path: str | None = None) -> None:
        self.socket_path = socket_path or ""
        self.last_socket_path = ""

    def request(self, payload: dict[str, Any]) -> dict[str, Any]:
        last_error: Exception | None = None
        for _ in range(5):
            try:
                path = Path(self.socket_path) if self.socket_path else newest_socket()
                wire = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                    sock.connect(str(path))
                    sock.sendall(wire)
                    chunks: list[bytes] = []
                    while True:
                        data = sock.recv(65536)
                        if not data:
                            break
                        chunks.append(data)
                self.last_socket_path = str(path)
                text = b"".join(chunks).decode("utf-8", errors="replace").strip()
                return json.loads(text)
            except Exception as exc:
                last_error = exc
                if self.socket_path:
                    break
                time.sleep(0.15)
        raise RuntimeError(str(last_error))

    def state(self) -> dict[str, Any]:
        return self.request({"cmd": "get_state"})

    def set_display(self, key: str, value: Any) -> dict[str, Any]:
        return self.request({"cmd": "set_display", "key": key, "value": str(value)})

    def set_emitter(self, key: str, value: Any) -> dict[str, Any]:
        return self.request({"cmd": "set_local_emitter", "key": key, "value": str(value)})

    def action(self, cmd: str) -> dict[str, Any]:
        return self.request({"cmd": cmd})


class ScrollableFrame(ttk.Frame):
    def __init__(self, parent: tk.Widget, padding: int = 0) -> None:
        super().__init__(parent)
        self.canvas = tk.Canvas(self, borderwidth=0, highlightthickness=0)
        self.content = ttk.Frame(self.canvas, padding=padding)
        scrollbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=scrollbar.set)

        self.window_id = self.canvas.create_window((0, 0), window=self.content, anchor="nw")
        self.content.bind(
            "<Configure>",
            lambda _event: self.canvas.configure(scrollregion=self.canvas.bbox("all")),
        )
        self.canvas.bind(
            "<Configure>",
            lambda event: self.canvas.itemconfigure(self.window_id, width=event.width),
        )

        self.canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")


class ControlPanel(ttk.Frame):
    def __init__(self, root: tk.Tk, client: Open3DClient) -> None:
        super().__init__(root, padding=10)
        self.root = root
        self.client = client
        self.vars: dict[str, tk.Variable] = {}
        self.editing: set[str] = set()
        self.staged_vars: set[str] = set()
        self.status = tk.StringVar(value="No state loaded")
        self.socket_var = tk.StringVar(value=client.socket_path)
        self.auto_socket_var = tk.BooleanVar(value=not bool(client.socket_path))
        self.active_socket_var = tk.StringVar(value="Active socket: not connected")
        self.timing_var = tk.StringVar(value="Timing: not connected")
        self.opt_debug_var = tk.StringVar(value="Opt debug: not connected")
        self.profile_var = tk.StringVar(value="default")
        self.import_presenter_hz_var = tk.StringVar(value="")
        self.socket_entry: ttk.Entry | None = None
        self.profile_combo: ttk.Combobox | None = None
        self.display_preset_frame: ttk.LabelFrame | None = None
        self.display_preset_refresh_hz: float = 0.0
        self.refresh_probe_hz: float = 0.0
        self.refresh_probe_mtime: float = 0.0
        self.pack(fill="both", expand=True)
        self._build()
        self.refresh()
        self.after(1000, self.poll)

    def _build(self) -> None:
        top = ttk.Frame(self)
        top.pack(fill="x", pady=(0, 8))
        ttk.Checkbutton(
            top,
            text="Auto",
            variable=self.auto_socket_var,
            command=self.toggle_auto_socket,
        ).pack(side="left", padx=(0, 6))
        ttk.Label(top, text="Socket").pack(side="left")
        socket_entry = ttk.Entry(top, textvariable=self.socket_var, width=64)
        self.socket_entry = socket_entry
        socket_entry.pack(side="left", padx=6, fill="x", expand=True)
        ttk.Button(top, text="Use Newest", command=self.use_newest_socket).pack(side="left", padx=2)
        ttk.Button(top, text="Refresh", command=self.refresh).pack(side="left", padx=2)
        ttk.Label(self, textvariable=self.active_socket_var).pack(anchor="w", pady=(0, 8))
        ttk.Label(self, textvariable=self.timing_var).pack(anchor="w", pady=(0, 8))
        ttk.Label(self, textvariable=self.opt_debug_var).pack(anchor="w", pady=(0, 8))
        self.update_socket_entry_state()

        profiles = ttk.Frame(self)
        profiles.pack(fill="x", pady=(0, 8))
        ttk.Label(profiles, text="Display Profile").pack(side="left")
        self.profile_combo = ttk.Combobox(profiles, textvariable=self.profile_var, width=28)
        self.profile_combo.pack(side="left", padx=6)
        ttk.Button(profiles, text="Save", command=self.save_profile).pack(side="left", padx=2)
        ttk.Button(profiles, text="Load", command=self.load_profile).pack(side="left", padx=2)
        ttk.Button(profiles, text="Refresh Profiles", command=self.refresh_profiles).pack(side="left", padx=2)
        ttk.Label(profiles, text="Import presenter Hz").pack(side="left", padx=(12, 2))
        ttk.Entry(profiles, textvariable=self.import_presenter_hz_var, width=8).pack(side="left", padx=2)
        ttk.Button(profiles, text="Import Legacy", command=self.import_legacy_profile).pack(side="left", padx=2)
        self.refresh_profiles()

        notebook = ttk.Notebook(self)
        notebook.pack(fill="both", expand=True)
        display_page = ScrollableFrame(notebook, padding=8)
        emitter_page = ScrollableFrame(notebook, padding=8)
        notebook.add(display_page, text="Display")
        notebook.add(emitter_page, text="Emitter")

        display = display_page.content
        emitter = emitter_page.content
        self._build_display_presets(display)
        self._build_display_json_actions(display)
        self._build_fields(display, "display", DISPLAY_FIELDS, start_row=4)
        self._build_emitter_json_actions(emitter)
        self._build_fields(emitter, "emitter", EMITTER_FIELDS, start_row=3)

        actions = ttk.Frame(emitter)
        actions.grid(row=len(EMITTER_FIELDS) + 4, column=0, columnspan=4, sticky="w", pady=(12, 0))
        ttk.Button(actions, text="Read From Emitter", command=lambda: self.run_action("read_emitter")).pack(
            side="left", padx=3
        )
        ttk.Button(actions, text="Send UI to Emitter RAM", command=self.send_emitter_ui_to_ram).pack(
            side="left", padx=3
        )
        ttk.Button(actions, text="Save UI to Emitter EEPROM", command=self.save_emitter_ui_to_eeprom).pack(
            side="left", padx=3
        )
        ttk.Button(actions, text="Disconnect", command=lambda: self.run_action("disconnect_emitter")).pack(
            side="left", padx=3
        )
        ttk.Button(actions, text="Reconnect", command=lambda: self.run_action("reconnect_emitter")).pack(
            side="left", padx=3
        )

        ttk.Label(self, textvariable=self.status).pack(anchor="w", pady=(8, 0))

    def _build_display_presets(self, parent: ttk.Frame) -> None:
        self.display_preset_frame = ttk.LabelFrame(parent, text="Timing and Calibration Presets", padding=8)
        self.display_preset_frame.grid(row=0, column=0, columnspan=4, sticky="ew", pady=(0, 10))
        self._populate_display_presets(120.0)
        ttk.Label(
            parent,
            text="Timing presets track the active monitor refresh via GNOME Wayland or xrandr when available.",
        ).grid(row=1, column=0, columnspan=4, sticky="w", pady=(0, 8))

    def _build_display_json_actions(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="3DPlayer Display JSON", padding=8)
        frame.grid(row=2, column=0, columnspan=4, sticky="ew", pady=(0, 8))
        ttk.Button(frame, text="Load JSON to UI", command=self.load_3dplayer_display_json_to_ui).pack(
            side="left", padx=3
        )
        ttk.Button(frame, text="Apply Display to VLC", command=self.apply_display_ui_to_vlc).pack(
            side="left", padx=3
        )
        ttk.Button(frame, text="Save UI as JSON", command=self.save_3dplayer_display_json).pack(
            side="left", padx=3
        )
        ttk.Label(
            parent,
            text="Loading JSON only stages UI values. Use Apply Display to VLC to change active playback.",
        ).grid(row=3, column=0, columnspan=4, sticky="w", pady=(0, 8))

    def _build_emitter_json_actions(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="3DPlayer Emitter JSON", padding=8)
        frame.grid(row=0, column=0, columnspan=4, sticky="ew", pady=(0, 8))
        ttk.Button(frame, text="Load JSON to UI", command=self.load_3dplayer_emitter_json_to_ui).pack(
            side="left", padx=3
        )
        ttk.Button(frame, text="Save UI as JSON", command=self.save_3dplayer_emitter_json).pack(
            side="left", padx=3
        )
        ttk.Label(
            parent,
            text="Loading JSON only stages UI values. Use Send UI to Emitter RAM, then Save UI to EEPROM if desired.",
        ).grid(row=1, column=0, columnspan=4, sticky="w", pady=(0, 8))
        ttk.Separator(parent, orient="horizontal").grid(row=2, column=0, columnspan=4, sticky="ew", pady=(0, 8))

    def _populate_display_presets(self, refresh_hz: float) -> None:
        if self.display_preset_frame is None:
            return
        for child in self.display_preset_frame.winfo_children():
            child.destroy()
        for label, values in quick_display_presets(refresh_hz):
            ttk.Button(
                self.display_preset_frame,
                text=label,
                command=lambda l=label, v=values: self.apply_display_preset(l, v),
            ).pack(side="left", padx=3)
        self.display_preset_refresh_hz = refresh_hz

    def _build_fields(
        self,
        parent: ttk.Frame,
        section: str,
        fields: list[tuple[Any, ...]],
        start_row: int = 0,
    ) -> None:
        for row, field in enumerate(fields):
            grid_row = row + start_row
            key = field[0]
            label = field[1]
            kind = field[2]
            choices = field[3] if section == "display" else field[4]
            var_key = f"{section}:{key}"
            ttk.Label(parent, text=label).grid(row=grid_row, column=0, sticky="w", padx=(0, 8), pady=2)
            if kind == "bool":
                var = tk.BooleanVar(value=False)
                widget = ttk.Checkbutton(parent, variable=var, command=lambda f=field, s=section: self.set_field(s, f))
                widget.grid(row=grid_row, column=1, sticky="w", pady=2)
            elif kind == "choice":
                var = tk.StringVar(value="")
                widget = ttk.Combobox(parent, textvariable=var, values=choices, width=18, state="readonly")
                widget.grid(row=grid_row, column=1, sticky="ew", pady=2)
                widget.bind("<<ComboboxSelected>>", lambda _event, f=field, s=section: self.set_field(s, f))
            else:
                var = tk.StringVar(value="")
                widget = ttk.Entry(parent, textvariable=var, width=20)
                widget.grid(row=grid_row, column=1, sticky="ew", pady=2)
                widget.bind("<FocusIn>", lambda _event, k=var_key: self.editing.add(k))
                widget.bind("<FocusOut>", lambda _event, k=var_key: self.editing.discard(k))
                widget.bind("<Return>", lambda _event, f=field, s=section: self.set_field(s, f))
                ttk.Button(parent, text="Set", command=lambda f=field, s=section: self.set_field(s, f)).grid(
                    row=grid_row, column=2, padx=4, pady=2
                )
            self.vars[var_key] = var
        parent.columnconfigure(1, weight=1)

    def use_newest_socket(self) -> None:
        try:
            self.auto_socket_var.set(False)
            self.socket_var.set(str(newest_socket()))
            self.client.socket_path = self.socket_var.get()
            self.update_socket_entry_state()
            self.refresh()
        except Exception as exc:
            self.status.set(f"Socket discovery failed: {exc}")

    def toggle_auto_socket(self) -> None:
        self.update_socket_entry_state()
        self.refresh()

    def update_socket_entry_state(self) -> None:
        if self.socket_entry is not None:
            self.socket_entry.configure(state="disabled" if self.auto_socket_var.get() else "normal")

    def refresh_profiles(self) -> None:
        names = list_profiles()
        values = names or ["default"]
        if self.profile_combo is not None:
            self.profile_combo.configure(values=values)
        if not self.profile_var.get().strip():
            self.profile_var.set(values[0])

    def collect_display_values(self, keys: tuple[str, ...] | None = None) -> dict[str, Any]:
        display: dict[str, Any] = {}
        for key, _label, kind, _choices in DISPLAY_FIELDS:
            if keys is not None and key not in keys:
                continue
            value = self.vars[f"display:{key}"].get()
            display[key] = coerce_value_for_profile(kind, value)
        return display

    def collect_display_profile(self) -> dict[str, Any]:
        display = self.collect_display_values()
        return {
            "schema": PROFILE_SCHEMA,
            "kind": "open3d-vlc-display-settings-profile",
            "saved_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "display": display,
        }

    def collect_emitter_values(self, keys: tuple[str, ...] | None = None) -> dict[str, Any]:
        emitter: dict[str, Any] = {}
        for key, _label, kind, _state_key, _choices in EMITTER_FIELDS:
            if keys is not None and key not in keys:
                continue
            value = self.vars[f"emitter:{key}"].get()
            emitter[key] = coerce_value_for_profile(kind, value)
        return emitter

    def set_section_vars(self, section: str, values: dict[str, Any], staged: bool) -> int:
        applied = 0
        field_map = DISPLAY_FIELD_MAP if section == "display" else EMITTER_FIELD_MAP
        for key, value in values.items():
            field = field_map.get(key)
            if field is None:
                continue
            kind = field[2]
            var_key = f"{section}:{key}"
            var = self.vars.get(var_key)
            if var is None:
                continue
            var.set(parse_bool(value) if kind == "bool" else str(value))
            if staged:
                self.staged_vars.add(var_key)
            else:
                self.staged_vars.discard(var_key)
            applied += 1
        return applied

    @staticmethod
    def format_json_load_status(result: dict[str, object]) -> str:
        loaded = len(result.get("loaded_keys", []))
        defaulted = len(result.get("defaulted_keys", []))
        ignored = len(result.get("ignored_keys", []))
        return f"{loaded} loaded, {defaulted} defaulted, {ignored} ignored"

    def save_profile(self) -> None:
        name = normalize_profile_name(self.profile_var.get())
        self.profile_var.set(name)
        try:
            path = save_display_profile(name, self.collect_display_profile()["display"])
            self.refresh_profiles()
            self.status.set(f"Saved display profile: {path}")
        except Exception as exc:
            self.status.set(f"Profile save failed: {exc}")

    def load_profile(self) -> None:
        name = normalize_profile_name(self.profile_var.get())
        self.profile_var.set(name)
        try:
            display = load_display_profile(name)
            applied = self.queue_display_settings(display)
            self.status.set(f"Loaded display profile {name}: queued {applied} setting(s)")
            self.after(500, self.refresh)
        except Exception as exc:
            self.status.set(f"Profile load failed: {exc}")

    def import_legacy_profile(self) -> None:
        path_text = filedialog.askopenfilename(
            title="Import 3DPlayer/MPC Display Settings",
            filetypes=[
                ("Display settings", "*.display_settings.json"),
                ("JSON files", "*.json"),
                ("All files", "*"),
            ],
        )
        if not path_text:
            return
        try:
            path = Path(path_text)
            display = import_legacy_display_settings(path)
            presenter_hz = self.import_presenter_hz_var.get().strip()
            if presenter_hz:
                display["open3d-presenter-hz"] = float(presenter_hz)
            name = normalize_profile_name(legacy_profile_default_name(path))
            saved_path = save_display_profile(name, display)
            self.profile_var.set(name)
            self.refresh_profiles()
            self.status.set(f"Imported legacy display profile: {saved_path}; use Load to apply")
        except Exception as exc:
            self.status.set(f"Legacy import failed: {exc}")

    def load_3dplayer_display_json_to_ui(self) -> None:
        path_text = filedialog.askopenfilename(
            title="Load 3DPlayer Display Settings to UI",
            initialdir=str(default_3dplayer_settings_dir()),
            filetypes=[
                ("3DPlayer display settings", "*.display_settings.json"),
                ("JSON files", "*.json"),
                ("All files", "*"),
            ],
        )
        if not path_text:
            return
        try:
            result = load_3dplayer_display_settings(Path(path_text))
            values = result["values"]
            if not isinstance(values, dict):
                raise RuntimeError("display JSON loader returned no values")
            applied = self.set_section_vars("display", values, staged=True)
            self.status.set(
                f"Loaded 3DPlayer display JSON to UI only: {applied} field(s), "
                f"{self.format_json_load_status(result)}. Not applied to VLC."
            )
        except Exception as exc:
            self.status.set(f"3DPlayer display JSON load failed: {exc}")

    def save_3dplayer_display_json(self) -> None:
        path_text = filedialog.asksaveasfilename(
            title="Save UI as 3DPlayer Display Settings",
            initialdir=str(default_3dplayer_settings_dir()),
            filetypes=[("3DPlayer display settings", "*.display_settings.json")],
            defaultextension=".display_settings.json",
        )
        if not path_text:
            return
        try:
            path = write_3dplayer_display_settings(
                Path(path_text),
                self.collect_display_values(OPEN3D_DISPLAY_3DPLAYER_KEYS),
            )
            self.status.set(f"Saved 3DPlayer display JSON: {path}")
        except Exception as exc:
            self.status.set(f"3DPlayer display JSON save failed: {exc}")

    def apply_display_ui_to_vlc(self) -> None:
        try:
            values = self.collect_display_values()
            applied = self.queue_display_settings(values)
            self.status.set(f"Applied UI display values to VLC: queued {applied} setting(s)")
            self.after(500, self.refresh)
        except Exception as exc:
            self.status.set(f"Apply display to VLC failed: {exc}")

    def load_3dplayer_emitter_json_to_ui(self) -> None:
        path_text = filedialog.askopenfilename(
            title="Load 3DPlayer Emitter Settings to UI",
            initialdir=str(default_3dplayer_settings_dir()),
            filetypes=[
                ("3DPlayer emitter settings", "*.emitter_settings.json"),
                ("JSON files", "*.json"),
                ("All files", "*"),
            ],
        )
        if not path_text:
            return
        try:
            result = load_3dplayer_emitter_settings(Path(path_text))
            values = result["values"]
            if not isinstance(values, dict):
                raise RuntimeError("emitter JSON loader returned no values")
            applied = self.set_section_vars("emitter", values, staged=True)
            self.status.set(
                f"Loaded 3DPlayer emitter JSON to UI only: {applied} field(s), "
                f"{self.format_json_load_status(result)}. Not sent to hardware."
            )
        except Exception as exc:
            self.status.set(f"3DPlayer emitter JSON load failed: {exc}")

    def save_3dplayer_emitter_json(self) -> None:
        path_text = filedialog.asksaveasfilename(
            title="Save UI as 3DPlayer Emitter Settings",
            initialdir=str(default_3dplayer_settings_dir()),
            filetypes=[("3DPlayer emitter settings", "*.emitter_settings.json")],
            defaultextension=".emitter_settings.json",
        )
        if not path_text:
            return
        try:
            path = write_3dplayer_emitter_settings(
                Path(path_text),
                self.collect_emitter_values(OPEN3D_EMITTER_3DPLAYER_KEYS),
            )
            self.status.set(f"Saved 3DPlayer emitter JSON: {path}")
        except Exception as exc:
            self.status.set(f"3DPlayer emitter JSON save failed: {exc}")

    def queue_display_settings(self, values: dict[str, Any]) -> int:
        applied = 0
        for key, value in values.items():
            field = DISPLAY_FIELD_MAP.get(key)
            if field is not None:
                kind = field[2]
                self.vars[f"display:{key}"].set(parse_bool(value) if kind == "bool" else str(value))
                self.staged_vars.discard(f"display:{key}")
                self.client.set_display(key, display_command_value(kind, value))
                applied += 1
                continue

            field = EMITTER_FIELD_MAP.get(key)
            if field is None:
                continue
            kind = field[2]
            self.vars[f"emitter:{key}"].set(parse_bool(value) if kind == "bool" else str(value))
            self.staged_vars.discard(f"emitter:{key}")
            self.client.set_emitter(key, display_command_value(kind, value))
            applied += 1
        return applied

    def queue_emitter_settings(self, values: dict[str, Any]) -> int:
        applied = 0
        for key, value in values.items():
            field = EMITTER_FIELD_MAP.get(key)
            if field is None:
                continue
            kind = field[2]
            self.vars[f"emitter:{key}"].set(parse_bool(value) if kind == "bool" else str(value))
            self.staged_vars.discard(f"emitter:{key}")
            self.client.set_emitter(key, display_command_value(kind, value))
            applied += 1
        return applied

    def send_emitter_ui_to_ram(self) -> None:
        try:
            values = self.collect_emitter_values(OPEN3D_EMITTER_3DPLAYER_KEYS)
            applied = self.queue_emitter_settings(values)
            self.client.action("apply_emitter")
            self.status.set(
                f"Sent UI emitter values to emitter RAM: queued {applied} setting(s). Not saved to EEPROM."
            )
            self.after(500, self.refresh)
        except Exception as exc:
            self.status.set(f"Send emitter UI to RAM failed: {exc}")

    def save_emitter_ui_to_eeprom(self) -> None:
        try:
            values = self.collect_emitter_values(OPEN3D_EMITTER_3DPLAYER_KEYS)
            applied = self.queue_emitter_settings(values)
            self.client.action("apply_emitter")
            self.client.action("save_emitter")
            self.status.set(
                f"Queued UI emitter values for EEPROM save: {applied} setting(s)."
            )
            self.after(500, self.refresh)
        except Exception as exc:
            self.status.set(f"Save emitter UI to EEPROM failed: {exc}")

    def apply_display_preset(self, label: str, values: dict[str, Any]) -> None:
        try:
            applied = self.queue_display_settings(values)
            self.status.set(f"Applied {label}: queued {applied} setting(s)")
            self.after(500, self.refresh)
        except Exception as exc:
            self.status.set(f"Preset failed: {exc}")

    def refresh(self) -> None:
        self.client.socket_path = "" if self.auto_socket_var.get() else self.socket_var.get().strip()
        try:
            state = self.client.state()
            self.apply_state(state)
            self.refresh_display_presets(state)
            socket_path = state.get("socket") or self.client.last_socket_path
            if socket_path:
                self.active_socket_var.set(f"Active socket: {socket_path}")
            self.timing_var.set(self.format_timing(state.get("timing", {})))
            emitter = state.get("local_emitter", {})
            self.opt_debug_var.set(self.format_opt_debug(emitter))
            connected = "connected" if emitter.get("connected") else "not connected"
            dirty = "dirty" if emitter.get("dirty") else "clean"
            if self.staged_vars:
                self.status.set(
                    f"Open3D state loaded: emitter {connected}, settings {dirty}; "
                    f"{len(self.staged_vars)} staged UI value(s) not applied"
                )
            else:
                self.status.set(f"Open3D state loaded: emitter {connected}, settings {dirty}")
        except Exception as exc:
            self.active_socket_var.set("Active socket: not connected")
            self.timing_var.set("Timing: not connected")
            self.opt_debug_var.set("Opt debug: not connected")
            if self.staged_vars:
                self.status.set(
                    f"State refresh failed; {len(self.staged_vars)} staged UI value(s) retained: {exc}"
                )
            else:
                self.status.set(f"State refresh failed: {exc}")

    def current_display_refresh_hz(self, state: dict[str, Any]) -> float:
        now = time.monotonic()
        if self.refresh_probe_hz > 0.0 and now - self.refresh_probe_mtime < 5.0:
            return self.refresh_probe_hz

        hz = probe_active_display_refresh_hz()
        if hz <= 0.0:
            timing = state.get("timing", {})
            if isinstance(timing, dict):
                try:
                    hz = float(timing.get("presenter-hz", 0.0))
                except (TypeError, ValueError):
                    hz = 0.0
        if hz <= 0.0:
            hz = 120.0

        self.refresh_probe_hz = hz
        self.refresh_probe_mtime = now
        return hz

    def refresh_display_presets(self, state: dict[str, Any]) -> None:
        refresh_hz = self.current_display_refresh_hz(state)
        if abs(refresh_hz - self.display_preset_refresh_hz) < 0.01:
            return
        self._populate_display_presets(refresh_hz)

    def poll(self) -> None:
        self.refresh()
        self.after(1000, self.poll)

    def apply_state(self, state: dict[str, Any]) -> None:
        display = state.get("display", {})
        emitter = state.get("local_emitter", {})
        for key, _label, kind, _choices in DISPLAY_FIELDS:
            self._set_var(f"display:{key}", display.get(key), kind)
        for key, _label, kind, state_key, _choices in EMITTER_FIELDS:
            value = emitter.get(state_key)
            if key == "open3d-trigger-drive-mode" and isinstance(value, int):
                value = "serial" if value == 1 else "optical"
            self._set_var(f"emitter:{key}", value, kind)

    def format_timing(self, timing: Any) -> str:
        if not isinstance(timing, dict):
            return "Timing: not reported by active vout"
        try:
            presenter_hz = float(timing.get("presenter-hz", 0.0))
            target_hz = float(timing.get("target-flip-hz", 0.0))
        except (TypeError, ValueError):
            return "Timing: not reported by active vout"
        target = f"{target_hz:.3f} Hz" if target_hz > 0.0 else "auto"
        mode = timing.get("flip-mode", "unknown")
        divider = int(timing.get("flip-presenter-divider") or 0)
        if mode == "presenter-divider" and divider > 0:
            mode = f"{mode} x{divider}"
        eye = timing.get("current-eye", "unknown")
        late = timing.get("late-flip-events", 0)
        misses = timing.get("presenter-deadline-miss-events", 0)
        return (
            f"Timing: presenter {presenter_hz:.3f} Hz, target {target}, "
            f"mode {mode}, eye {eye}, late flips {late}, presenter misses {misses}"
        )

    def format_opt_debug(self, emitter: Any) -> str:
        if not isinstance(emitter, dict):
            return "Opt debug: not reported by active vout"
        stream_enabled = bool(emitter.get("open3d-emitter-opt-serial-debug-enable"))
        stream_active = bool(emitter.get("open3d-emitter-opt-serial-debug-active"))
        csv_enabled = bool(emitter.get("open3d-emitter-opt-csv-enable"))
        samples = int(emitter.get("opt_debug_sample_count") or 0)
        prefix = (
            f"Opt debug: stream {'on' if stream_enabled else 'off'}"
            f"/{'active' if stream_active else 'inactive'}, "
            f"CSV {'on' if csv_enabled else 'off'}, samples {samples}"
        )
        if not emitter.get("opt_debug_latest_valid"):
            return f"{prefix}, no sample yet"
        return (
            f"{prefix}, "
            f"L {emitter.get('opt_debug_left_sensor')} "
            f"R {emitter.get('opt_debug_right_sensor')} "
            f"active {int(bool(emitter.get('opt_debug_readings_active')))} "
            f"trig {int(bool(emitter.get('opt_debug_reading_triggered_left')))}/"
            f"{int(bool(emitter.get('opt_debug_reading_triggered_right')))} "
            f"sent {int(bool(emitter.get('opt_debug_left_sent_ir')))}/"
            f"{int(bool(emitter.get('opt_debug_right_sent_ir')))}"
        )

    def _set_var(self, var_key: str, value: Any, kind: str) -> None:
        if var_key in self.editing or var_key in self.staged_vars or value is None:
            return
        var = self.vars.get(var_key)
        if var is None:
            return
        if kind == "bool":
            var.set(bool(value))
        else:
            var.set(str(value))

    def set_field(self, section: str, field: tuple[Any, ...]) -> None:
        key = field[0]
        kind = field[2]
        var = self.vars[f"{section}:{key}"]
        value: Any = var.get()
        if kind == "bool":
            value = "true" if bool(value) else "false"
        try:
            if section == "display":
                self.client.set_display(key, value)
            else:
                self.client.set_emitter(key, value)
            self.staged_vars.discard(f"{section}:{key}")
            self.status.set(f"Queued {key}={value}")
            self.after(200, self.refresh)
        except Exception as exc:
            self.status.set(f"Set failed for {key}: {exc}")

    def run_action(self, cmd: str) -> None:
        try:
            self.client.action(cmd)
            self.status.set(f"Queued {cmd}")
            self.after(500, self.refresh)
        except Exception as exc:
            self.status.set(f"Action failed: {exc}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Open3D VLC runtime control panel")
    parser.add_argument("--socket", help="Specific open3d-vlc-*.sock path")
    args = parser.parse_args()

    root = tk.Tk()
    root.title("Open3D VLC Control")
    root.minsize(720, 620)
    ControlPanel(root, Open3DClient(args.socket))
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
