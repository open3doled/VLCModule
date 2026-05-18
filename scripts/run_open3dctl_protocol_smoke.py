#!/usr/bin/env python3
"""Smoke-test open3dctl protocol behavior with a fake Unix socket server."""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from typing import Any

from open3d_control_common import (
    load_3dplayer_display_settings,
    load_3dplayer_emitter_settings,
    to_3dplayer_display_settings,
    to_3dplayer_emitter_settings,
)


INITIAL_DISPLAY: dict[str, Any] = {
    "open3d-enable": True,
    "open3d-layout": "auto",
    "open3d-default-half-layout": "sbs",
    "open3d-flip-eyes": False,
    "open3d-target-flip-hz": 0.0,
    "open3d-presenter-hz": 120.0,
    "open3d-gpu-overlay-enable": True,
    "open3d-bfi-enable": False,
    "open3d-bfi-visible-frames": 1,
    "open3d-bfi-black-frames": 1,
    "open3d-trigger-enable": True,
    "open3d-trigger-size": 24,
    "open3d-trigger-padding": 8,
    "open3d-trigger-spacing": 4,
    "open3d-trigger-corner": "top-left",
    "open3d-trigger-offset-x": 0,
    "open3d-trigger-offset-y": 0,
    "open3d-trigger-alpha": 1.0,
    "open3d-trigger-brightness": 255,
    "open3d-trigger-black-border": 0,
    "open3d-trigger-invert": False,
    "open3d-calibration-enable": False,
    "open3d-status-osd-enable": True,
    "open3d-status-osd-duration-ms": 1500,
    "open3d-status-help-duration-ms": 8000,
}

INITIAL_EMITTER: dict[str, Any] = {
    "open3d-emitter-enable": False,
    "open3d-emitter-tty": "auto",
    "open3d-emitter-baud": 115200,
    "open3d-emitter-auto-reconnect": True,
    "open3d-emitter-reconnect-ms": 1000,
    "open3d-emitter-log-io": False,
    "open3d-emitter-read-on-connect": True,
    "open3d-emitter-apply-on-connect": False,
    "open3d-emitter-save-on-apply": False,
    "selected_tty": "",
    "connected": False,
    "dirty": False,
    "ir_protocol": 0,
    "ir_frame_delay": 0,
    "ir_frame_duration": 0,
    "ir_signal_spacing": 0,
    "opt_block_signal_detection_delay": 0,
    "opt_min_threshold_value_to_activate": 0,
    "opt_detection_threshold_high": 0,
    "opt_detection_threshold_low": 0,
    "opt_enable_ignore_during_ir": 0,
    "opt_enable_duplicate_realtime_reporting": 0,
    "opt_output_stats": 0,
    "opt_ignore_all_duplicates": 0,
    "opt_sensor_filter_mode": 0,
    "ir_flip_eyes": 0,
    "ir_average_timing_mode": 0,
    "target_frametime": 8333,
    "ir_drive_mode": 0,
}

EMITTER_STATE_KEY_MAP = {
    "open3d-trigger-drive-mode": "ir_drive_mode",
    "open3d-emitter-ir-protocol": "ir_protocol",
    "open3d-emitter-ir-frame-delay": "ir_frame_delay",
    "open3d-emitter-ir-frame-duration": "ir_frame_duration",
    "open3d-emitter-ir-signal-spacing": "ir_signal_spacing",
    "open3d-emitter-target-frametime": "target_frametime",
    "open3d-emitter-ir-flip-eyes": "ir_flip_eyes",
    "open3d-emitter-ir-avg-timing": "ir_average_timing_mode",
    "open3d-emitter-opt-block-delay": "opt_block_signal_detection_delay",
    "open3d-emitter-opt-min-threshold": "opt_min_threshold_value_to_activate",
    "open3d-emitter-opt-threshold-high": "opt_detection_threshold_high",
    "open3d-emitter-opt-threshold-low": "opt_detection_threshold_low",
    "open3d-emitter-opt-ignore-during-ir": "opt_enable_ignore_during_ir",
    "open3d-emitter-opt-dup-realtime": "opt_enable_duplicate_realtime_reporting",
    "open3d-emitter-opt-output-stats": "opt_output_stats",
    "open3d-emitter-opt-ignore-duplicates": "opt_ignore_all_duplicates",
    "open3d-emitter-opt-sensor-filter": "opt_sensor_filter_mode",
}


class FakeOpen3DServer:
    def __init__(self, socket_path: Path) -> None:
        self.socket_path = socket_path
        self.display = dict(INITIAL_DISPLAY)
        self.emitter = dict(INITIAL_EMITTER)
        self.stop = threading.Event()
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self) -> None:
        self.thread.start()
        if not self.ready.wait(timeout=2.0):
            raise RuntimeError("fake server did not start")

    def close(self) -> None:
        self.stop.set()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.connect(str(self.socket_path))
        except OSError:
            pass
        self.thread.join(timeout=2.0)

    def run(self) -> None:
        self.socket_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            self.socket_path.unlink()
        except FileNotFoundError:
            pass
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(str(self.socket_path))
            server.listen(8)
            self.ready.set()
            while not self.stop.is_set():
                try:
                    conn, _addr = server.accept()
                except OSError:
                    if self.stop.is_set():
                        break
                    raise
                with conn:
                    request = conn.recv(4096).decode("utf-8", errors="replace").strip()
                    if not request:
                        continue
                    response = self.handle(request)
                    conn.sendall(json.dumps(response, separators=(",", ":")).encode("utf-8") + b"\n")

    def state(self) -> dict[str, Any]:
        return {
            "ok": True,
            "pid": os.getpid(),
            "instance": 1,
            "socket": str(self.socket_path),
            "display": dict(self.display),
            "local_emitter": dict(self.emitter),
        }

    def handle(self, request: str) -> dict[str, Any]:
        if request in ("state", "get_state"):
            return self.state()
        payload = json.loads(request)
        cmd = payload.get("cmd") or payload.get("command")
        if cmd == "get_state":
            return self.state()
        key = str(payload.get("key", ""))
        value = payload.get("value")
        if cmd in ("set", "set_display"):
            self.display[key] = self.coerce_like(self.display.get(key), value)
            return {"ok": True, "queued": True}
        if cmd in ("set_local_emitter", "set_emitter"):
            state_key = EMITTER_STATE_KEY_MAP.get(key, key)
            self.emitter[state_key] = self.coerce_like(self.emitter.get(state_key), value)
            return {"ok": True, "queued": True}
        return {"ok": True, "queued": True}

    @staticmethod
    def coerce_like(actual: Any, value: Any) -> Any:
        if isinstance(actual, bool):
            return str(value).strip().lower() in ("1", "true", "yes", "on")
        if isinstance(actual, int) and not isinstance(actual, bool):
            if str(value).strip().lower() == "serial":
                return 1
            if str(value).strip().lower() == "optical":
                return 0
            return int(float(str(value).strip()))
        if isinstance(actual, float):
            return float(str(value).strip())
        return str(value)


def run_cmd(argv: list[str], env: dict[str, str]) -> dict[str, Any]:
    result = subprocess.run(
        argv,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return json.loads(result.stdout)


def run_settings_json_smoke(root: Path) -> None:
    display_path = root / "legacy_3dplayer.display_settings.json"
    display_path.write_text(
        json.dumps(
            {
                "target_framerate": "120",
                "add_n_bfi_frames_every_frame": "1",
                "display_resolution": "1920x1080",
                "whitebox_brightness": "200",
                "whitebox_corner_position": "bottom_right",
                "whitebox_vertical_position": "-5",
                "whitebox_horizontal_position": "7",
                "whitebox_size": "15",
                "whitebox_horizontal_spacing": "18",
                "blackbox_border": "10",
                "calibration_mode": True,
                "future_display_key": "ignored",
            }
        ),
        encoding="utf-8",
    )
    display_result = load_3dplayer_display_settings(display_path)
    display = display_result["values"]
    assert isinstance(display, dict)
    assert abs(display["open3d-target-flip-hz"] - 120.0) < 0.001
    assert display["open3d-bfi-enable"] is True
    assert display["open3d-bfi-black-frames"] == 1
    assert display["open3d-trigger-enable"] is True
    assert display["open3d-trigger-corner"] == "bottom-right"
    assert "future_display_key" in display_result["ignored_keys"]

    display_export = to_3dplayer_display_settings(display)
    assert display_export["target_framerate"] == "120"
    assert display_export["add_n_bfi_frames_every_frame"] == "1"
    assert display_export["whitebox_corner_position"] == "bottom_right"
    assert display_export["display_size"] == "55"

    emitter_path = root / "legacy_3dplayer.emitter_settings.json"
    emitter_path.write_text(
        json.dumps(
            {
                "ir_drive_mode": "0",
                "ir_protocol": "6",
                "ir_frame_delay": "300",
                "ir_frame_duration": "7000",
                "ir_signal_spacing": "30",
                "ir_flip_eyes": "0",
                "ir_average_timing_mode": "1",
                "target_frametime": "8333",
                "opt_block_signal_detection_delay": "7500",
                "opt_ignore_all_duplicates": "1",
                "opt_sensor_filter_mode_variable": "0",
                "opt_min_threshold_value_to_activate": "10",
                "opt_detection_threshold_high": "128",
                "opt_detection_threshold_low": "32",
                "opt_enable_ignore_during_ir": "0",
                "opt_enable_duplicate_realtime_reporting": "0",
                "opt_output_stats": "0",
                "future_emitter_key": "ignored",
            }
        ),
        encoding="utf-8",
    )
    emitter_result = load_3dplayer_emitter_settings(emitter_path)
    emitter = emitter_result["values"]
    assert isinstance(emitter, dict)
    assert emitter["open3d-trigger-drive-mode"] == "optical"
    assert emitter["open3d-emitter-ir-protocol"] == 6
    assert emitter["open3d-emitter-ir-frame-delay"] == 300
    assert emitter["open3d-emitter-target-frametime"] == 8333
    assert "future_emitter_key" in emitter_result["ignored_keys"]

    emitter_export = to_3dplayer_emitter_settings(
        {
            **emitter,
            "open3d-trigger-drive-mode": "serial",
            "open3d-emitter-opt-sensor-filter": 3,
        }
    )
    assert emitter_export["ir_drive_mode"] == "1"
    assert emitter_export["opt_sensor_filter_mode_variable"] == "3"
    assert emitter_export["target_frametime"] == "8333"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--client",
        default=str(Path(__file__).resolve().parent / "open3dctl"),
        help="open3dctl executable to test.",
    )
    parser.add_argument(
        "--client-arg",
        action="append",
        default=[],
        help="Extra argument inserted immediately after --client, for example --client-arg=--open3dctl.",
    )
    args = parser.parse_args(argv)
    client_prefix = [args.client, *args.client_arg]

    with tempfile.TemporaryDirectory(prefix="open3dctl-protocol-") as tmp:
        root = Path(tmp)
        runtime_dir = root / "runtime"
        home_dir = root / "home"
        socket_path = runtime_dir / "open3doled" / "vlc" / "open3d-vlc-1.sock"
        runtime_dir.mkdir(parents=True)
        home_dir.mkdir()
        os.chmod(runtime_dir, 0o700)
        run_settings_json_smoke(root)

        server = FakeOpen3DServer(socket_path)
        server.start()
        env = dict(os.environ)
        env["HOME"] = str(home_dir)
        env["XDG_RUNTIME_DIR"] = str(runtime_dir)
        try:
            run_cmd(
                [
                    *client_prefix,
                    "--wait-applied",
                    "set-display",
                    "open3d-target-flip-hz",
                    "120",
                ],
                env,
            )
            state = run_cmd([*client_prefix, "state"], env)
            assert abs(state["display"]["open3d-target-flip-hz"] - 120.0) < 0.001

            run_cmd(
                [
                    *client_prefix,
                    "--wait-applied",
                    "preset",
                    "240hz-120flip",
                ],
                env,
            )
            state = run_cmd([*client_prefix, "state"], env)
            assert abs(state["display"]["open3d-presenter-hz"] - 240.0) < 0.001
            assert abs(state["display"]["open3d-target-flip-hz"] - 120.0) < 0.001
            assert state["local_emitter"]["target_frametime"] == 8333

            profile_save = run_cmd([*client_prefix, "profile-save", "smoke 240/120"], env)
            assert profile_save["profile"] == "smoke_240_120"
            saved_profile = Path(profile_save["path"])
            saved_payload = json.loads(saved_profile.read_text(encoding="utf-8"))
            assert "open3d-calibration-enable" not in saved_payload["display"]

            run_cmd(
                [
                    *client_prefix,
                    "--wait-applied",
                    "set-display",
                    "open3d-target-flip-hz",
                    "0",
                ],
                env,
            )
            state = run_cmd([*client_prefix, "state"], env)
            assert abs(state["display"]["open3d-target-flip-hz"] - 0.0) < 0.001

            profile_load = run_cmd(
                [
                    *client_prefix,
                    "--wait-applied",
                    "profile-load",
                    "smoke_240_120",
                ],
                env,
            )
            assert profile_load["applied"] is True
            state = run_cmd([*client_prefix, "state"], env)
            assert abs(state["display"]["open3d-presenter-hz"] - 240.0) < 0.001
            assert abs(state["display"]["open3d-target-flip-hz"] - 120.0) < 0.001

            run_cmd(
                [
                    *client_prefix,
                    "--wait-applied",
                    "preset",
                    "240hz-120flip-bfi",
                ],
                env,
            )
            state = run_cmd([*client_prefix, "state"], env)
            assert abs(state["display"]["open3d-presenter-hz"] - 240.0) < 0.001
            assert abs(state["display"]["open3d-target-flip-hz"] - 120.0) < 0.001
            assert state["display"]["open3d-bfi-enable"] is True
            assert state["display"]["open3d-bfi-visible-frames"] == 1
            assert state["display"]["open3d-bfi-black-frames"] == 1
            assert state["local_emitter"]["target_frametime"] == 8333

            legacy_path = root / "legacy.display_settings.json"
            legacy_path.write_text(
                json.dumps(
                    {
                        "target_framerate": "120",
                        "pageflip_enabled": True,
                        "pageflip_default_aspect": "top-and-bottom",
                        "pageflip_flip_eyes": True,
                        "pageflip_show_overlay": False,
                        "whitebox_brightness": "200",
                        "whitebox_corner_position": "bottom_right",
                        "whitebox_vertical_position": "-5",
                        "whitebox_horizontal_position": "7",
                        "whitebox_size": "15",
                        "whitebox_horizontal_spacing": "18",
                        "blackbox_border": "10",
                        "calibration_mode": True,
                    }
                ),
                encoding="utf-8",
            )
            imported = run_cmd(
                [
                    *client_prefix,
                    "--wait-applied",
                    "profile-import",
                    str(legacy_path),
                    "--name",
                    "legacy test",
                    "--presenter-hz",
                    "240",
                    "--apply",
                ],
                env,
            )
            assert imported["profile"] == "legacy_test"
            assert imported["queued"] >= 12
            assert imported["applied"] is True
            state = run_cmd([*client_prefix, "state"], env)
            display = state["display"]
            assert abs(display["open3d-target-flip-hz"] - 120.0) < 0.001
            assert abs(display["open3d-presenter-hz"] - 240.0) < 0.001
            assert display["open3d-default-half-layout"] == "tb"
            assert display["open3d-flip-eyes"] is True
            assert display["open3d-status-osd-enable"] is False
            assert display["open3d-trigger-enable"] is True
            assert display["open3d-trigger-brightness"] == 200
            assert display["open3d-trigger-corner"] == "bottom-right"
            assert display["open3d-trigger-offset-y"] == -5
            assert display["open3d-trigger-offset-x"] == 7
            assert display["open3d-trigger-size"] == 15
            assert display["open3d-trigger-spacing"] == 18
            assert display["open3d-trigger-black-border"] == 10
            assert display["open3d-calibration-enable"] is False
            imported_profile = Path(imported["path"])
            imported_payload = json.loads(imported_profile.read_text(encoding="utf-8"))
            assert "open3d-calibration-enable" not in imported_payload["display"]

            run_cmd(
                [
                    *client_prefix,
                    "--wait-applied",
                    "set-emitter",
                    "open3d-trigger-drive-mode",
                    "serial",
                ],
                env,
            )
            state = run_cmd([*client_prefix, "state"], env)
            assert state["local_emitter"]["ir_drive_mode"] == 1
        finally:
            server.close()

    print(json.dumps({"ok": True, "client": client_prefix}, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
