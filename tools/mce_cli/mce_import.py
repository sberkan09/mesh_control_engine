#!/usr/bin/env python3
"""mce import: 2D authoring-tool exports into the engine's native text assets (M-IMPORT).

    mce import aseprite <export.json> [--out file.mcsheet] [--name n] [--werror]

aseprite: Aseprite's JSON export (File > Export Sprite Sheet, "JSON Data", array or hash frames, with
          "Tags" and, if wanted, "Slices" on) next to its PNG becomes a .mcsheet:

            frames          one region each, in export order (region index = frame index)
            tags            one clip each; direction forward / reverse / pingpong / pingpong_reverse
                            (PingPong mode); fps from the frame durations (mean, with a warning when they
                            differ); no tags: one clip "all" over every frame
            slices          one named region each (the first key's bounds)

          The PNG gets a texture sidecar when it has none; re-importing keeps the sheet's GUID. The output
          is a pure function of the inputs: same bytes in, same bytes out.

Unsupported features are warnings that name the feature (stderr); --werror turns them into a failure and
nothing is written.
"""

import argparse
import json
import os
import sys
import uuid

MAX_CLIP_NAME = 31
MAX_FPS = 240.0


class Importer:
    def __init__(self, tool):
        self.tool = tool
        self.warnings = []

    def warn(self, msg):
        self.warnings.append(msg)
        print(f"mce import {self.tool}: warning: {msg}", file=sys.stderr)


def read_json(path):
    with open(path, "rb") as f:
        return json.loads(f.read().decode("utf-8-sig"))


def existing_guid(path):
    """The GUID of the native asset already at `path`, or None."""
    try:
        g = read_json(path).get("guid")
        return g if isinstance(g, str) and g else None
    except (OSError, ValueError, AttributeError):
        return None


def frame_list(doc):
    fr = doc.get("frames")
    if isinstance(fr, dict):
        return list(fr.values())
    if isinstance(fr, list):
        return fr
    raise ValueError('no "frames" array or object (export with "JSON Data" on)')


def rect_of(o, what):
    try:
        return [int(o["x"]), int(o["y"]), int(o["w"]), int(o["h"])]
    except (KeyError, TypeError, ValueError):
        raise ValueError(f"{what} needs x, y, w and h") from None


def unique(name, taken, imp, what):
    base = name[:MAX_CLIP_NAME] if what == "clip" else name
    if len(base) != len(name):
        imp.warn(f"{what} name '{name}' is longer than {MAX_CLIP_NAME} characters; cut to '{base}'")
    out, n = base, 2
    while out in taken:
        suffix = f"_{n}"
        out = base[:MAX_CLIP_NAME - len(suffix)] + suffix if what == "clip" else base + suffix
        n += 1
    if out != base:
        imp.warn(f"two {what}s are named '{base}'; the second is '{out}'")
    taken.add(out)
    return out


def convert_aseprite(doc, sheet_name, texture_guid, sheet_guid, imp):
    meta = doc.get("meta") or {}
    frames = frame_list(doc)
    if not frames:
        raise ValueError("the export has no frames")
    size = meta.get("size") or {}
    iw, ih = int(size.get("w", 0)), int(size.get("h", 0))
    if iw < 1 or ih < 1:
        raise ValueError('meta.size is missing (export with "JSON Data" on)')

    regions, durations = [], []
    trimmed = rotated = False
    for i, f in enumerate(frames):
        r = rect_of(f.get("frame") or {}, f"frame {i}")
        if r[2] < 1 or r[3] < 1 or r[0] < 0 or r[1] < 0 or r[0] + r[2] > iw or r[1] + r[3] > ih:
            raise ValueError(f"frame {i} lies outside the {iw}x{ih} image")
        regions.append({"rect": r})
        durations.append(int(f.get("duration", 100)) or 100)
        trimmed = trimmed or bool(f.get("trimmed"))
        rotated = rotated or bool(f.get("rotated"))
    if trimmed:
        imp.warn("trimmed frames: the empty border Aseprite cut off is not restored, so the sprite's "
                 "pivot shifts; export with Trim off")
    if rotated:
        imp.warn("rotated frames are not supported; the regions are the packed (rotated) rectangles")

    taken = set()
    slices = meta.get("slices") or []
    for s in slices:
        keys = s.get("keys") or []
        if not keys:
            imp.warn(f"slice '{s.get('name', '?')}' has no keys; skipped")
            continue
        b = rect_of(keys[0].get("bounds") or {}, f"slice '{s.get('name', '?')}'")
        if len({json.dumps(k.get("bounds"), sort_keys=True) for k in keys}) > 1:
            imp.warn(f"slice '{s['name']}' changes over time; the first key's bounds are used")
        if b[2] < 1 or b[3] < 1 or b[0] < 0 or b[1] < 0 or b[0] + b[2] > iw or b[1] + b[3] > ih:
            imp.warn(f"slice '{s['name']}' lies outside the image; skipped")
            continue
        regions.append({"name": unique(str(s["name"]), taken, imp, "slice"), "rect": b})

    clips = []
    names = set()
    tags = meta.get("frameTags") or []
    if not tags:
        tags = [{"name": "all", "from": 0, "to": len(frames) - 1, "direction": "forward"}]
    for t in tags:
        a, b = int(t.get("from", 0)), int(t.get("to", 0))
        if not (0 <= a < len(frames) and 0 <= b < len(frames)):
            raise ValueError(f"tag '{t.get('name', '?')}' names frames outside 0..{len(frames) - 1}")
        idx = list(range(a, b + 1)) if a <= b else list(range(a, b - 1, -1))
        direction = t.get("direction", "forward")
        mode = "Loop"
        if direction == "reverse":
            idx.reverse()
        elif direction == "pingpong":
            mode = "PingPong"
        elif direction == "pingpong_reverse":
            idx.reverse()
            mode = "PingPong"
        elif direction != "forward":
            imp.warn(f"tag '{t['name']}' has direction '{direction}', which is not supported; played forward")
        d = [durations[i] for i in idx]
        mean = sum(d) / len(d)
        if len(set(d)) > 1:
            imp.warn(f"tag '{t['name']}': frame durations differ ({min(d)}..{max(d)} ms); the clip plays at "
                     f"the mean, {round(1000 / mean, 3)} fps")
        fps = round(1000.0 / mean, 3)
        if fps > MAX_FPS:
            imp.warn(f"tag '{t['name']}' would play at {fps} fps; limited to {int(MAX_FPS)}")
            fps = MAX_FPS
        clip = {"name": unique(str(t["name"]), names, imp, "clip"), "frames": idx, "fps": fps}
        if mode != "Loop":
            clip["mode"] = mode
        clips.append(clip)
    if meta.get("layers") and any(l.get("blendMode", "normal") != "normal" for l in meta["layers"]):
        imp.warn("layer blend modes are flattened by Aseprite's export; nothing to do here")

    return {
        "format": "mce-spritesheet", "version": 1, "guid": sheet_guid, "name": sheet_name,
        "texture": f"guid:{texture_guid}", "image_size": [iw, ih],
        "regions": regions, "clips": clips,
    }


def cmd_aseprite(args):
    imp = Importer("aseprite")
    src = args.file
    try:
        doc = read_json(src)
        meta = doc.get("meta") or {}
        image = meta.get("image")
        if not image:
            raise ValueError("meta.image is missing: the export must name its PNG")
        png = os.path.normpath(os.path.join(os.path.dirname(src) or ".", image))
        if not os.path.isfile(png):
            raise ValueError(f"the exported image '{image}' is not next to the JSON ({png})")
        out = args.out or os.path.splitext(src)[0] + ".mcsheet"
        name = args.name or os.path.splitext(os.path.basename(out))[0]
        sheet_guid = existing_guid(out) or str(uuid.uuid4())
        # Probe the texture GUID without writing anything yet: --werror must leave the project untouched.
        side = png + ".mceasset"
        tex_guid = existing_guid(side) or str(uuid.uuid4())
        sheet = convert_aseprite(doc, name, tex_guid, sheet_guid, imp)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
        print(f"mce import aseprite: {src}: {e}", file=sys.stderr)
        return 1
    if imp.warnings and args.werror:
        print("mce import aseprite: --werror: nothing written", file=sys.stderr)
        return 1
    if not os.path.isfile(side):
        doc_side = {"guid": tex_guid, "importer": "texture", "importer_version": 1,
                    "settings": {"srgb": True, "mips": False, "filter": "nearest"}}
        with open(side, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(doc_side, indent=2) + "\n")
        print(f"wrote {side}")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(sheet, indent=2) + "\n")
    print(f"wrote {out} ({len(sheet['regions'])} regions, {len(sheet['clips'])} clips)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mce import", description="2D authoring-tool exports into native assets")
    sub = ap.add_subparsers(dest="tool", metavar="<tool>", required=True)
    s = sub.add_parser("aseprite", help="Aseprite JSON export + PNG into a .mcsheet")
    s.add_argument("file", help="the exported .json")
    s.add_argument("--out", help="the .mcsheet to write (default: beside the export)")
    s.add_argument("--name", help="the sheet's name (default: the file name)")
    s.add_argument("--werror", action="store_true", help="a warning is a failure; nothing is written")
    s.set_defaults(fn=cmd_aseprite)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
