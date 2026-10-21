#!/usr/bin/env python3
"""samples/import_demo: writes the art and the three tool exports under import/, then converts them with
`mce import` into assets/ (the converters are tools/mce_cli/mce_import.py).

    python3 samples/import_demo/gen_art.py

  import/hero.json      Aseprite JSON export (array frames, tags)   -> assets/sprites/hero.mcsheet
  import/cave.tmj       Tiled map                                   -> assets/levels/cave*.mc{scene,tilemap}
  import/world.ldtk     LDtk project, two levels                    -> assets/levels/world_*.mc{scene,tilemap}
  assets/models/tent.obj (+ .mtl)   an OBJ with a default and an MTL material, written here (its sidecars use fixed GUIDs)
  assets/models/quad.fbx            an ASCII FBX 7.4 quad, written here
  assets/models/lamp.gltf           a glTF triangle with no lights (what the cook sees)
  import/lamp_lit.gltf              the same triangle as an exporter writes it, with a point light and a camera
                                    -> assets/prefabs/lamp.mcprefab (`mce import gltf`: MeshRef to lamp.gltf + the two entities)
  assets/scenes/main.mcscene  hand-written: camera, the imported hero and the imported cave ground

The exports are written in the tools' file formats by this script, not saved from the applications, so
the sample needs no Aseprite, Tiled or LDtk to rebuild. Running it again changes nothing (GUIDs are kept);
check_reimport.py (a ctest) proves it. MCE_IMPORT_SCRIPT names another mce_import.py.
"""

import base64
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
# The 3D fixtures' sidecars are written once with these fixed GUIDs (the demo's textures do the same: committed sidecars).
MODEL_GUIDS = {"tent.obj": ("1e1f0000-0000-4000-8000-0000000000a1", "obj", 1),
               "tent.mtl": ("1e1f0000-0000-4000-8000-0000000000a2", "mtl", 1),
               "quad.fbx": ("1e1f0000-0000-4000-8000-0000000000a3", "fbx", 1),
               "lamp.gltf": ("1e1f0000-0000-4000-8000-0000000000a4", "gltf", 4)}


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


def sidecar(path, name):
    """<path>.mceasset for a model file, with the fixed GUID of `name`; the file is left alone when it exists."""
    guid, importer, version = MODEL_GUIDS[name]
    path += ".mceasset"
    if not os.path.exists(path):
        write(path, {"guid": guid, "importer": importer, "importer_version": version, "settings": {}})


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def obj_files(models):
    """A tent: a floor triangle with the default material and a roof triangle with the .mtl's one (a .mtl asset holds one material)."""
    write_text(os.path.join(models, "tent.mtl"),
               "newmtl roof\nKd 0.8 0.3 0.2\n")
    write_text(os.path.join(models, "tent.obj"),
               "mtllib tent.mtl\no tent\nv -1 0 -1\nv 1 0 -1\nv 0 1.5 1\nv 0 0 1\n"
               "f 1 4 2\nusemtl roof\nf 1 2 3\n")
    sidecar(os.path.join(models, "tent.obj"), "tent.obj")
    sidecar(os.path.join(models, "tent.mtl"), "tent.mtl")


def fbx_quad():
    """An ASCII FBX 7.4 quad (Y up, metres) with one red material, as test_cook_fbx writes its fixtures."""
    return """; FBX 7.4.0 project file
FBXHeaderExtension:  {
\tFBXHeaderVersion: 1003
\tFBXVersion: 7400
\tCreator: "import_demo gen_art.py"
}
GlobalSettings:  {
\tVersion: 1000
\tProperties70:  {
\t\tP: "UpAxis", "int", "Integer", "",1
\t\tP: "UpAxisSign", "int", "Integer", "",1
\t\tP: "FrontAxis", "int", "Integer", "",2
\t\tP: "FrontAxisSign", "int", "Integer", "",1
\t\tP: "CoordAxis", "int", "Integer", "",0
\t\tP: "CoordAxisSign", "int", "Integer", "",1
\t\tP: "UnitScaleFactor", "double", "Number", "",100
\t}
}
Objects:  {
\tGeometry: 1001, "Geometry::Quad", "Mesh" {
\t\tVertices: *12 {
\t\t\ta: -1,0,-1,1,0,-1,1,0,1,-1,0,1
\t\t}
\t\tPolygonVertexIndex: *4 {
\t\t\ta: 0,2,1,-4
\t\t}
\t\tGeometryVersion: 124
\t}
\tModel: 1002, "Model::Quad", "Mesh" {
\t\tVersion: 232
\t\tProperties70:  {
\t\t\tP: "Lcl Translation", "Lcl Translation", "", "A",0,0,0
\t\t\tP: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0
\t\t\tP: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
\t\t}
\t}
\tMaterial: 1003, "Material::Red", "" {
\t\tVersion: 102
\t\tShadingModel: "phong"
\t\tMultiLayer: 0
\t\tProperties70:  {
\t\t\tP: "DiffuseColor", "Color", "", "A",0.8,0.2,0.2
\t\t}
\t}
}
Connections:  {
\tC: "OO",1002,0
\tC: "OO",1001,1002
\tC: "OO",1003,1002
}
"""


def gltf_triangle(lit):
    """A one-triangle glTF (data URI buffer). `lit` adds a point light and a camera on their own nodes, as an exporter does."""
    pos = struct.pack("<9f", -1, 0, 0, 1, 0, 0, 0, 1, 0)
    idx = struct.pack("<3H", 0, 1, 2) + b"\0\0"
    uri = "data:application/octet-stream;base64," + base64.b64encode(pos + idx).decode()
    doc = {"asset": {"version": "2.0", "generator": "import_demo gen_art.py"},
           "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"name": "Tri", "mesh": 0}],
           "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
           "buffers": [{"byteLength": len(pos) + len(idx), "uri": uri}],
           "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
           "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                          "min": [-1, 0, 0], "max": [1, 1, 0]},
                         {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]}
    if lit:
        doc["scenes"][0]["nodes"] = [0, 1, 2]
        doc["nodes"] += [{"name": "Lamp", "translation": [0, 3, 1], "extensions": {"KHR_lights_punctual": {"light": 0}}},
                         {"name": "View", "translation": [0, 1, 5], "camera": 0}]
        doc["extensionsUsed"] = ["KHR_lights_punctual"]
        doc["extensions"] = {"KHR_lights_punctual": {"lights": [{"type": "point", "color": [1, 0.9, 0.7], "intensity": 4000}]}}
        doc["cameras"] = [{"type": "perspective", "perspective": {"yfov": 0.8, "znear": 0.1, "zfar": 100}}]
    return doc


def models_3d(assets):
    models = os.path.join(assets, "models")
    obj_files(models)
    write_text(os.path.join(models, "quad.fbx"), fbx_quad())
    sidecar(os.path.join(models, "quad.fbx"), "quad.fbx")
    write(os.path.join(models, "lamp.gltf"), gltf_triangle(False))
    sidecar(os.path.join(models, "lamp.gltf"), "lamp.gltf")
    # The exporter's file keeps its lights and camera; the cook would warn that it drops them (the prefab carries them), so
    # it sits under import/ and shares the model's GUID through its own sidecar.
    lit = os.path.join(HERE, "import", "lamp_lit.gltf")
    write(lit, gltf_triangle(True))
    sidecar(lit, "lamp.gltf")
    convert("gltf", lit, "--out-dir", os.path.join(assets, "prefabs"), "--name", "lamp", "--werror")


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
    models_3d(assets)
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
