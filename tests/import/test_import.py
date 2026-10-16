#!/usr/bin/env python3
"""M-IMPORT: the converters in tools/mce_cli/mce_import.py, on small hand-made exports.

    test_import.py <work dir>
"""

import contextlib
import io
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "mce_cli"))
import mce_import  # noqa: E402

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def run(argv):
    """(exit code, stderr) of mce_import.main."""
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        code = mce_import.main(argv)
    return code, err.getvalue()


def frame(x, y, w=16, h=16, duration=100, **extra):
    d = {"filename": f"hero {x}.ase", "frame": {"x": x, "y": y, "w": w, "h": h}, "rotated": False,
         "trimmed": False, "spriteSourceSize": {"x": 0, "y": 0, "w": w, "h": h}, "sourceSize": {"w": w, "h": h},
         "duration": duration}
    d.update(extra)
    return d


def export(frames, tags=None, slices=None, size=(64, 16), as_hash=False):
    meta = {"app": "https://www.aseprite.org/", "image": "hero.png", "format": "RGBA8888",
            "size": {"w": size[0], "h": size[1]}, "scale": "1"}
    if tags is not None:
        meta["frameTags"] = tags
    if slices is not None:
        meta["slices"] = slices
    fr = {f["filename"]: f for f in frames} if as_hash else frames
    return {"frames": fr, "meta": meta}


def setup(work, name, doc):
    d = os.path.join(work, name)
    os.makedirs(d)
    with open(os.path.join(d, "hero.png"), "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")  # the converter only needs the file to exist
    path = os.path.join(d, "hero.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return d, path


def sheet_of(d):
    with open(os.path.join(d, "hero.mcsheet"), encoding="utf-8") as f:
        return json.load(f)


def test_basic(work):
    frames = [frame(0, 0), frame(16, 0), frame(32, 0), frame(48, 0)]
    tags = [{"name": "idle", "from": 0, "to": 1, "direction": "forward"},
            {"name": "run", "from": 1, "to": 3, "direction": "reverse"},
            {"name": "bounce", "from": 0, "to": 2, "direction": "pingpong"},
            {"name": "back", "from": 0, "to": 2, "direction": "pingpong_reverse"}]
    slices = [{"name": "hit", "color": "#0000ffff", "keys": [{"frame": 0, "bounds": {"x": 2, "y": 3, "w": 5, "h": 6}}]}]
    d, path = setup(work, "basic", export(frames, tags, slices))
    code, err = run(["aseprite", path])
    check(code == 0 and err == "", "basic export converts without warnings: " + err)
    s = sheet_of(d)
    check(s["format"] == "mce-spritesheet" and s["version"] == 1, "format and version")
    check(s["image_size"] == [64, 16], "image size")
    check([r["rect"] for r in s["regions"][:4]] == [[0, 0, 16, 16], [16, 0, 16, 16], [32, 0, 16, 16], [48, 0, 16, 16]],
          "a region per frame, in order")
    check(s["regions"][4] == {"name": "hit", "rect": [2, 3, 5, 6]}, "a slice is a named region after the frames")
    clips = {c["name"]: c for c in s["clips"]}
    check(clips["idle"]["frames"] == [0, 1] and clips["idle"]["fps"] == 10.0 and "mode" not in clips["idle"], "forward tag")
    check(clips["run"]["frames"] == [3, 2, 1], "reverse tag plays backwards")
    check(clips["bounce"]["mode"] == "PingPong" and clips["bounce"]["frames"] == [0, 1, 2], "pingpong tag")
    check(clips["back"]["mode"] == "PingPong" and clips["back"]["frames"] == [2, 1, 0], "pingpong_reverse tag")
    guid = json.load(open(os.path.join(d, "hero.png.mceasset")))["guid"]
    check(s["texture"] == "guid:" + guid, "the sheet names the PNG's sidecar GUID")
    side = json.load(open(os.path.join(d, "hero.png.mceasset")))
    check(side["importer"] == "texture" and side["settings"]["filter"] == "nearest", "a pixel-art texture sidecar")


def test_idempotent(work):
    d, path = setup(work, "again", export([frame(0, 0), frame(16, 0)], [{"name": "a", "from": 0, "to": 1}]))
    run(["aseprite", path])
    first = open(os.path.join(d, "hero.mcsheet"), "rb").read()
    side = open(os.path.join(d, "hero.png.mceasset"), "rb").read()
    run(["aseprite", path])
    check(open(os.path.join(d, "hero.mcsheet"), "rb").read() == first, "a re-import gives the same bytes and keeps the GUID")
    check(open(os.path.join(d, "hero.png.mceasset"), "rb").read() == side, "an existing sidecar is left alone")


def test_hash_frames_and_default_clip(work):
    d, path = setup(work, "hash", export([frame(0, 0), frame(16, 0)], as_hash=True))
    code, err = run(["aseprite", path])
    check(code == 0, "hash-style frames convert: " + err)
    s = sheet_of(d)
    check(len(s["regions"]) == 2 and s["clips"] == [{"name": "all", "frames": [0, 1], "fps": 10.0}],
          "no tags: one clip 'all' over every frame")


def test_warnings(work):
    frames = [frame(0, 0, duration=100), frame(16, 0, duration=300, trimmed=True)]
    tags = [{"name": "x" * 40, "from": 0, "to": 1}, {"name": "x" * 40, "from": 0, "to": 0}]
    d, path = setup(work, "warn", export(frames, tags))
    code, err = run(["aseprite", path])
    check(code == 0, "warnings do not fail by default")
    check("trimmed" in err, "trimmed frames warn")
    check("durations differ" in err, "different durations warn")
    check("longer than 31" in err, "a long clip name warns")
    names = [c["name"] for c in sheet_of(d)["clips"]]
    check(len(set(names)) == 2 and all(len(n) <= 31 for n in names), "clip names stay unique and short: %r" % names)

    d2, path2 = setup(work, "werror", export(frames, tags))
    code, err = run(["aseprite", path2, "--werror"])
    check(code == 1, "--werror fails on a warning")
    check(os.listdir(d2) == ["hero.json", "hero.png"] or sorted(os.listdir(d2)) == ["hero.json", "hero.png"],
          "--werror writes nothing: %r" % sorted(os.listdir(d2)))


def test_errors(work):
    d, path = setup(work, "bad", export([frame(60, 0)], size=(64, 16)))  # 60 + 16 > 64
    code, err = run(["aseprite", path])
    check(code == 1 and "outside" in err, "a frame outside the image is an error: " + err)
    d, path = setup(work, "badtag", export([frame(0, 0)], [{"name": "a", "from": 0, "to": 5}]))
    code, err = run(["aseprite", path])
    check(code == 1 and "outside 0..0" in err, "a tag past the last frame is an error: " + err)
    code, err = run(["aseprite", os.path.join(work, "missing.json")])
    check(code == 1, "a missing file fails")


# ---- Tiled ------------------------------------------------------------------------------------------

def tiled_map(**over):
    doc = {
        "type": "map", "orientation": "orthogonal", "infinite": False, "width": 4, "height": 3,
        "tilewidth": 16, "tileheight": 16,
        "tilesets": [{"firstgid": 1, "name": "terrain", "image": "terrain.png", "imagewidth": 64, "imageheight": 32,
                      "tilewidth": 16, "tileheight": 16, "tilecount": 8, "columns": 4, "margin": 0, "spacing": 0,
                      "tiles": [{"id": 1, "properties": [{"name": "solid", "type": "bool", "value": True}]}]}],
        "layers": [
            {"type": "tilelayer", "name": "Ground", "width": 4, "height": 3, "visible": True, "opacity": 1,
             "data": [0, 0, 0, 0,
                      0, 3, 0, 0,
                      2, 2, 1, 1]},
            {"type": "objectgroup", "name": "Things", "objects": [
                {"id": 1, "name": "Door", "x": 16, "y": 16, "width": 16, "height": 32, "properties": [
                    {"name": "sensor", "type": "bool", "value": True}]},
                {"id": 2, "name": "Start", "x": 8, "y": 40, "point": True},
                {"id": 3, "name": "Blob", "x": 0, "y": 0, "width": 8, "height": 8, "ellipse": True}]}],
    }
    doc.update(over)
    return doc


def tiled_setup(work, name, doc):
    d = os.path.join(work, name)
    os.makedirs(d)
    with open(os.path.join(d, "terrain.png"), "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
    path = os.path.join(d, "level.tmj")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return d, path


def read(d, name):
    with open(os.path.join(d, name), encoding="utf-8") as f:
        return json.load(f)


def test_tiled_basic(work):
    d, path = tiled_setup(work, "tbasic", tiled_map())
    code, err = run(["tiled", path])
    check(code == 0, "a plain map converts: " + err)
    check("ellipse" in err, "an ellipse object warns")
    tm = read(d, "level_Ground.mctilemap")
    check(tm["format"] == "mce-tilemap" and tm["width"] == 4 and tm["height"] == 3 and tm["columns"] == 4 and tm["rows"] == 2,
          "tilemap header")
    check(tm["tiles"] == [2, 2, 1, 1,   0, 3, 0, 0,   0, 0, 0, 0],
          "rows are flipped (engine row 0 is the bottom) and ids are local id + 1: %r" % tm["tiles"])
    side = read(d, "terrain.png.mceasset")
    check(tm["tileset"] == "guid:" + side["guid"], "the tilemap names the tileset image's GUID")
    sc = read(d, "level.mcscene")
    by = {e["name"]: e for e in sc["entities"]}
    check(sc["format"] == "mce-scene" and "Ground" in by, "a scene with a layer entity")
    check(by["Ground"]["components"]["TilemapLayer"]["map"] == "guid:" + tm["guid"], "the layer entity names its tilemap")
    door = by["Door"]["components"]
    check(door["Collider2D"]["shape"] == {"half": [0.5, 1]} and door["Collider2D"]["is_sensor"] is True, "a rectangle is a sensor box")
    check(door["LocalTransform"]["position"] == [1.5, 1.0, 0], "rectangle position: centre, y up: %r" % door["LocalTransform"]["position"])
    check(by["Start"]["components"]["LocalTransform"]["position"] == [0.5, 0.5, 0], "a point is a marker entity")
    boxes = [e for e in sc["entities"] if e["name"].startswith("Ground collision")]
    check(len(boxes) == 1 and boxes[0]["components"]["Collider2D"]["shape"] == {"half": [1, 0.5]},
          "solid tiles (local id 1: the two 2s of the bottom row) merge into one box: %r" % [b["components"] for b in boxes])
    check(boxes[0]["components"]["LocalTransform"]["position"] == [1, 0.5, 0], "...on the bottom row, columns 0-1")


def test_tiled_idempotent(work):
    d, path = tiled_setup(work, "tagain", tiled_map())
    run(["tiled", path])
    first = {f: open(os.path.join(d, f), "rb").read() for f in sorted(os.listdir(d)) if f.startswith("level")}
    run(["tiled", path])
    second = {f: open(os.path.join(d, f), "rb").read() for f in sorted(os.listdir(d)) if f.startswith("level")}
    check(first == second and len(first) >= 3, "a re-import gives the same bytes (GUIDs kept)")


def test_tiled_encodings_and_errors(work):
    import base64
    import struct
    import zlib
    gids = [0, 0, 0, 0, 0, 3, 0, 0, 2, 2, 1, 1]
    raw = struct.pack("<12I", *gids)
    for tag, comp, blob in (("b64", "", raw), ("zlib", "zlib", zlib.compress(raw))):
        lyr = tiled_map()["layers"][0]
        lyr.update({"data": base64.b64encode(blob).decode(), "encoding": "base64"})
        if comp:
            lyr["compression"] = comp
        d, path = tiled_setup(work, "t" + tag, tiled_map(layers=[lyr]))
        code, err = run(["tiled", path])
        tm = read(d, "level_Ground.mctilemap") if code == 0 else {}
        check(code == 0 and tm.get("tiles", [0] * 12)[:4] == [2, 2, 1, 1], f"base64 {comp or 'raw'} data decodes: {err}")
    for what, doc, needle in (
            ("infinite", tiled_map(infinite=True), "infinite"),
            ("isometric", tiled_map(orientation="isometric"), "isometric"),
            ("spacing", tiled_map(tilesets=[dict(tiled_map()["tilesets"][0], spacing=2)]), "margin or spacing")):
        d, path = tiled_setup(work, "terr_" + what, doc)
        code, err = run(["tiled", path])
        check(code == 1 and needle in err, f"{what} map is an error: {err}")
    # flipped tiles warn
    lyr = tiled_map()["layers"][0]
    lyr["data"][5] = 3 | 0x80000000
    d, path = tiled_setup(work, "tflip", tiled_map(layers=[lyr]))
    code, err = run(["tiled", path])
    check(code == 0 and "flipped" in err, "flipped tiles warn: " + err)
    d, path = tiled_setup(work, "tflip2", tiled_map(layers=[lyr]))
    code, err = run(["tiled", path, "--werror"])
    check(code == 1 and not os.path.exists(os.path.join(d, "level.mcscene")), "--werror writes nothing")
    # external tileset, collide layer property, two tilesets
    d, path = tiled_setup(work, "text", tiled_map(tilesets=[{"firstgid": 1, "source": "terrain.tsj"}]))
    ts = dict(tiled_map()["tilesets"][0])
    ts.pop("firstgid"), ts.pop("tiles")
    json.dump(ts, open(os.path.join(d, "terrain.tsj"), "w"))
    code, err = run(["tiled", path])
    check(code == 0, "an external .tsj tileset loads: " + err)
    d, path = tiled_setup(work, "txml", tiled_map(tilesets=[{"firstgid": 1, "source": "terrain.tsx"}]))
    code, err = run(["tiled", path])
    check(code == 1 and ".tsx" in err, "an XML tileset is a clear error: " + err)


def main():
    work = sys.argv[1]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    for t in (test_basic, test_idempotent, test_hash_frames_and_default_clip, test_warnings, test_errors,
              test_tiled_basic, test_tiled_idempotent, test_tiled_encodings_and_errors):
        t(work)
    if failures:
        print(f"{len(failures)} check(s) failed")
        return 1
    print("import converters: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
