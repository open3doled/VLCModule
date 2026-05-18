#!/usr/bin/env python3
"""Shared helpers for Open3D VLC runtime control tools."""

from __future__ import annotations

import json
import os
import re
import time
from pathlib import Path
from typing import Any


PROFILE_SUFFIX = ".display_settings.json"
PROFILE_SCHEMA = 1
TRANSIENT_DISPLAY_KEYS = {
    "open3d-calibration-enable",
}

THREEDPLAYER_DISPLAY_DEFAULTS: dict[str, Any] = {
    "target_framerate": "0",
    "add_n_bfi_frames_every_frame": "0",
    "display_resolution": "1920x1080",
    "display_zoom_factor": "100",
    "display_size": "55",
    "decoder_preference": "default (GStreamers first choice)",
    "whitebox_brightness": "255",
    "whitebox_corner_position": "top_left",
    "whitebox_vertical_position": "0",
    "whitebox_horizontal_position": "0",
    "whitebox_size": "13",
    "whitebox_horizontal_spacing": "23",
    "blackbox_border": "10",
    "calibration_mode": False,
    "display_osd_timestamp": False,
    "disable_3d_on_mouse_move_under_windows": True,
}

THREEDPLAYER_EMITTER_DEFAULTS: dict[str, Any] = {
    "ir_drive_mode": "0",
    "ir_protocol": "6",
    "ir_frame_delay": "500",
    "ir_frame_duration": "7000",
    "ir_signal_spacing": "30",
    "ir_flip_eyes": "0",
    "ir_average_timing_mode": "0",
    "target_frametime": "0",
    "opt_block_signal_detection_delay": "7500",
    "opt_ignore_all_duplicates": "0",
    "opt_sensor_filter_mode_variable": "0",
    "opt_min_threshold_value_to_activate": "10",
    "opt_detection_threshold_high": "128",
    "opt_detection_threshold_low": "32",
    "opt_enable_ignore_during_ir": "0",
    "opt_enable_duplicate_realtime_reporting": "0",
    "opt_output_stats": "0",
}

LEGACY_CORNER_MAP = {
    "top_left": "top-left",
    "top-left": "top-left",
    "topleft": "top-left",
    "top_right": "top-right",
    "top-right": "top-right",
    "topright": "top-right",
    "bottom_left": "bottom-left",
    "bottom-left": "bottom-left",
    "bottomleft": "bottom-left",
    "bottom_right": "bottom-right",
    "bottom-right": "bottom-right",
    "bottomright": "bottom-right",
}

LEGACY_ASPECT_MAP = {
    "side-by-side": "sbs",
    "side_by_side": "sbs",
    "sidebyside": "sbs",
    "sbs": "sbs",
    "left-right": "sbs",
    "leftright": "sbs",
    "lr": "sbs",
    "top-and-bottom": "tb",
    "top_and_bottom": "tb",
    "topandbottom": "tb",
    "top-bottom": "tb",
    "top_bottom": "tb",
    "topbottom": "tb",
    "over-under": "tb",
    "over_under": "tb",
    "overunder": "tb",
    "tb": "tb",
    "ou": "tb",
}

LEGACY_DISPLAY_KEY_MAP = {
    "target_framerate": ("open3d-target-flip-hz", "float"),
    "pageflip_enabled": ("open3d-enable", "bool"),
    "pageflip_flip_eyes": ("open3d-flip-eyes", "bool"),
    "pageflip_show_overlay": ("open3d-status-osd-enable", "bool"),
    "whitebox_brightness": ("open3d-trigger-brightness", "int"),
    "whitebox_vertical_position": ("open3d-trigger-offset-y", "int"),
    "whitebox_horizontal_position": ("open3d-trigger-offset-x", "int"),
    "whitebox_size": ("open3d-trigger-size", "int"),
    "whitebox_horizontal_spacing": ("open3d-trigger-spacing", "int"),
    "blackbox_border": ("open3d-trigger-black-border", "int"),
}

THREEDPLAYER_DISPLAY_KEY_MAP = {
    "target_framerate": ("open3d-target-flip-hz", "float"),
    "whitebox_brightness": ("open3d-trigger-brightness", "int"),
    "whitebox_vertical_position": ("open3d-trigger-offset-y", "int"),
    "whitebox_horizontal_position": ("open3d-trigger-offset-x", "int"),
    "whitebox_size": ("open3d-trigger-size", "int"),
    "whitebox_horizontal_spacing": ("open3d-trigger-spacing", "int"),
    "blackbox_border": ("open3d-trigger-black-border", "int"),
    "calibration_mode": ("open3d-calibration-enable", "bool"),
}

THREEDPLAYER_EMITTER_KEY_MAP = {
    "ir_drive_mode": ("open3d-trigger-drive-mode", "drive_mode", ()),
    "ir_protocol": ("open3d-emitter-ir-protocol", "int", ()),
    "ir_frame_delay": ("open3d-emitter-ir-frame-delay", "int", ()),
    "ir_frame_duration": ("open3d-emitter-ir-frame-duration", "int", ()),
    "ir_signal_spacing": ("open3d-emitter-ir-signal-spacing", "int", ()),
    "ir_flip_eyes": ("open3d-emitter-ir-flip-eyes", "int", ()),
    "ir_average_timing_mode": ("open3d-emitter-ir-avg-timing", "int", ()),
    "target_frametime": ("open3d-emitter-target-frametime", "int", ()),
    "opt_block_signal_detection_delay": (
        "open3d-emitter-opt-block-delay",
        "int",
        ("opt101_block_signal_detection_delay",),
    ),
    "opt_ignore_all_duplicates": (
        "open3d-emitter-opt-ignore-duplicates",
        "int",
        ("opt101_ignore_all_duplicates",),
    ),
    "opt_sensor_filter_mode_variable": (
        "open3d-emitter-opt-sensor-filter",
        "int",
        ("opt_sensor_filter_mode", "opt101_sensor_filter_mode"),
    ),
    "opt_min_threshold_value_to_activate": (
        "open3d-emitter-opt-min-threshold",
        "int",
        ("opt101_min_threshold_value_to_activate",),
    ),
    "opt_detection_threshold_high": (
        "open3d-emitter-opt-threshold-high",
        "int",
        ("opt101_detection_threshold",),
    ),
    "opt_detection_threshold_low": ("open3d-emitter-opt-threshold-low", "int", ()),
    "opt_enable_ignore_during_ir": (
        "open3d-emitter-opt-ignore-during-ir",
        "int",
        ("opt101_enable_ignore_during_ir",),
    ),
    "opt_enable_duplicate_realtime_reporting": (
        "open3d-emitter-opt-dup-realtime",
        "int",
        ("opt101_enable_duplicate_realtime_reporting",),
    ),
    "opt_output_stats": ("open3d-emitter-opt-output-stats", "int", ()),
}

OPEN3D_EMITTER_3DPLAYER_KEYS = tuple(
    mapping[0] for mapping in THREEDPLAYER_EMITTER_KEY_MAP.values()
)

OPEN3D_DISPLAY_3DPLAYER_KEYS = tuple(
    mapping[0] for mapping in THREEDPLAYER_DISPLAY_KEY_MAP.values()
) + (
    "open3d-bfi-enable",
    "open3d-bfi-black-frames",
    "open3d-trigger-enable",
)

DISPLAY_PRESETS: dict[str, dict[str, Any]] = {
    "240hz-120flip": {
        "label": "240 Hz panel / 120 Hz flip",
        "display": {
            "open3d-presenter-hz": 240.0,
            "open3d-target-flip-hz": 120.0,
        },
        "emitter": {
            "open3d-emitter-target-frametime": 8333,
        },
    },
    "240hz-120flip-bfi": {
        "label": "240 Hz panel / 120 Hz flip + 1:1 BFI",
        "display": {
            "open3d-presenter-hz": 240.0,
            "open3d-target-flip-hz": 120.0,
            "open3d-bfi-enable": True,
            "open3d-bfi-visible-frames": 1,
            "open3d-bfi-black-frames": 1,
        },
        "emitter": {
            "open3d-emitter-target-frametime": 8333,
        },
    },
    "120hz-auto": {
        "label": "120 Hz presenter / auto flip",
        "display": {
            "open3d-presenter-hz": 120.0,
            "open3d-target-flip-hz": 0.0,
        },
        "emitter": {
            "open3d-emitter-target-frametime": 8333,
        },
    },
    "calibration-osd": {
        "label": "Calibration OSD",
        "display": {
            "open3d-trigger-enable": True,
            "open3d-calibration-enable": True,
            "open3d-status-osd-enable": True,
        },
    },
}


def config_home() -> Path:
    if os.environ.get("XDG_CONFIG_HOME"):
        return Path(os.environ["XDG_CONFIG_HOME"])
    if os.environ.get("HOME"):
        return Path(os.environ["HOME"]) / ".config"
    raise RuntimeError("HOME or XDG_CONFIG_HOME is required")


def profiles_dir() -> Path:
    return config_home() / "open3doled" / "vlc" / "profiles"


def display_settings_path() -> Path:
    return config_home() / "open3doled" / "vlc" / "open3d_display_settings.json"


def local_emitter_connection_path() -> Path:
    return config_home() / "open3doled" / "vlc" / "local_emitter_connection.json"


def normalize_profile_name(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", name.strip())
    cleaned = cleaned.strip("._-")
    return cleaned or "default"


def profile_path(name: str) -> Path:
    normalized = normalize_profile_name(name)
    if normalized.endswith(PROFILE_SUFFIX):
        normalized = normalized[: -len(PROFILE_SUFFIX)]
    return profiles_dir() / f"{normalized}{PROFILE_SUFFIX}"


def list_profiles() -> list[str]:
    directory = profiles_dir()
    if not directory.exists():
        return []
    return [path.name[: -len(PROFILE_SUFFIX)] for path in sorted(directory.glob(f"*{PROFILE_SUFFIX}"))]


def parse_bool(value: Any) -> bool:
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on")
    return bool(value)


def command_value(value: object) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def coerce_legacy_float(value: Any) -> float:
    return float(str(value).strip())


def coerce_legacy_int(value: Any) -> int:
    return int(float(str(value).strip()))


def coerce_legacy_value(value: Any, value_type: str) -> object:
    if value_type == "bool":
        return parse_bool(value)
    if value_type == "int":
        return coerce_legacy_int(value)
    if value_type == "float":
        return coerce_legacy_float(value)
    return value


def normalize_legacy_token(value: Any) -> str:
    return str(value).strip().lower().replace(" ", "_")


def format_legacy_number(value: Any) -> str:
    try:
        numeric = float(str(value).strip())
    except (TypeError, ValueError):
        return str(value)
    if numeric.is_integer():
        return str(int(numeric))
    return f"{numeric:g}"


def open3d_drive_mode_to_3dplayer(value: Any) -> str:
    token = str(value).strip().lower()
    if token == "serial":
        return "1"
    if token == "optical":
        return "0"
    try:
        return str(int(float(token)))
    except ValueError:
        return THREEDPLAYER_EMITTER_DEFAULTS["ir_drive_mode"]


def threedplayer_drive_mode_to_open3d(value: Any) -> str:
    token = str(value).strip().lower()
    if token in ("1", "serial", "pcserial"):
        return "serial"
    return "optical"


def threedplayer_load_result(
    values: dict[str, object],
    payload: dict[str, object],
    supported_keys: set[str],
    defaulted_keys: list[str],
) -> dict[str, object]:
    ignored = sorted(key for key in payload if key not in supported_keys)
    return {
        "values": values,
        "loaded_keys": sorted(key for key in supported_keys if key in payload),
        "defaulted_keys": sorted(defaulted_keys),
        "ignored_keys": ignored,
    }


def legacy_profile_default_name(path: Path) -> str:
    name = path.name
    if name.endswith(PROFILE_SUFFIX):
        return name[: -len(PROFILE_SUFFIX)]
    if name.endswith(".json"):
        return name[:-5]
    return path.stem


def load_3dplayer_display_settings(path: Path) -> dict[str, object]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"3DPlayer display settings must be a JSON object: {path}")

    display: dict[str, object] = {}
    supported_keys = set(THREEDPLAYER_DISPLAY_KEY_MAP)
    defaulted_keys: list[str] = []
    for legacy_key, (open3d_key, value_type) in THREEDPLAYER_DISPLAY_KEY_MAP.items():
        if legacy_key in payload:
            value = payload[legacy_key]
        else:
            defaulted_keys.append(legacy_key)
            value = THREEDPLAYER_DISPLAY_DEFAULTS[legacy_key]
        display[open3d_key] = coerce_legacy_value(value, value_type)

    if "add_n_bfi_frames_every_frame" in payload:
        supported_keys.add("add_n_bfi_frames_every_frame")
        bfi_value = payload["add_n_bfi_frames_every_frame"]
    else:
        defaulted_keys.append("add_n_bfi_frames_every_frame")
        bfi_value = THREEDPLAYER_DISPLAY_DEFAULTS["add_n_bfi_frames_every_frame"]
    bfi_frames = coerce_legacy_int(bfi_value)
    display["open3d-bfi-enable"] = bfi_frames > 0
    display["open3d-bfi-black-frames"] = max(1, bfi_frames) if bfi_frames > 0 else 1

    if any(key.startswith("whitebox_") or key == "blackbox_border" for key in payload):
        display.setdefault("open3d-trigger-enable", True)

    if "whitebox_corner_position" in payload:
        supported_keys.add("whitebox_corner_position")
        corner_value = payload["whitebox_corner_position"]
    else:
        defaulted_keys.append("whitebox_corner_position")
        corner_value = THREEDPLAYER_DISPLAY_DEFAULTS["whitebox_corner_position"]
    token = normalize_legacy_token(corner_value)
    if token not in LEGACY_CORNER_MAP:
        raise RuntimeError(f"unsupported legacy whitebox_corner_position: {corner_value!r}")
    display["open3d-trigger-corner"] = LEGACY_CORNER_MAP[token]

    return threedplayer_load_result(display, payload, supported_keys, defaulted_keys)


def import_legacy_display_settings(path: Path) -> dict[str, object]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"legacy display settings must be a JSON object: {path}")

    display: dict[str, object] = {}
    for legacy_key, (open3d_key, value_type) in LEGACY_DISPLAY_KEY_MAP.items():
        if legacy_key in payload:
            display[open3d_key] = coerce_legacy_value(payload[legacy_key], value_type)

    if "whitebox_corner_position" in payload:
        token = normalize_legacy_token(payload["whitebox_corner_position"])
        if token not in LEGACY_CORNER_MAP:
            raise RuntimeError(f"unsupported legacy whitebox_corner_position: {payload['whitebox_corner_position']!r}")
        display["open3d-trigger-corner"] = LEGACY_CORNER_MAP[token]

    if "pageflip_default_aspect" in payload:
        token = normalize_legacy_token(payload["pageflip_default_aspect"])
        if token not in LEGACY_ASPECT_MAP:
            raise RuntimeError(f"unsupported legacy pageflip_default_aspect: {payload['pageflip_default_aspect']!r}")
        display["open3d-default-half-layout"] = LEGACY_ASPECT_MAP[token]

    if any(key.startswith("whitebox_") or key == "blackbox_border" for key in payload):
        display.setdefault("open3d-trigger-enable", True)

    if not display:
        raise RuntimeError(f"no supported legacy display settings found in: {path}")
    return display


def to_3dplayer_display_settings(display: dict[str, object]) -> dict[str, object]:
    payload = dict(THREEDPLAYER_DISPLAY_DEFAULTS)
    if "open3d-target-flip-hz" in display:
        payload["target_framerate"] = format_legacy_number(display["open3d-target-flip-hz"])
    if parse_bool(display.get("open3d-bfi-enable", False)):
        payload["add_n_bfi_frames_every_frame"] = format_legacy_number(
            display.get("open3d-bfi-black-frames", "1")
        )
    else:
        payload["add_n_bfi_frames_every_frame"] = "0"
    if "open3d-trigger-brightness" in display:
        payload["whitebox_brightness"] = format_legacy_number(display["open3d-trigger-brightness"])
    if "open3d-trigger-corner" in display:
        payload["whitebox_corner_position"] = str(display["open3d-trigger-corner"]).replace("-", "_")
    if "open3d-trigger-offset-y" in display:
        payload["whitebox_vertical_position"] = format_legacy_number(display["open3d-trigger-offset-y"])
    if "open3d-trigger-offset-x" in display:
        payload["whitebox_horizontal_position"] = format_legacy_number(display["open3d-trigger-offset-x"])
    if "open3d-trigger-size" in display:
        payload["whitebox_size"] = format_legacy_number(display["open3d-trigger-size"])
    if "open3d-trigger-spacing" in display:
        payload["whitebox_horizontal_spacing"] = format_legacy_number(display["open3d-trigger-spacing"])
    if "open3d-trigger-black-border" in display:
        payload["blackbox_border"] = format_legacy_number(display["open3d-trigger-black-border"])
    if "open3d-calibration-enable" in display:
        payload["calibration_mode"] = parse_bool(display["open3d-calibration-enable"])
    return payload


def write_3dplayer_display_settings(path: Path, display: dict[str, object]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = to_3dplayer_display_settings(display)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return path


def load_3dplayer_emitter_settings(path: Path) -> dict[str, object]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"3DPlayer emitter settings must be a JSON object: {path}")

    emitter: dict[str, object] = {}
    supported_keys: set[str] = set()
    defaulted_keys: list[str] = []
    for threed_key, (open3d_key, value_type, aliases) in THREEDPLAYER_EMITTER_KEY_MAP.items():
        source_key = None
        for candidate in (threed_key, *aliases):
            supported_keys.add(candidate)
            if candidate in payload:
                source_key = candidate
                break
        if source_key is None:
            defaulted_keys.append(threed_key)
            value = THREEDPLAYER_EMITTER_DEFAULTS[threed_key]
        else:
            value = payload[source_key]
        if value_type == "drive_mode":
            emitter[open3d_key] = threedplayer_drive_mode_to_open3d(value)
        else:
            emitter[open3d_key] = coerce_legacy_int(value)

    return threedplayer_load_result(emitter, payload, supported_keys, defaulted_keys)


def to_3dplayer_emitter_settings(emitter: dict[str, object]) -> dict[str, object]:
    payload = dict(THREEDPLAYER_EMITTER_DEFAULTS)
    for threed_key, (open3d_key, value_type, _aliases) in THREEDPLAYER_EMITTER_KEY_MAP.items():
        if open3d_key not in emitter:
            continue
        value = emitter[open3d_key]
        if value_type == "drive_mode":
            payload[threed_key] = open3d_drive_mode_to_3dplayer(value)
        else:
            payload[threed_key] = format_legacy_number(value)
    return payload


def write_3dplayer_emitter_settings(path: Path, emitter: dict[str, object]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = to_3dplayer_emitter_settings(emitter)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return path


def save_display_profile(name: str, display: dict[str, object]) -> Path:
    path = profile_path(name)
    path.parent.mkdir(parents=True, exist_ok=True)
    persistent_display = {
        key: value for key, value in display.items() if key not in TRANSIENT_DISPLAY_KEYS
    }
    payload = {
        "schema": PROFILE_SCHEMA,
        "kind": "open3d-vlc-display-settings-profile",
        "saved_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "display": persistent_display,
    }
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    tmp_path.replace(path)
    return path


def load_display_profile(name: str) -> dict[str, object]:
    path = profile_path(name)
    payload = json.loads(path.read_text(encoding="utf-8"))
    display = payload.get("display")
    if not isinstance(display, dict):
        raise RuntimeError(f"profile has no display object: {path}")
    return {key: value for key, value in display.items() if key not in TRANSIENT_DISPLAY_KEYS}
