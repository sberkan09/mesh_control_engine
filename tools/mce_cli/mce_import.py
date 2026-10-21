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

tiled:    a Tiled map exported as JSON (.tmj; tilesets embedded or external .tsj) becomes
            <name>.mcscene plus one <name>_<layer>.mctilemap per tile layer and tileset:
              tile layers     rows flipped to the engine's bottom-up order, ids kept; CSV and base64
                              (raw, zlib, gzip) data; opacity as tint; hidden layers skipped
              collision       a layer property collide=true, or tileset tiles with a bool property
                              solid=true, become static boxes (neighbouring tiles merged)
              object layers   rectangles are static boxes (bool properties sensor, one_way); points are
                              marker entities; other shapes warn
            The tileset images get texture sidecars; re-importing keeps every GUID. One metre per tile
            unless --tile-size says otherwise. Infinite, isometric and hex maps, margins and spacing in a
            tileset and XML tilesets are errors with the way out; flips and animated tiles are warnings.

ldtk:     an LDtk project (.ldtk; external level files are followed) becomes, per level, <name>_<level>.mcscene
          plus one <name>_<level>_<layer>.mctilemap per Tiles / AutoLayer / IntGrid-with-tiles layer:
            tile layers     tile id = column + row * tileset columns + 1; layer order and opacity kept
            IntGrid         every non-zero cell is solid; neighbouring cells merge into static boxes
            entity layers   every entity is a named marker entity at its pivot; tagged 'solid' or 'sensor'
                            it is a static box (sensor: a trigger) about its rectangle
          --level L imports one level. Padding and spacing, a layer grid that differs from its tileset's
          are errors; flipped tiles, stacked tiles, entity fields and hidden layers are warnings.

gltf, fbx: a 3D model's lights and cameras become <name>.mcprefab (a root with MeshRef to the model, a child per
          PointLight3D / SpotLight3D / Camera3DComponent at the node's world transform; directional lights are a
          warning naming Environment3D.sun_direction). Implemented in mce_import_3d.py; see its docstring and the
          user guide, section 8.4, for the light-unit formulas. fbx needs mce_cook (--cook, MCE_COOK or beside).

Unsupported features are warnings that name the feature (stderr); --werror turns them into a failure and
nothing is written.
"""

import argparse
import json
import os
import re
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


def dumps(doc):
    return json.dumps(doc, indent=2)


def write_json(path, doc):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(dumps(doc) + "\n")


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
            f.write(dumps(doc_side) + "\n")
        print(f"wrote {side}")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(dumps(sheet) + "\n")
    print(f"wrote {out} ({len(sheet['regions'])} regions, {len(sheet['clips'])} clips)")
    return 0


# ---- Tiled ------------------------------------------------------------------------------------------

FLIP_MASK = 0xF0000000  # horizontal, vertical, diagonal and hexagonal-rotation flags in a tile id
GID_MASK = 0x0FFFFFFF


def safe_name(s):
    out = "".join(c if c.isalnum() or c in "-_" else "_" for c in str(s)).strip("_")
    return out or "layer"


def props_of(o):
    """A Tiled object's or layer's custom properties as {name: value} (both the 1.9+ list and the old dict)."""
    p = o.get("properties")
    if isinstance(p, list):
        return {x.get("name"): x.get("value") for x in p if isinstance(x, dict)}
    return p if isinstance(p, dict) else {}


def decode_tile_data(layer, imp):
    """A tile layer's cells as a flat list of u32 gids (row 0 first, as Tiled stores them)."""
    data = layer.get("data")
    w, h = int(layer.get("width", 0)), int(layer.get("height", 0))
    if isinstance(data, list):
        gids = [int(v) & 0xFFFFFFFF for v in data]
    elif isinstance(data, str):
        import base64
        import struct
        import zlib
        raw = base64.b64decode(data)
        comp = layer.get("compression", "")
        if comp == "zlib":
            raw = zlib.decompress(raw)
        elif comp == "gzip":
            raw = zlib.decompress(raw, 16 + zlib.MAX_WBITS)
        elif comp:
            raise ValueError(f"layer '{layer.get('name')}': '{comp}' compression is not supported; "
                             "use CSV, or base64 with zlib or gzip (Map properties > Tile Layer Format)")
        gids = list(struct.unpack(f"<{len(raw) // 4}I", raw[:len(raw) // 4 * 4]))
    else:
        raise ValueError(f"layer '{layer.get('name')}' has no tile data")
    if len(gids) != w * h:
        raise ValueError(f"layer '{layer.get('name')}' has {len(gids)} cells, expected {w}x{h}")
    return gids


def load_tilesets(doc, base_dir, imp):
    """[{firstgid, name, image(abs path), columns, rows, tilecount, solid(set of local ids), tw, th}]"""
    out = []
    for ts in doc.get("tilesets") or []:
        first = int(ts.get("firstgid", 1))
        d = base_dir
        if "source" in ts:
            src = os.path.normpath(os.path.join(base_dir, ts["source"]))
            if src.lower().endswith(".tsx"):
                raise ValueError(f"tileset '{ts['source']}' is XML (.tsx); in Tiled use File > Export As > JSON "
                                 "(.tsj) or embed the tileset in the map")
            ts = read_json(src)
            d = os.path.dirname(src)
        name = str(ts.get("name", "tileset"))
        if not ts.get("image"):
            imp.warn(f"tileset '{name}' is a collection of images, which is not supported; its tiles are skipped")
            continue
        tw, th = int(ts["tilewidth"]), int(ts["tileheight"])
        cols, count = int(ts["columns"]), int(ts["tilecount"])
        if int(ts.get("margin", 0)) or int(ts.get("spacing", 0)):
            raise ValueError(f"tileset '{name}' has margin or spacing; the engine cuts a tileset into equal "
                             "cells with none. Re-export the tileset image without them")
        rows = (count + cols - 1) // cols
        iw, ih = int(ts.get("imagewidth", cols * tw)), int(ts.get("imageheight", rows * th))
        if iw != cols * tw or ih != rows * th:
            raise ValueError(f"tileset '{name}': the {iw}x{ih} image is not {cols}x{rows} cells of {tw}x{th}")
        solid, animated = set(), False
        for t in ts.get("tiles") or []:
            if props_of(t).get("solid") is True:
                solid.add(int(t["id"]))
            animated = animated or bool(t.get("animation"))
        if animated:
            imp.warn(f"tileset '{name}' has animated tiles; they draw as their first frame")
        image = os.path.normpath(os.path.join(d, ts["image"]))
        if not os.path.isfile(image):
            raise ValueError(f"tileset '{name}': the image '{ts['image']}' was not found ({image})")
        out.append({"firstgid": first, "name": safe_name(name), "image": image, "columns": cols, "rows": rows,
                    "count": count, "solid": solid, "tw": tw, "th": th})
    out.sort(key=lambda t: t["firstgid"])
    return out


def tileset_of(gid, tilesets):
    for t in reversed(tilesets):
        if gid >= t["firstgid"]:
            return t if gid < t["firstgid"] + t["count"] else None
    return None


def merge_rects(cells, w, h):
    """Greedy rectangles over a set of (col, row) cells: row runs merged downward. Returns [(c, r, cw, rh)]."""
    cellset = set(cells)
    used = set()
    out = []
    for r in range(h):
        c = 0
        while c < w:
            if (c, r) in cellset and (c, r) not in used:
                c2 = c
                while c2 + 1 < w and (c2 + 1, r) in cellset and (c2 + 1, r) not in used:
                    c2 += 1
                r2 = r
                while r2 + 1 < h and all((x, r2 + 1) in cellset and (x, r2 + 1) not in used for x in range(c, c2 + 1)):
                    r2 += 1
                for rr in range(r, r2 + 1):
                    for x in range(c, c2 + 1):
                        used.add((x, rr))
                out.append((c, r, c2 - c + 1, r2 - r + 1))
                c = c2 + 1
            else:
                c += 1
    return out


def num(v):
    """A JSON number without float noise."""
    v = round(float(v), 4)
    return int(v) if v == int(v) else v


def convert_tiled(doc, map_dir, name, out_dir, existing, tile_size, imp):
    """{file name: JSON doc}: the scene and one tilemap per layer and tileset. Pure; GUIDs come from `existing`.
    A tilemap's tileset and a layer entity's map are placeholders (("png", path), ("map", file)) the caller resolves."""
    if doc.get("infinite"):
        raise ValueError("infinite maps are not supported; in Tiled turn off Infinite (Map properties)")
    if doc.get("orientation", "orthogonal") != "orthogonal":
        raise ValueError(f"'{doc.get('orientation')}' maps are not supported (orthogonal only)")
    mw, mh = int(doc["width"]), int(doc["height"])
    tw, th = int(doc["tilewidth"]), int(doc["tileheight"])
    tilesets = load_tilesets(doc, map_dir, imp)
    sx, sy = tile_size / tw, tile_size / th  # metres per Tiled pixel
    files = {}
    entities = []

    def new_id():
        return len(entities) + 1

    def guid_of(fname):
        return existing.get(fname) or str(uuid.uuid4())

    def add_collider_boxes(label, rects, sensor=False, one_way=False):
        for i, (c, r, cw, rh) in enumerate(rects):
            col = {"shape": {"half": [num(cw * tile_size / 2), num(rh * tile_size / 2)]}, "density": 0}
            if sensor:
                col["is_sensor"] = True
            if one_way:
                col["one_way"] = True
            entities.append({"id": new_id(), "name": f"{label} {i}", "components": {
                "Collider2D": col,
                "LocalTransform": {"position": [num((c + cw / 2) * tile_size), num((mh - r - rh / 2) * tile_size), 0]},
                "RigidBody2D": {}}})

    layer_n = 0

    def walk(layers, prefix=""):
        nonlocal layer_n
        for lyr in layers:
            kind = lyr.get("type")
            lname = prefix + str(lyr.get("name", "layer"))
            if kind == "group":
                walk(lyr.get("layers") or [], lname + "/")
                continue
            props = props_of(lyr)
            ox, oy = float(lyr.get("offsetx", 0)), float(lyr.get("offsety", 0))
            if kind == "tilelayer":
                layer_n += 1
                gids = decode_tile_data(lyr, imp)
                flipped = any(g & FLIP_MASK for g in gids)
                if flipped:
                    imp.warn(f"layer '{lname}' has flipped or rotated tiles; they draw unflipped")
                groups = {}
                for i, g in enumerate(gids):
                    g &= GID_MASK
                    if g == 0:
                        continue
                    ts = tileset_of(g, tilesets)
                    if ts is None:
                        imp.warn(f"layer '{lname}' uses tile id {g}, which no tileset holds; skipped")
                        continue
                    groups.setdefault(ts["firstgid"], (ts, []))[1].append((i % mw, i // mw, g - ts["firstgid"]))
                collide_all = props.get("collide") is True
                solid_cells = [(c, r) for ts, cells in groups.values() for c, r, lid in cells
                               if collide_all or lid in ts["solid"]]
                visible = lyr.get("visible", True)
                if not visible and not solid_cells:
                    imp.warn(f"layer '{lname}' is hidden and has no collision; skipped")
                    continue
                if visible:
                    for ts, cells in sorted(groups.values(), key=lambda g: g[0]["firstgid"]):
                        suffix = "" if len(groups) == 1 else "_" + ts["name"]
                        fname = f"{name}_{safe_name(lname)}{suffix}.mctilemap"
                        tiles = [0] * (mw * mh)
                        for c, r, lid in cells:
                            tiles[(mh - 1 - r) * mw + c] = lid + 1  # the engine's row 0 is the bottom
                        files[fname] = {"format": "mce-tilemap", "version": 1, "guid": guid_of(fname),
                                        "name": fname[:-len(".mctilemap")], "tileset": ("png", ts["image"]),
                                        "columns": ts["columns"], "rows": ts["rows"], "width": mw, "height": mh,
                                        "tile_size": num(tile_size), "tiles": tiles}
                        comp = {"map": ("map", fname), "order": layer_n}
                        if float(lyr.get("opacity", 1)) != 1:
                            comp["tint"] = [1, 1, 1, num(lyr["opacity"])]
                        entities.append({"id": new_id(), "name": lname + suffix, "components": {
                            "LocalTransform": {"position": [num(ox * sx), num(-oy * sy), 0]},
                            "TilemapLayer": comp}})
                if solid_cells:
                    add_collider_boxes(f"{lname} collision", merge_rects(solid_cells, mw, mh))
            elif kind == "objectgroup":
                for o in lyr.get("objects") or []:
                    oname = str(o.get("name") or f"{lname}_{o.get('id')}")
                    p = props_of(o)
                    x, y = float(o.get("x", 0)), float(o.get("y", 0))
                    if o.get("rotation"):
                        imp.warn(f"object '{oname}' is rotated; the rotation is ignored")
                    if any(k in o for k in ("ellipse", "polygon", "polyline", "text", "gid")):
                        kindname = next(k for k in ("ellipse", "polygon", "polyline", "text", "gid") if k in o)
                        imp.warn(f"object '{oname}' is a {'tile' if kindname == 'gid' else kindname}, which is not "
                                 "imported; use a rectangle or a point")
                        continue
                    if o.get("point"):
                        entities.append({"id": new_id(), "name": oname, "components": {
                            "LocalTransform": {"position": [num(x * sx), num((mh * th - y) * sy), 0]}}})
                        continue
                    w, h = float(o.get("width", 0)), float(o.get("height", 0))
                    if w <= 0 or h <= 0:
                        imp.warn(f"object '{oname}' has no size; skipped")
                        continue
                    col = {"shape": {"half": [num(w * sx / 2), num(h * sy / 2)]}, "density": 0}
                    if p.get("sensor") is True:
                        col["is_sensor"] = True
                    if p.get("one_way") is True:
                        col["one_way"] = True
                    entities.append({"id": new_id(), "name": oname, "components": {
                        "Collider2D": col,
                        "LocalTransform": {"position": [num((x + w / 2) * sx), num((mh * th - y - h / 2) * sy), 0]},
                        "RigidBody2D": {}}})
            else:
                imp.warn(f"layer '{lname}' is a {kind} layer, which is not imported")

    walk(doc.get("layers") or [])
    scene_name = f"{name}.mcscene"
    files[scene_name] = {"format": "mce-scene", "version": 1, "guid": guid_of(scene_name), "name": name,
                         "entities": entities}
    return files


def resolve_guids(files):
    """Replaces the converters' placeholders in `files` with GUID references; returns the texture sidecars
    that have to be written as [(path, guid)]."""
    tex_guid, new_sidecars = {}, []
    for fdoc in files.values():
        ts = fdoc.get("tileset")
        if isinstance(ts, tuple) and ts[1] not in tex_guid:
            side = ts[1] + ".mceasset"
            g = existing_guid(side)
            if not g:
                g = str(uuid.uuid4())
                new_sidecars.append((side, g))
            tex_guid[ts[1]] = g
    fname_guid = {f: d["guid"] for f, d in files.items()}
    for fdoc in files.values():
        ts = fdoc.get("tileset")
        if isinstance(ts, tuple):
            fdoc["tileset"] = "guid:" + tex_guid[ts[1]]
        for e in fdoc.get("entities", []):
            tl = e["components"].get("TilemapLayer")
            if tl:
                tl["map"] = "guid:" + fname_guid[tl["map"][1]]
    return new_sidecars


def write_outputs(out_dir, files, new_sidecars):
    os.makedirs(out_dir, exist_ok=True)
    for side, g in new_sidecars:
        write_json(side, {"guid": g, "importer": "texture", "importer_version": 1,
                          "settings": {"srgb": True, "mips": False, "filter": "nearest"}})
        print(f"wrote {side}")
    for f in sorted(files):
        write_json(os.path.join(out_dir, f), files[f])


def cmd_tiled(args):
    imp = Importer("tiled")
    src = args.file
    try:
        doc = read_json(src)
        map_dir = os.path.dirname(os.path.abspath(src))
        out_dir = args.out_dir or map_dir
        name = safe_name(args.name or os.path.splitext(os.path.basename(src))[0])
        existing = {}
        for f in os.listdir(out_dir) if os.path.isdir(out_dir) else []:
            if f.startswith(name) and f.endswith((".mcscene", ".mctilemap")):
                g = existing_guid(os.path.join(out_dir, f))
                if g:
                    existing[f] = g
        files = convert_tiled(doc, map_dir, name, out_dir, existing, args.tile_size, imp)
        new_sidecars = resolve_guids(files)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
        print(f"mce import tiled: {src}: {e}", file=sys.stderr)
        return 1
    if imp.warnings and args.werror:
        print("mce import tiled: --werror: nothing written", file=sys.stderr)
        return 1
    write_outputs(out_dir, files, new_sidecars)
    maps = sum(1 for f in files if f.endswith(".mctilemap"))
    ents = len(files[f"{name}.mcscene"]["entities"])
    print(f"wrote {out_dir}{os.sep}{name}.mcscene ({ents} entities) and {maps} tilemap(s)")
    return 0


def ldtk_tilesets(doc, base_dir, imp):
    """{tileset uid: {name, image, columns, rows, grid}} for the tilesets that have an image file."""
    out = {}
    for ts in (doc.get("defs") or {}).get("tilesets") or []:
        rel = ts.get("relPath")
        name = str(ts.get("identifier", "tileset"))
        if not rel:
            continue  # LDtk's embedded icon atlas
        grid = int(ts["tileGridSize"])
        if int(ts.get("padding", 0)) or int(ts.get("spacing", 0)):
            raise ValueError(f"tileset '{name}' has padding or spacing; the engine cuts a tileset into equal "
                             "cells with none. Re-export the tileset image without them")
        wpx, hpx = int(ts["pxWid"]), int(ts["pxHei"])
        if grid <= 0 or wpx % grid or hpx % grid:
            raise ValueError(f"tileset '{name}': the {wpx}x{hpx} image is not a whole number of {grid}px cells")
        image = os.path.normpath(os.path.join(base_dir, rel))
        if not os.path.isfile(image):
            raise ValueError(f"tileset '{name}': the image '{rel}' was not found ({image})")
        out[ts["uid"]] = {"name": safe_name(name), "image": image, "columns": wpx // grid, "rows": hpx // grid,
                          "grid": grid}
    return out


def ldtk_levels(doc, base_dir, only):
    """[level dict with layerInstances] (external level files read), optionally just the one called `only`."""
    out = []
    for lv in doc.get("levels") or []:
        if only and lv.get("identifier") != only:
            continue
        if lv.get("layerInstances") is None and lv.get("externalRelPath"):
            lv = read_json(os.path.normpath(os.path.join(base_dir, lv["externalRelPath"])))
        if lv.get("layerInstances") is None:
            raise ValueError(f"level '{lv.get('identifier')}' has no layers; save the project in LDtk and export again")
        out.append(lv)
    if only and not out:
        raise ValueError(f"no level called '{only}' (levels: " +
                         ", ".join(str(l.get("identifier")) for l in doc.get("levels") or []) + ")")
    if not out:
        raise ValueError("the project has no levels")
    return out


def convert_ldtk(doc, base_dir, name, existing, tile_size, imp, only=None):
    """{file name: JSON doc}: per level a scene and one tilemap per rendered layer. Pure; GUIDs from `existing`."""
    # The world layout is ignored: every level is its own scene with its origin at the level's bottom-left.
    tilesets = ldtk_tilesets(doc, base_dir, imp)
    grid0 = int(doc.get("defaultGridSize") or 16)
    s = tile_size / grid0  # metres per LDtk pixel
    files = {}

    def guid_of(fname):
        return existing.get(fname) or str(uuid.uuid4())

    for lv in ldtk_levels(doc, base_dir, only):
        lname = safe_name(lv.get("identifier", "level"))
        pw, ph = int(lv["pxWid"]), int(lv["pxHei"])
        entities, taken = [], {}

        def add(ename, comps):
            n = taken.get(ename, 0) + 1
            taken[ename] = n
            entities.append({"id": len(entities) + 1, "name": ename if n == 1 else f"{ename} {n}", "components": comps})

        layers = lv["layerInstances"]
        for depth, lyr in enumerate(reversed(layers), 1):  # LDtk lists the top layer first; order 1 = bottom
            ident = str(lyr.get("__identifier", "layer"))
            kind = lyr.get("__type")
            grid = int(lyr["__gridSize"])
            cw, ch = int(lyr["__cWid"]), int(lyr["__cHei"])
            ox, oy = int(lyr.get("__pxTotalOffsetX", 0)), int(lyr.get("__pxTotalOffsetY", 0))
            where = f"level '{lv.get('identifier')}' layer '{ident}'"

            def px_to_m(x, y):
                return [num((ox + x) * s), num((ph - oy - y) * s), 0]

            def boxes(cells, label, **flags):
                for i, (c, r, w, h) in enumerate(merge_rects(cells, cw, ch)):
                    col = {"shape": {"half": [num(w * grid * s / 2), num(h * grid * s / 2)]}, "density": 0}
                    col.update(flags)
                    add(f"{label} {i}", {"Collider2D": col, "RigidBody2D": {},
                                         "LocalTransform": {"position": px_to_m((c + w / 2) * grid, (r + h / 2) * grid)}})

            if kind == "Entities":
                for e in lyr.get("entityInstances") or []:
                    ename = str(e.get("__identifier", "entity"))
                    x, y = e["px"]
                    w, h = float(e.get("width", 0)), float(e.get("height", 0))
                    pv = e.get("__pivot") or [0, 0]
                    tags = e.get("__tags") or []
                    solid, sensor = "solid" in tags, "sensor" in tags
                    if (solid or sensor) and w > 0 and h > 0:
                        col = {"shape": {"half": [num(w * s / 2), num(h * s / 2)]}, "density": 0}
                        if sensor:
                            col["is_sensor"] = True
                        add(ename, {"Collider2D": col, "RigidBody2D": {},
                                    "LocalTransform": {"position": px_to_m(x + (0.5 - pv[0]) * w, y + (0.5 - pv[1]) * h)}})
                    else:
                        add(ename, {"LocalTransform": {"position": px_to_m(x, y)}})
                if any(e.get("fieldInstances") for e in lyr.get("entityInstances") or []):
                    imp.warn(f"{where}: entity fields are not imported (name an entity by its identifier or tag it "
                             "'solid' / 'sensor')")
                continue
            if kind not in ("IntGrid", "Tiles", "AutoLayer"):
                imp.warn(f"{where} is a {kind} layer, which is not imported")
                continue
            if kind == "IntGrid":
                csv = lyr.get("intGridCsv") or []
                if len(csv) != cw * ch:
                    raise ValueError(f"{where}: intGridCsv has {len(csv)} cells, expected {cw}x{ch}")
                solid = [(i % cw, i // cw) for i, v in enumerate(csv) if v]
                if solid:
                    boxes(solid, f"{ident} collision")
            tiles = lyr.get("gridTiles") if kind == "Tiles" else lyr.get("autoLayerTiles")
            tiles = tiles or []
            if not tiles or not lyr.get("visible", True):
                if tiles:
                    imp.warn(f"{where} is hidden; skipped")
                continue
            ts = tilesets.get(lyr.get("__tilesetDefUid"))
            if ts is None:
                imp.warn(f"{where} uses a tileset without an image file; skipped")
                continue
            if ts["grid"] != grid:
                raise ValueError(f"{where}: the layer grid ({grid}px) differs from its tileset's ({ts['grid']}px)")
            cells, flipped, stacked = [0] * (cw * ch), False, False
            for t in tiles:
                x, y = t["px"]
                sx, sy = t["src"]
                col, row = x // grid, y // grid
                if x % grid or y % grid or sx % grid or sy % grid or not (0 <= col < cw and 0 <= row < ch):
                    raise ValueError(f"{where}: a tile at {x},{y} is not on the layer's {grid}px grid")
                if t.get("f"):
                    flipped = True
                at = (ch - 1 - row) * cw + col  # the engine's row 0 is the bottom
                stacked = stacked or cells[at] != 0
                cells[at] = (sy // grid) * ts["columns"] + sx // grid + 1
            if flipped:
                imp.warn(f"{where} has flipped tiles; they draw unflipped")
            if stacked:
                imp.warn(f"{where} stacks several tiles in one cell; the last one wins")
            fname = f"{name}_{lname}_{safe_name(ident)}.mctilemap"
            files[fname] = {"format": "mce-tilemap", "version": 1, "guid": guid_of(fname), "name": fname[:-len(".mctilemap")],
                            "tileset": ("png", ts["image"]), "columns": ts["columns"], "rows": ts["rows"],
                            "width": cw, "height": ch, "tile_size": num(grid * s), "tiles": cells}
            comp = {"map": ("map", fname), "order": depth}
            if float(lyr.get("__opacity", 1)) != 1:
                comp["tint"] = [1, 1, 1, num(lyr["__opacity"])]
            add(ident, {"LocalTransform": {"position": px_to_m(0, ch * grid)}, "TilemapLayer": comp})
        scene = f"{name}_{lname}.mcscene"
        files[scene] = {"format": "mce-scene", "version": 1, "guid": guid_of(scene), "name": f"{name}_{lname}",
                        "entities": entities}
    return files


def cmd_ldtk(args):
    imp = Importer("ldtk")
    src = args.file
    try:
        doc = read_json(src)
        base_dir = os.path.dirname(os.path.abspath(src))
        out_dir = args.out_dir or base_dir
        name = safe_name(args.name or os.path.splitext(os.path.basename(src))[0])
        existing = {}
        for f in os.listdir(out_dir) if os.path.isdir(out_dir) else []:
            if f.startswith(name) and f.endswith((".mcscene", ".mctilemap")):
                g = existing_guid(os.path.join(out_dir, f))
                if g:
                    existing[f] = g
        files = convert_ldtk(doc, base_dir, name, existing, args.tile_size, imp, args.level)
        new_sidecars = resolve_guids(files)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
        print(f"mce import ldtk: {src}: {e}", file=sys.stderr)
        return 1
    if imp.warnings and args.werror:
        print("mce import ldtk: --werror: nothing written", file=sys.stderr)
        return 1
    write_outputs(out_dir, files, new_sidecars)
    scenes = sorted(f for f in files if f.endswith(".mcscene"))
    maps = sum(1 for f in files if f.endswith(".mctilemap"))
    print(f"wrote {len(scenes)} scene(s) ({', '.join(scenes)}) and {maps} tilemap(s) in {out_dir}")
    return 0


def cmd_3d(args):
    import mce_import_3d  # lights and cameras of glTF / FBX models (M-IMPORT3D step 4), installed beside this script
    return mce_import_3d.run(args.tool, args)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mce import", description="2D authoring-tool exports and 3D models' lights and cameras into native assets")
    ap.add_argument("--merge-output", action="store_true", help=argparse.SUPPRESS)  # warnings on stdout (the MCP tool)
    sub = ap.add_subparsers(dest="tool", metavar="<tool>", required=True)
    s = sub.add_parser("aseprite", help="Aseprite JSON export + PNG into a .mcsheet")
    s.add_argument("file", help="the exported .json")
    s.add_argument("--out", help="the .mcsheet to write (default: beside the export)")
    s.add_argument("--name", help="the sheet's name (default: the file name)")
    s.add_argument("--werror", action="store_true", help="a warning is a failure; nothing is written")
    s.set_defaults(fn=cmd_aseprite)
    s = sub.add_parser("tiled", help="Tiled .tmj map (+ .tsj tilesets) into tilemaps and a scene")
    s.add_argument("file", help="the exported .tmj / .json map")
    s.add_argument("--out-dir", help="where to write (default: beside the map)")
    s.add_argument("--name", help="base name of the files written (default: the map's file name)")
    s.add_argument("--tile-size", type=float, default=1.0, help="metres per tile (default 1)")
    s.add_argument("--werror", action="store_true", help="a warning is a failure; nothing is written")
    s.set_defaults(fn=cmd_tiled)
    s = sub.add_parser("ldtk", help="LDtk .ldtk project: one scene per level, tilemaps per tile layer")
    s.add_argument("file", help="the .ldtk project")
    s.add_argument("--out-dir", help="where to write (default: beside the project)")
    s.add_argument("--name", help="base name of the files written (default: the project's file name)")
    s.add_argument("--level", help="import only the level with this identifier (default: every level)")
    s.add_argument("--tile-size", type=float, default=1.0, help="metres per default grid cell (default 1)")
    s.add_argument("--werror", action="store_true", help="a warning is a failure; nothing is written")
    s.set_defaults(fn=cmd_ldtk)
    for tool, what in (("gltf", "glTF / GLB model"), ("fbx", "FBX model")):
        s = sub.add_parser(tool, help=f"{what}: its lights and cameras into a .mcprefab")
        s.add_argument("file", help="the model inside the project's assets/ (its .mceasset sidecar is created when missing)")
        s.add_argument("--out-dir", help="where to write the prefab (default: beside the model)")
        s.add_argument("--name", help="the prefab's name (default: the model's file name)")
        if tool == "fbx":
            s.add_argument("--cook", help="the mce_cook that reads the FBX (default: MCE_COOK, or beside this script)")
        else:
            s.set_defaults(cook=None)
        s.add_argument("--werror", action="store_true", help="a warning is a failure; nothing is written")
        s.set_defaults(fn=cmd_3d)
    args = ap.parse_args(argv)
    if args.merge_output:
        sys.stderr = sys.stdout
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
