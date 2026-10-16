#!/usr/bin/env python3
"""samples/import_demo: writes the art and the three tool exports under import/, then converts them with
`mce import` into assets/ (the converters are tools/mce_cli/mce_import.py).

    python3 samples/import_demo/gen_art.py

  import/hero.json      Aseprite JSON export (array frames, tags)   -> assets/sprites/hero.mcsheet
  import/cave.tmj       Tiled map                                   -> assets/levels/cave*.mc{scene,tilemap}
  import/world.ldtk     LDtk project, two levels                    -> assets/levels/world_*.mc{scene,tilemap}
  assets/scenes/main.mcscene  hand-written: camera, the imported hero and the imported cave ground

The exports are written in the tools' file formats by this script, not saved from the applications, so
the sample needs no Aseprite, Tiled or LDtk to rebuild. Running it again changes nothing (GUIDs are kept);
check_reimport.py (a ctest) proves it. MCE_IMPORT_SCRIPT names another mce_import.py.
"""

import json
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
IMPORTER = os.environ.get("MCE_IMPORT_SCRIPT") or os.path.join(ROOT, "tools", "mce_cli", "mce_import.py")
SCENE_GUID = "1e1f0000-0000-4000-8000-000000000001"


def png(path, w, h, pixel):
    rows = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def write(path, doc):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")


HERO_COLORS = [(230, 90, 60), (240, 130, 70), (60, 160, 230), (90, 200, 240)]


def hero_pixel(x, y):
    f, lx = divmod(x, 16)
    edge = lx in (0, 15) or y in (0, 15)
    r, g, b = HERO_COLORS[f]
    eye = y in (5, 6) and lx in (4, 11)
    if eye:
        return (255, 255, 255, 255)
    return (r // 2, g // 2, b // 2, 255) if edge else (r, g, b, 255)


TILE_COLORS = [(110, 80, 50), (70, 150, 70), (130, 130, 140), (200, 180, 90),
               (90, 90, 200), (160, 60, 60), (60, 60, 60), (230, 230, 230)]


def tile_pixel(x, y):
    t = (y // 16) * 4 + x // 16
    r, g, b = TILE_COLORS[t]
    lx, ly = x % 16, y % 16
    shade = 0.8 if (lx in (0, 15) or ly in (0, 15)) else 1.0
    return (int(r * shade), int(g * shade), int(b * shade), 255)


def aseprite_export():
    frames = []
    for i in range(4):
        frames.append({"filename": f"hero {i}.aseprite", "frame": {"x": i * 16, "y": 0, "w": 16, "h": 16},
                       "rotated": False, "trimmed": False,
                       "spriteSourceSize": {"x": 0, "y": 0, "w": 16, "h": 16}, "sourceSize": {"w": 16, "h": 16},
                       "duration": 150 if i < 2 else 100})
    return {"frames": frames,
            "meta": {"app": "https://www.aseprite.org/", "version": "1.3", "image": "../assets/textures/hero.png",
                     "format": "RGBA8888", "size": {"w": 64, "h": 16}, "scale": "1",
                     "frameTags": [{"name": "idle", "from": 0, "to": 1, "direction": "pingpong"},
                                   {"name": "run", "from": 2, "to": 3, "direction": "forward"}]}}


def tiled_export():
    w, h = 12, 8
    data = [0] * (w * h)
    for c in range(w):  # Tiled stores the top row first: a grass row (tile 2) on a dirt row (tile 1), both solid
        data[(h - 2) * w + c] = 2
        data[(h - 1) * w + c] = 1
    for c in (4, 5, 6):
        data[(h - 4) * w + c] = 3  # a stone ledge
    return {"type": "map", "version": "1.10", "orientation": "orthogonal", "renderorder": "right-down",
            "infinite": False, "width": w, "height": h, "tilewidth": 16, "tileheight": 16,
            "tilesets": [{"firstgid": 1, "name": "tiles", "image": "../assets/textures/tiles.png",
                          "imagewidth": 64, "imageheight": 32, "tilewidth": 16, "tileheight": 16, "tilecount": 8,
                          "columns": 4, "margin": 0, "spacing": 0,
                          "tiles": [{"id": 0, "properties": [{"name": "solid", "type": "bool", "value": True}]},
                                    {"id": 1, "properties": [{"name": "solid", "type": "bool", "value": True}]},
                                    {"id": 2, "properties": [{"name": "solid", "type": "bool", "value": True}]}]}],
            "layers": [
                {"type": "tilelayer", "name": "Ground", "width": w, "height": h, "visible": True, "opacity": 1,
                 "data": data},
                {"type": "objectgroup", "name": "Things", "objects": [
                    {"id": 1, "name": "Start", "x": 24, "y": 96, "point": True},
                    {"id": 2, "name": "Exit", "x": 160, "y": 64, "width": 16, "height": 32,
                     "properties": [{"name": "sensor", "type": "bool", "value": True}]}]}]}


def ldtk_export():
    w, h = 12, 8

    def level(ident, floor, coins):
        csv = [0] * (w * h)
        for c in range(w):
            csv[(h - 1) * w + c] = 1
        for c in floor:
            csv[(h - 3) * w + c] = 1
        tiles = []
        for i, v in enumerate(csv):
            if v:
                c, r = i % w, i // w
                tiles.append({"px": [c * 16, r * 16], "src": [32, 0], "f": 0, "t": 2})
        ents = [{"__identifier": "Coin", "px": [x * 16 + 8, 24], "width": 8, "height": 8, "__pivot": [0.5, 0.5],
                 "__tags": ["sensor"], "fieldInstances": []} for x in coins]
        common = {"__cWid": w, "__cHei": h, "__gridSize": 16, "__pxTotalOffsetX": 0, "__pxTotalOffsetY": 0,
                  "visible": True}
        return {"identifier": ident, "pxWid": w * 16, "pxHei": h * 16, "layerInstances": [
            dict(common, __identifier="Things", __type="Entities", entityInstances=ents),
            dict(common, __identifier="Floor", __type="IntGrid", __opacity=1, __tilesetDefUid=1,
                 intGridCsv=csv, autoLayerTiles=tiles)]}

    return {"__header__": {"fileType": "LDtk Project JSON", "app": "LDtk"}, "jsonVersion": "1.5.3",
            "defaultGridSize": 16,
            "defs": {"tilesets": [{"uid": 1, "identifier": "Tiles", "relPath": "../assets/textures/tiles.png",
                                   "pxWid": 64, "pxHei": 32, "tileGridSize": 16, "padding": 0, "spacing": 0}]},
            "levels": [level("Level_1", [3, 4, 5], [4, 8]), level("Level_2", [2, 3, 7, 8], [3, 9])]}


def convert(*argv):
    r = subprocess.run([sys.executable, IMPORTER, *argv], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode:
        sys.exit(r.returncode)


def guid_of(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)["guid"]


def main():
    assets = os.path.join(HERE, "assets")
    png(os.path.join(assets, "textures", "hero.png"), 64, 16, hero_pixel)
    png(os.path.join(assets, "textures", "tiles.png"), 64, 32, tile_pixel)
    write(os.path.join(HERE, "import", "hero.json"), aseprite_export())
    write(os.path.join(HERE, "import", "cave.tmj"), tiled_export())
    write(os.path.join(HERE, "import", "world.ldtk"), ldtk_export())
    convert("aseprite", os.path.join(HERE, "import", "hero.json"), "--out", os.path.join(assets, "sprites", "hero.mcsheet"),
            "--name", "hero")
    convert("tiled", os.path.join(HERE, "import", "cave.tmj"), "--out-dir", os.path.join(assets, "levels"), "--name", "cave")
    convert("ldtk", os.path.join(HERE, "import", "world.ldtk"), "--out-dir", os.path.join(assets, "levels"), "--name", "world")
    main_scene = os.path.join(assets, "scenes", "main.mcscene")
    sheet = guid_of(os.path.join(assets, "sprites", "hero.mcsheet"))
    ground = guid_of(os.path.join(assets, "levels", "cave_Ground.mctilemap"))
    try:
        scene_guid = guid_of(main_scene)
    except OSError:
        scene_guid = SCENE_GUID
    write(main_scene, {"format": "mce-scene", "version": 1, "guid": scene_guid, "name": "main", "entities": [
        {"id": 1, "name": "Camera", "components": {
            "LocalTransform": {"position": [6, 4, 0]},
            "Camera2DComponent": {"fit_height": 8, "pixel_snap": False, "clear_color": [0.55, 0.72, 0.9, 1]}}},
        {"id": 2, "name": "Ground", "components": {
            "LocalTransform": {"position": [0, 0, 0]}, "TilemapLayer": {"map": "guid:" + ground, "order": 1}}},
        {"id": 3, "name": "Hero", "components": {
            "LocalTransform": {"position": [3, 2.5, 0]},
            "Sprite": {"layer": 2, "size": [1, 1]},
            "AnimatedSprite": {"sheet": "guid:" + sheet, "clip": "run"}}}]})


if __name__ == "__main__":
    main()
