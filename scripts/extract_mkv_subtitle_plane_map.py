#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: extract_mkv_subtitle_plane_map.py <ffprobe.json>", file=sys.stderr)
        return 1

    ffprobe_path = Path(sys.argv[1])
    with ffprobe_path.open("r", encoding="utf-8") as fh:
        obj = json.load(fh)

    result = {
        "source": str(ffprobe_path),
        "subtitle_plane_map": [],
    }

    sub_track_index = 0
    for stream in obj.get("streams", []):
        if stream.get("codec_type") != "subtitle":
            continue

        tags = stream.get("tags") or {}
        plane_key = None
        plane_value = None
        for key, value in tags.items():
            if re.fullmatch(r"3d-plane(?:-[A-Za-z0-9_]+)?", key):
                plane_key = key
                plane_value = value
                break

        try:
            plane = int(plane_value) if plane_value is not None else None
        except ValueError:
            plane = None

        result["subtitle_plane_map"].append(
            {
                "sub_track_index": sub_track_index,
                "stream_index": stream.get("index"),
                "codec_name": stream.get("codec_name"),
                "language": tags.get("language"),
                "disposition": stream.get("disposition"),
                "source_id": tags.get("SOURCE_ID-eng") or tags.get("SOURCE_ID"),
                "plane_tag_key": plane_key,
                "plane": plane,
                "static_offset_units": plane,
            }
        )
        sub_track_index += 1

    json.dump(result, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
