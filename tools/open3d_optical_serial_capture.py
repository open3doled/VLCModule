#!/usr/bin/env python3
"""Capture Open3D emitter optical debug samples to VLC-compatible CSV."""

from __future__ import annotations

import argparse
import csv
import glob
import os
import sys
import time
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError as exc:  # pragma: no cover - environment diagnostic
    print(f"pyserial is required for optical capture: {exc}", file=sys.stderr)
    raise SystemExit(2)


HEADER = [
    "mono_us",
    "packet_type",
    "opt_current_time",
    "left_sensor",
    "right_sensor",
    "duplicate_frames_in_a_row_counter",
    "opt_block_signal_detection_until",
    "opt_readings_active",
    "opt_sensor_average_timing_mode_resync",
    "opt_sensor_frametime_average_updated",
    "opt_reading_triggered_left",
    "left_duplicate_detected",
    "left_duplicate_ignored",
    "left_sent_ir",
    "opt_reading_triggered_right",
    "right_duplicate_detected",
    "right_duplicate_ignored",
    "right_sent_ir",
    "raw_text",
    "raw_payload_hex",
]


def mono_us() -> int:
    return time.monotonic_ns() // 1000


def discover_port() -> str:
    ports = list(serial.tools.list_ports.comports())
    preferred = []
    for port in ports:
        text = " ".join(str(x) for x in (port.device, port.description, port.hwid)).lower()
        if any(token in text for token in ("arduino", "leonardo", "pro micro", "sparkfun", "open3d")):
            preferred.append(port.device)
    if preferred:
        return sorted(preferred)[0]

    candidates = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    if candidates:
        return candidates[0]
    raise RuntimeError("no serial port found; pass --port /dev/ttyACM0")


def decode_opt_line(line: bytes) -> list[object] | None:
    if len(line) < 13 or not line.startswith(b"+o "):
        return None
    raw = line[3:13]
    opt_current_time = (
        (raw[9] & 0x7F)
        | ((raw[8] & 0x7F) << 7)
        | ((raw[7] & 0x7F) << 14)
        | ((raw[6] & 0x7F) << 21)
        | ((raw[5] & 0x0F) << 28)
    )
    left_sensor = ((raw[5] & 0x70) >> 4) | ((raw[4] & 0x1F) << 3)
    right_sensor = ((raw[4] & 0x60) >> 5) | ((raw[3] & 0x3F) << 2)
    duplicate_frames_in_a_row_counter = raw[2] & 0x7F

    opt_duplicate_frame_left = (raw[0] & 0x20) != 0
    opt_ignore_duplicate_left = (raw[0] & 0x10) != 0
    opt_reading_triggered_left = (raw[0] & 0x08) != 0
    opt_duplicate_frame_right = (raw[0] & 0x04) != 0
    opt_ignore_duplicate_right = (raw[0] & 0x02) != 0
    opt_reading_triggered_right = (raw[0] & 0x01) != 0

    opt_sensor_average_timing_mode_resync = (raw[1] & 0x20) != 0
    opt_sensor_frametime_average_updated = (raw[1] & 0x10) != 0
    opt_readings_active = (raw[1] & 0x08) != 0
    opt_detected_signal_start_eye = (raw[1] & 0x04) != 0
    opt_initiated_sending_ir_signal = (raw[1] & 0x02) != 0
    opt_block_signal_detection_until = (raw[1] & 0x01) != 0

    left_sent_ir = opt_initiated_sending_ir_signal and opt_detected_signal_start_eye
    right_sent_ir = opt_initiated_sending_ir_signal and not opt_detected_signal_start_eye
    left_duplicate_detected = opt_reading_triggered_left and opt_duplicate_frame_left
    left_duplicate_ignored = left_duplicate_detected and opt_ignore_duplicate_left
    right_duplicate_detected = opt_reading_triggered_right and opt_duplicate_frame_right
    right_duplicate_ignored = right_duplicate_detected and opt_ignore_duplicate_right

    return [
        mono_us(),
        "opt",
        opt_current_time,
        left_sensor,
        right_sensor,
        duplicate_frames_in_a_row_counter,
        int(opt_block_signal_detection_until),
        int(opt_readings_active),
        int(opt_sensor_average_timing_mode_resync),
        int(opt_sensor_frametime_average_updated),
        int(opt_reading_triggered_left),
        int(left_duplicate_detected),
        int(left_duplicate_ignored),
        int(left_sent_ir),
        int(opt_reading_triggered_right),
        int(right_duplicate_detected),
        int(right_duplicate_ignored),
        int(right_sent_ir),
        "",
        raw.hex().upper(),
    ]


def raw_row(line: bytes) -> list[object]:
    text = line.decode("utf-8", errors="replace")
    return [mono_us(), "raw"] + [""] * 16 + [text, ""]


def event_row(text: str) -> list[object]:
    return [mono_us(), "event"] + [""] * 16 + [text, ""]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial port. Defaults to first likely /dev/ttyACM* emitter.")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--no-toggle", action="store_true", help="Do not send firmware command 10,1/10,0.")
    args = parser.parse_args(argv)

    port = args.port or discover_port()
    args.csv.parent.mkdir(parents=True, exist_ok=True)

    deadline = time.monotonic() + max(0.1, args.seconds)
    decoded = 0
    raw_count = 0
    with serial.Serial(port=port, baudrate=args.baud, timeout=0.05) as ser, args.csv.open("w", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(HEADER)
        writer.writerow(event_row(f"capture_start port={port} baud={args.baud} seconds={args.seconds:.3f}"))
        fp.flush()
        ser.reset_input_buffer()
        if not args.no_toggle:
            ser.write(b"10,1\n")
            ser.flush()

        try:
            while time.monotonic() < deadline:
                line = ser.readline().rstrip(b"\r\n")
                if not line:
                    continue
                row = decode_opt_line(line)
                if row is not None:
                    writer.writerow(row)
                    decoded += 1
                else:
                    writer.writerow(raw_row(line))
                    raw_count += 1
                if (decoded + raw_count) % 512 == 0:
                    fp.flush()
        finally:
            if not args.no_toggle:
                try:
                    ser.write(b"10,0\n")
                    ser.flush()
                except OSError:
                    pass
            writer.writerow(event_row(f"capture_stop decoded={decoded} raw={raw_count}"))
            fp.flush()

    print(f"optical capture wrote {args.csv} decoded={decoded} raw={raw_count}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
