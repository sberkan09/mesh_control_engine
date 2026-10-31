#!/usr/bin/env python3
"""Rebuilds assets/ of the 09_openworld sample (python3 gen_assets.py). The sample is data only.

A 1 km x 1 km city slice (M-OPENWORLD E1): a 5 x 5 junction grid, 250 m apart, with two avenues
(two lanes each way) crossing at the middle and one lane each way elsewhere, traffic lights at the
nine inner junctions, blocks of boxes for buildings and parks, a player on foot with a gun and a
parked car to take, a few police, traffic and a pedestrian crowd that gather around the player.
Cars, characters and buildings are CC0 Kenney models (`mce content add`, see CREDITS.md). Road, kerb, ground and park surfaces
are textured slabs with road paint (E7: textures from tools/gen_textures.py, meshes written here); trees, lamp posts, benches,
hydrants and bins are Blender models (blender/gen_models.py); the unit cube is left for the pads and the lamp heads.
"""
import json
import math
import os
import random
import struct
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
NS = uuid.UUID("0b0e0000-0000-4000-8000-0000000000e1")


def guid(name):
    return str(uuid.uuid5(NS, name))


G_BOX = guid("models/box.glb")
TP = "third_party/"
PROPS = ["tree_oak", "tree_pine", "bush", "lamp_post", "hydrant", "bench", "bin"]
CAR_MODELS = ["sedan", "sedan-sports", "taxi", "suv", "delivery", "police"]
CHARACTERS = ["character-a", "character-b", "character-c", "character-d", "character-e", "character-f"]
DOWNTOWN_MODELS = [("building-m", 3.15), ("building-n", 2.48), ("building-l", 2.27), ("building-skyscraper-a", 2.88),
                   ("building-skyscraper-b", 4.48), ("building-skyscraper-c", 4.08)]
SUBURB_MODELS = [("building-" + c, h) for c, h in (("a", 1.29), ("b", 1.29), ("d", 1.29), ("f", 1.69), ("g", 1.69), ("h", 1.29), ("i", 1.68))]
RESIDENTIAL_MODELS = [("building-type-" + c, h) for c, h in (("a", 1.0), ("b", 1.14), ("c", 1.03), ("d", 1.24), ("e", 1.14), ("f", 1.14))]
KIT = {"sedan": "kenney-car-kit", "sedan-sports": "kenney-car-kit", "taxi": "kenney-car-kit", "suv": "kenney-car-kit",
       "delivery": "kenney-car-kit", "police": "kenney-car-kit"}


def model_rel(name):
    """Imported (mce content add) model path, relative to assets/."""
    if name in PROPS:
        return "models/props/%s.glb" % name
    if name in KIT:
        pack = KIT[name]
    elif name.startswith("character"):
        pack = "kenney-blocky-characters"
    elif name.startswith("building-type"):
        pack = "kenney-city-kit-suburban"
    else:
        pack = "kenney-city-kit-commercial"
    return "%s%s/models/%s.glb" % (TP, pack, name)


def model_guid(name):
    return guid(model_rel(name))


G_ROADS = guid("roads/city.mcroads")
G_CAR = guid("prefabs/traffic_car.mcprefab")
G_WALKER = guid("prefabs/pedestrian.mcprefab")
G_BLOCK_CAR = guid("prefabs/roadblock_car.mcprefab")
G_BLOCK_COP = guid("prefabs/roadblock_officer.mcprefab")
G_INPUT = guid("input/openworld.mcinput")
G_SCENE = guid("scenes/city.mcscene")
G_HUD = guid("ui/hud.mcui")
G_ITEMS = guid("items/loot.mcitems")

GRID = 5          # junctions per side
SPACING = 250.0   # m between junctions: 4 blocks of 250 m = 1 km
LANE_W = 3.5
SIDEWALK = 2.5


def write(rel, doc):
    path = os.path.join(HERE, "assets", rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


def write_bytes(rel, data):
    path = os.path.join(HERE, "assets", rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


# --- the one mesh: a unit cube, white, tinted per entity ------------------------------------------------

def box_glb():
    faces = [  # normal, four corners (counter-clockwise seen from outside)
        ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),
        ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)]),
        ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
        ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
        ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
        ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
    ]
    pos, nrm, uv, idx = b"", b"", b"", b""
    uvs = [(0, 1), (1, 1), (1, 0), (0, 0)]
    for fi, (n, corners) in enumerate(faces):
        for c, t in zip(corners, uvs):
            pos += struct.pack("<3f", *(0.5 * v for v in c))
            nrm += struct.pack("<3f", *n)
            uv += struct.pack("<2f", *t)
        base = fi * 4
        idx += struct.pack("<6I", base, base + 1, base + 2, base, base + 2, base + 3)
    blob = pos + nrm + uv + idx
    doc = {
        "asset": {"version": "2.0", "generator": "09_openworld gen_assets"}, "scene": 0,
        "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "box"}],
        "meshes": [{"name": "box", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                                   "indices": 3, "material": 0}]}],
        "materials": [{"name": "box", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0,
                                                              "roughnessFactor": 0.85}}],
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(pos), "target": 34962},
                        {"buffer": 0, "byteOffset": len(pos), "byteLength": len(nrm), "target": 34962},
                        {"buffer": 0, "byteOffset": len(pos) + len(nrm), "byteLength": len(uv), "target": 34962},
                        {"buffer": 0, "byteOffset": len(pos) + len(nrm) + len(uv), "byteLength": len(idx), "target": 34963}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3", "min": [-0.5] * 3, "max": [0.5] * 3},
                      {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5126, "count": 24, "type": "VEC2"},
                      {"bufferView": 3, "componentType": 5125, "count": 36, "type": "SCALAR"}],
    }
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    blob += b"\0" * (-len(blob) % 4)
    total = 12 + 8 + len(js) + 8 + len(blob)
    return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(js), b"JSON") + js
            + struct.pack("<I4s", len(blob), b"BIN\0") + blob)


def models():
    write_bytes("models/box.glb", box_glb())
    write("models/box.glb.mceasset", {"guid": G_BOX, "importer": "gltf", "importer_version": 3, "settings": {}})
    # The Kenney models come from `mce content add` (CC0, see CREDITS.md); only their sidecars are written here.
    names = set(CAR_MODELS) | set(CHARACTERS) | set(PROPS) | {n for n, _ in DOWNTOWN_MODELS + SUBURB_MODELS + RESIDENTIAL_MODELS}
    for n in sorted(names):
        write(model_rel(n) + ".mceasset", {"guid": model_guid(n), "importer": "gltf", "importer_version": 3, "settings": {}})


# --- input ----------------------------------------------------------------------------------------------

def key(name, scale=None, axis=None):
    b = {"key": name}
    if axis:
        b["axis"] = axis
    if scale:
        b["scale"] = scale
    return b


def input_map():
    gameplay = [
        {"name": "Move", "type": "axis2d", "bindings": [
            key("D"), key("A", -1), key("W", axis="y"), key("S", -1, "y"),
            key("Right"), key("Left", -1), key("Up", axis="y"), key("Down", -1, "y"),
            {"gamepad_axis": "LeftX", "deadzone": [0.15, 1]},
            {"gamepad_axis": "LeftY", "axis": "y", "deadzone": [0.15, 1], "scale": -1}]},
        {"name": "Look", "type": "axis2d", "bindings": [
            {"mouse_delta": "x"}, {"mouse_delta": "y"},
            {"gamepad_axis": "RightX", "deadzone": [0.15, 1]},
            {"gamepad_axis": "RightY", "axis": "y", "deadzone": [0.15, 1], "scale": -1}]},
        {"name": "Jump", "bindings": [key("Space"), {"gamepad_button": "South"}]},
        {"name": "Fire", "bindings": [{"mouse_button": "Left"}, key("J"), {"gamepad_button": "West"}]},
        {"name": "Reload", "bindings": [key("R"), {"gamepad_button": "North"}]},
        {"name": "Interact", "bindings": [key("E"), key("F"), {"gamepad_button": "East"}]},
        {"name": "UseItem", "bindings": [key("H"), {"gamepad_button": "RightShoulder"}]},
        {"name": "Sprint", "bindings": [key("LShift"), {"gamepad_button": "LeftShoulder"}]},
        {"name": "TriggerL", "bindings": [key("LCtrl"), {"gamepad_axis": "LeftTrigger"}]},
        {"name": "Horn", "bindings": [key("G"), {"gamepad_button": "LeftStick"}]},
        {"name": "Lights", "bindings": [key("L"), {"gamepad_button": "DpadRight"}]},
        {"name": "Pause", "bindings": [key("Escape"), {"gamepad_button": "Start"}]},
    ]
    ui = [
        {"name": "Navigate", "type": "axis2d", "bindings": [
            key("Right"), key("Left", -1), key("Up", axis="y"), key("Down", -1, "y"),
            key("D"), key("A", -1), key("W", axis="y"), key("S", -1, "y")]},
        {"name": "Confirm", "bindings": [key("Return"), key("Space"), {"gamepad_button": "South"}]},
        {"name": "Cancel", "bindings": [key("Escape"), {"gamepad_button": "East"}]},
        {"name": "Resume", "bindings": [key("Escape"), {"gamepad_button": "Start"}]},  # also what the pause menu's button emits
    ]
    write("input/openworld.mcinput", {"format": "mce-input", "version": 1, "guid": G_INPUT, "maps": [
        {"name": "Gameplay", "actions": gameplay}, {"name": "UI", "blocks_below": True, "actions": ui}]})


# --- the city's shape -----------------------------------------------------------------------------------

def node_name(i, j):
    return "n%d_%d" % (i, j)


def node_pos(i, j):
    return ((i - GRID // 2) * SPACING, (j - GRID // 2) * SPACING)


def is_avenue(road):
    """Roads on the middle row/column are the avenues (two lanes each way, faster)."""
    kind, i, j = road
    return (j == GRID // 2) if kind == "x" else (i == GRID // 2)


def road_list():
    roads = []
    for j in range(GRID):
        for i in range(GRID - 1):
            roads.append(("x", i, j))
    for i in range(GRID):
        for j in range(GRID - 1):
            roads.append(("z", i, j))
    return roads


def road_name(r):
    return "%s%d_%d" % r


def road_ends(r):
    kind, i, j = r
    return ((i, j), (i + 1, j)) if kind == "x" else ((i, j), (i, j + 1))


def road_half_width(r):
    return (2 if is_avenue(r) else 1) * LANE_W


def roads_doc():
    nodes = [{"name": node_name(i, j), "at": [node_pos(i, j)[0], 0, node_pos(i, j)[1]],
              **({"control": "signal"} if 0 < i < GRID - 1 and 0 < j < GRID - 1 else {})}
             for j in range(GRID) for i in range(GRID)]
    roads = []
    for r in road_list():
        a, b = road_ends(r)
        av = is_avenue(r)
        roads.append({"name": road_name(r), "from": node_name(*a), "to": node_name(*b), "lanes": 2 if av else 1,
                      "speed": 16 if av else 11, "sidewalk": SIDEWALK, "priority": 8 if av else 4})
    signals = []
    for j in range(1, GRID - 1):
        for i in range(1, GRID - 1):
            ew = [road_name(("x", i - 1, j)), road_name(("x", i, j))]
            ns = [road_name(("z", i, j - 1)), road_name(("z", i, j))]
            # The avenues get the longer green; neighbouring lights are shifted for a green wave along them.
            ew_s, ns_s = (30, 18) if j == GRID // 2 else (18, 30) if i == GRID // 2 else (22, 22)
            signals.append({"node": node_name(i, j), "amber": 3, "all_red": 1, "offset": (i * 7 + j * 3) % 20,
                            "phases": [{"green": ew, "seconds": ew_s}, {"green": ns, "seconds": ns_s}]})
    return {"format": "mce-roads", "version": 1, "guid": G_ROADS, "name": "city", "drive": "right",
            "lane_width": LANE_W, "speed": 11, "nodes": nodes, "roads": roads, "signals": signals}


# --- audio (M-OPENWORLD E3) -------------------------------------------------------------------------------
# The sound files in assets/audio/ come from tools/fetch_audio.py (CC0, see assets/audio/SOURCES.md); here only their sidecars,
# the two sound sets and the GUIDs the scene uses are written.

AUDIO_FILES = {"engine_loop": ("wav", {}), "rain_loop": ("wav", {"compress": "vorbis", "quality": 0.3}),
               "wind_loop": ("wav", {"compress": "vorbis", "quality": 0.3}), "night_crickets": ("wav", {"compress": "vorbis", "quality": 0.3}), "shot_1": ("wav", {}), "shot_2": ("wav", {}), "siren": ("wav", {}), "horn": ("wav", {}),
               "city_ambience": ("wav", {"compress": "vorbis", "quality": 0.4})}
FOOTSTEPS = ["footstep_%d" % i for i in range(1, 7)]
HITS = ["hit_%d" % i for i in range(1, 6)]
CRASHES = ["crash_%d" % i for i in range(1, 4)]
THUNDER = ["thunder"]
AUDIO_EXT = {name: ext for name, (ext, _) in AUDIO_FILES.items()}
AUDIO_EXT.update({name: "ogg" for name in FOOTSTEPS + HITS + CRASHES + THUNDER})


def audio_guid(name):
    return guid("audio/%s.%s" % (name, AUDIO_EXT[name]))


G_FOOTSTEPS = guid("audio/footsteps.mcsound")
G_SHOTS = guid("audio/shots.mcsound")
G_HITS = guid("audio/hits.mcsound")
G_LAMP = guid("materials/lamp.mcmat")
G_CRASHES = guid("audio/crashes.mcsound")


def horn_wav():
    """A car horn: two tones (400 and 500 Hz, softened squares) for 0.5 s, 10 ms fades, mono 44.1 kHz. Synthesized, no source file."""
    rate, n = 44100, 22050
    frames = bytearray()
    for i in range(n):
        t = i / rate
        v = sum(math.tanh(3.0 * math.sin(2 * math.pi * f * t)) for f in (400.0, 500.0)) * 0.35
        fade = min(1.0, i / (0.01 * rate), (n - 1 - i) / (0.01 * rate))
        frames += struct.pack("<h", int(max(-1.0, min(1.0, v * fade)) * 30000))
    import io
    import wave
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))
    return buf.getvalue()


def audio_assets():
    write_bytes("audio/horn.wav", horn_wav())
    for name, (ext, settings) in AUDIO_FILES.items():
        write("audio/%s.%s.mceasset" % (name, ext), {"guid": audio_guid(name), "importer": ext, "importer_version": 2, "settings": settings})
    for name in FOOTSTEPS + HITS + CRASHES + THUNDER:
        write("audio/%s.ogg.mceasset" % name, {"guid": audio_guid(name), "importer": "ogg", "importer_version": 2, "settings": {}})
    write("audio/footsteps.mcsound", {"format": "mce-sound", "version": 1, "guid": G_FOOTSTEPS,
                                      "clips": ["guid:" + audio_guid(n) for n in FOOTSTEPS], "pick": "random", "no_repeat": True,
                                      "pitch": [0.92, 1.08], "gain_db": [-3, 0]})
    write("audio/hits.mcsound", {"format": "mce-sound", "version": 1, "guid": G_HITS, "clips": ["guid:" + audio_guid(n) for n in HITS],
                                 "pick": "random", "no_repeat": True, "pitch": [0.9, 1.1], "gain_db": [-3, 0]})
    write("audio/crashes.mcsound", {"format": "mce-sound", "version": 1, "guid": G_CRASHES, "clips": ["guid:" + audio_guid(n) for n in CRASHES],
                                    "pick": "random", "no_repeat": True, "pitch": [0.85, 1.05], "gain_db": [-2, 0]})
    write("audio/shots.mcsound", {"format": "mce-sound", "version": 1, "guid": G_SHOTS,
                                  "clips": ["guid:" + audio_guid("shot_1"), "guid:" + audio_guid("shot_2")], "pick": "random",
                                  "no_repeat": True, "pitch": [0.96, 1.04], "gain_db": [-2, 0]})


def engine_audio(traffic):
    """The engine of a car: a looping 3D AudioSource whose gain and pitch EngineAudio drives from the car's speed."""
    return {
        "AudioSource": {"clip": "guid:" + audio_guid("engine_loop"), "bus": "SFX", "loop": True, "play_on_start": True,
                        "gain_db": -60, "priority": 90 if traffic else 150},
        "AudioSpatial": {"attenuation_id": 1},
        "EngineAudio": {"idle_pitch": 0.8, "top_pitch": 2.3, "top_speed": 28, "idle_db": -14 if traffic else -10,
                        "top_db": -6 if traffic else 0, "parked_db": -60},
    }


# --- scene helpers --------------------------------------------------------------------------------------

class Scene:
    def __init__(self):
        self.ents = []

    def add(self, name, comps):
        self.ents.append({"id": len(self.ents) + 1, "name": name, "components": comps})
        return len(self.ents) - 1

    def ref(self, index):
        return "$%d" % index

    def doc(self, name, guid_):
        return {"format": "mce-scene", "version": 1, "guid": guid_, "name": name, "entities": self.ents}


def tf(position, scale=None, yaw=None):
    t = {"position": [round(v, 3) for v in position]}
    if yaw:
        t["rotation"] = [0, round(math.sin(yaw / 2), 5), 0, round(math.cos(yaw / 2), 5)]
    if scale:
        t["scale"] = [round(v, 3) for v in scale]
    return t


def model_mesh(name, shadows=1):
    m = {"mesh": "guid:" + model_guid(name)}
    if not shadows:
        m["cast_shadows"] = 0
    return m


def mesh(tint, shadows=1):
    m = {"mesh": "guid:" + G_BOX, "tint": [round(c, 3) for c in tint] + [1]}
    if not shadows:
        m["cast_shadows"] = 0
    return m


def prop(scene, name, pos, size, tint, collider=False, shadows=1):
    """A box: visual at `pos` (its centre) of `size`, with a static collider when asked."""
    comps = {"LocalTransform": tf(pos, size), "MeshRef": mesh(tint, shadows)}
    if collider:
        comps["BoxCollider3D"] = {"half": [round(s / 2, 3) for s in size]}
        comps["LocalTransform"] = tf(pos)
        # A collider's entity scale would scale the mesh and not the collider; keep the visual a child.
        e = scene.add(name, {"LocalTransform": tf(pos), "BoxCollider3D": comps["BoxCollider3D"]})
        scene.add(name + " mesh", {"ChildOf": {"parent": scene.ref(e)}, "LocalTransform": {"scale": [round(s, 3) for s in size]},
                                   "MeshRef": comps["MeshRef"]})
        return e
    return scene.add(name, comps)


ASPHALT = (0.11, 0.115, 0.125)
KERB = (0.55, 0.55, 0.53)
GRASS = (0.20, 0.34, 0.14)
PARK = (0.16, 0.38, 0.17)


# --- the look (M-OPENWORLD E7): textured surfaces, road paint, street furniture ------------------------------
# tools/gen_textures.py writes assets/textures/*.jpg; the sidecars, the four surface materials, the slab and paint meshes
# (boxes with UVs in metres, so one texture tile covers `uv` metres whatever the slab size) are written here. The Blender props
# (blender/gen_models.py) come in through the PROPS list.

SURFACES = {"asphalt": (4.0, [1.0, 1.0, 1.0, 1.0], 0.92), "paving": (2.4, [1.0, 0.98, 0.94, 1.0], 0.85),
            "grass": (8.0, [0.85, 0.9, 0.8, 1.0], 0.95), "concrete": (4.0, [1.0, 1.0, 1.0, 1.0], 0.9)}
PAINT = {"marking_white": [0.92, 0.92, 0.88, 1.0], "marking_yellow": [0.95, 0.74, 0.12, 1.0]}
G_MAT = {name: guid("materials/%s.mcmat" % name) for name in list(SURFACES) + list(PAINT)}
PAINT_H = 0.03       # paint is a 3 cm slab on the road, so it never fights the road for the depth buffer
ROAD_TOP = 0.14
SIDEWALK_TOP = 0.2


def surface_assets():
    for name, (uv, tint, rough) in SURFACES.items():
        textures = {}
        for kind in ("color", "normal"):
            rel = "textures/%s_%s.jpg" % (name, kind)
            write(rel + ".mceasset", {"guid": guid(rel), "importer": "texture", "importer_version": 1,
                                      "settings": {"srgb": False} if kind == "normal" else {}})
            textures["base_color" if kind == "color" else "normal"] = "guid:" + guid(rel)
        write("materials/%s.mcmat" % name, {"format": "mce-material", "version": 1, "guid": G_MAT[name], "name": name, "base_color": tint,
                                            "metallic": 0.0, "roughness": rough, "emissive": [0, 0, 0], "alpha_mode": "opaque",
                                            "alpha_cutoff": 0.5, "double_sided": False, "unlit": False, "textures": textures})
    for name, color in PAINT.items():
        write("materials/%s.mcmat" % name, {"format": "mce-material", "version": 1, "guid": G_MAT[name], "name": name, "base_color": color,
                                            "metallic": 0.0, "roughness": 0.6, "emissive": [0, 0, 0], "alpha_mode": "opaque",
                                            "alpha_cutoff": 0.5, "double_sided": False, "unlit": False, "textures": {}})


FACES = [  # normal, four corners (counter-clockwise seen from outside)
    ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),
    ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)]),
    ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
    ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
    ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
    ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
]


def boxes_glb(boxes, uv_m):
    """One glTF mesh of axis-aligned boxes (cx, cy, cz, sx, sy, sz); UVs are world metres / uv_m (0 when uv_m is None)."""
    pos, nrm, uvs, idx = [], [], [], []
    for cx, cy, cz, sx, sy, sz in boxes:
        for n, corners in FACES:
            base = len(pos)
            for c in corners:
                p = (cx + 0.5 * c[0] * sx, cy + 0.5 * c[1] * sy, cz + 0.5 * c[2] * sz)
                pos.append(p)
                nrm.append(n)
                if uv_m is None:
                    uvs.append((0.0, 0.0))
                elif n[1]:
                    uvs.append((p[0] / uv_m, p[2] / uv_m))
                elif n[0]:
                    uvs.append((p[2] / uv_m, -p[1] / uv_m))
                else:
                    uvs.append((p[0] / uv_m, -p[1] / uv_m))
            idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    pb = b"".join(struct.pack("<3f", *p) for p in pos)
    nb = b"".join(struct.pack("<3f", *p) for p in nrm)
    ub = b"".join(struct.pack("<2f", *p) for p in uvs)
    ib = b"".join(struct.pack("<I", i) for i in idx)
    blob = pb + nb + ub + ib
    lo = [min(p[k] for p in pos) for k in range(3)]
    hi = [max(p[k] for p in pos) for k in range(3)]
    doc = {
        "asset": {"version": "2.0", "generator": "09_openworld gen_assets"}, "scene": 0,
        "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "surface"}],
        "meshes": [{"name": "surface", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{"name": "surface", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0.9}}],
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(pb), "target": 34962},
                        {"buffer": 0, "byteOffset": len(pb), "byteLength": len(nb), "target": 34962},
                        {"buffer": 0, "byteOffset": len(pb) + len(nb), "byteLength": len(ub), "target": 34962},
                        {"buffer": 0, "byteOffset": len(pb) + len(nb) + len(ub), "byteLength": len(ib), "target": 34963}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": lo, "max": hi},
                      {"bufferView": 1, "componentType": 5126, "count": len(pos), "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5126, "count": len(pos), "type": "VEC2"},
                      {"bufferView": 3, "componentType": 5125, "count": len(idx), "type": "SCALAR"}],
    }
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    blob += b"\0" * (-len(blob) % 4)
    total = 12 + 8 + len(js) + 8 + len(blob)
    return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(js), b"JSON") + js
            + struct.pack("<I4s", len(blob), b"BIN\0") + blob)


WRITTEN = {}


def surface_mesh(key, boxes, uv_m):
    """The guid of the slab/paint mesh `key`, writing it (and its sidecar) the first time it is asked for."""
    rel = "models/surface/%s.glb" % key
    if rel not in WRITTEN:
        write_bytes(rel, boxes_glb(boxes, uv_m))
        write(rel + ".mceasset", {"guid": guid(rel), "importer": "gltf", "importer_version": 3, "settings": {}})
        WRITTEN[rel] = guid(rel)
    return WRITTEN[rel]


def dims(*v):
    return "_".join(("%.2f" % x).replace(".", "p") for x in v)


def slab(sx, sy, sz, material):
    """A MeshRef of a box of `sx` x `sy` x `sz` centred on the entity, textured with surface `material` at its tile size."""
    g = surface_mesh("slab_%s_%s" % (material, dims(sx, sy, sz)), [(0, 0, 0, sx, sy, sz)], SURFACES[material][0])
    return {"mesh": "guid:" + g, "material": "guid:" + G_MAT[material], "cast_shadows": 0}


def paint(key, boxes, colour):
    g = surface_mesh(key, boxes, None)
    return {"mesh": "guid:" + g, "material": "guid:" + G_MAT[colour], "cast_shadows": 0}


def dashes(x0, x1, z, width, on=3.0, off=6.0):
    x = x0
    while x + on <= x1 + 1e-6:
        yield (x + on / 2, PAINT_H / 2, z, on, PAINT_H, width)
        x += on + off


def road_paint(L, h, ha, hb, avenue):
    """The road paint of one x-road of length L (traffic drives on the right: +x on the +z side) between two junctions whose
    pads are `ha` and `hb` wide (half-widths, the junction top reaches 1 m further): returns (white, yellow) box lists."""
    white, yellow = [], []
    xa, xb = -L / 2 + ha + 1, L / 2 - hb - 1          # the road meets the junction tops here
    # crosswalks across the whole road at both ends (bars along the road, every 1.2 m), a stop line before each, on the lane that arrives
    for end, x_in, sgn in (("a", xa, 1), ("b", xb, -1)):
        zc = -h + 0.9
        while zc <= h - 0.9 + 1e-6:
            white.append((x_in + sgn * 2.0, PAINT_H / 2, zc, 3.0, PAINT_H, 0.6))
            zc += 1.2
        # the arriving lane is -z at end a (traffic heading -x) and +z at end b
        z0, z1 = (-h + 0.2, -0.2) if end == "a" else (0.2, h - 0.2)
        white.append((x_in + sgn * 4.3, PAINT_H / 2, (z0 + z1) / 2, 0.5, PAINT_H, z1 - z0))
    x0, x1 = xa + 5.5, xb - 5.5
    white.append(((x0 + x1) / 2, PAINT_H / 2, h - 0.3, x1 - x0, PAINT_H, 0.15))
    white.append(((x0 + x1) / 2, PAINT_H / 2, -h + 0.3, x1 - x0, PAINT_H, 0.15))
    if avenue:
        for z in (-0.12, 0.12):
            yellow.append(((x0 + x1) / 2, PAINT_H / 2, z, x1 - x0, PAINT_H, 0.12))
        for z in (-LANE_W, LANE_W):
            white.extend(dashes(x0, x1, z, 0.15))
    else:
        white.extend(dashes(x0, x1, 0.0, 0.15))
    return white, yellow


def city_surface(scene):
    # Ground: a grass slab (the visual) over a collider slab (the physics floor for everything).
    half = GRID // 2 * SPACING + 60
    scene.add("Ground", {"LocalTransform": tf((0, -0.5, 0)), "BoxCollider3D": {"half": [half, 0.5, half], "friction": 1.0}})
    scene.add("Ground mesh", {"LocalTransform": tf((0, -0.25, 0)), "MeshRef": slab(2 * half, 0.5, 2 * half, "grass")})
    # The world edge: four invisible walls on the slab's rim, so nothing (player, car, walker) walks or drives off the map.
    for n, (x, z, hx, hz) in enumerate(((half + 1, 0, 1, half + 2), (-half - 1, 0, 1, half + 2), (0, half + 1, half + 2, 1), (0, -half - 1, half + 2, 1))):
        scene.add("World edge %d" % n, {"LocalTransform": tf((x, 10.0, z)), "BoxCollider3D": {"half": [hx, 10.0, hz]}})
    node_half = {}
    for r in road_list():
        for n in road_ends(r):
            node_half[n] = max(node_half.get(n, 0), road_half_width(r))
    # Junctions: a paving pad (the corners) with an asphalt top over it.
    for (i, j), h in sorted(node_half.items()):
        x, z = node_pos(i, j)
        pad = 2 * (h + SIDEWALK) + 2
        scene.add("Corner %d_%d" % (i, j), {"LocalTransform": tf((x, 0.05, z)), "MeshRef": slab(pad, 0.1, pad, "paving")})
        scene.add("Junction top %d_%d" % (i, j), {"LocalTransform": tf((x, ROAD_TOP / 2, z)),
                                                  "MeshRef": slab(2 * h + 2, ROAD_TOP, 2 * h + 2, "asphalt")})
    for r in road_list():
        kind, i, j = r
        a, b = road_ends(r)
        (ax, az), (bx, bz) = node_pos(*a), node_pos(*b)
        h = road_half_width(r)
        ha, hb = node_half[a], node_half[b]
        long_x = kind == "x"
        yaw = None if long_x else -math.pi / 2       # a z-road is the x-road turned so that local +x runs towards node b
        cx, cz = (ax + bx) / 2, (az + bz) / 2

        def at(lx, ly, lz=0.0):
            """World position of a point `lx` along the road from its middle (+x towards b), `lz` to its right, `ly` high."""
            return (cx + lx, ly, cz + lz) if long_x else (cx - lz, ly, cz + lx)

        # The asphalt runs from one junction top to the other.
        xa, xb = -SPACING / 2 + ha + 1, SPACING / 2 - hb - 1
        scene.add("Road " + road_name(r), {"LocalTransform": tf(at((xa + xb) / 2, ROAD_TOP / 2), yaw=yaw),
                                           "MeshRef": slab(xb - xa, ROAD_TOP, 2 * h, "asphalt")})
        white, yellow = road_paint(SPACING, h, ha, hb, is_avenue(r))
        key = "paint_%s_%s" % (dims(h, ha, hb), "avenue" if is_avenue(r) else "street")
        scene.add("Paint " + road_name(r), {"LocalTransform": tf(at(0, ROAD_TOP), yaw=yaw), "MeshRef": paint(key + "_white", white, "marking_white")})
        if yellow:
            scene.add("Paint yellow " + road_name(r), {"LocalTransform": tf(at(0, ROAD_TOP), yaw=yaw),
                                                       "MeshRef": paint(key + "_yellow", yellow, "marking_yellow")})
        trim = max(ha, hb)
        span = SPACING - 2 * trim
        for side in (-1, 1):
            off = side * (h + SIDEWALK / 2)
            scene.add("Sidewalk %s %+d" % (road_name(r), side),
                      {"LocalTransform": tf(at(0, SIDEWALK_TOP / 2, off), yaw=yaw), "MeshRef": slab(span, SIDEWALK_TOP, SIDEWALK, "paving")})
    return node_half


def street_furniture(scene, rng, node_half):
    """Street trees along the outer edge of the sidewalks (no collider: the crowd walks there), a bench or two per road, a hydrant and a
    bin at the two free corners of every junction."""
    count = 0
    for r in road_list():
        kind, i, j = r
        a, b = road_ends(r)
        (ax, az), (bx, bz) = node_pos(*a), node_pos(*b)
        h = road_half_width(r)
        trim = max(node_half[a], node_half[b])
        long_x = kind == "x"
        cx, cz = (ax + bx) / 2, (az + bz) / 2
        for side in (-1, 1):
            lat = side * (h + SIDEWALK - 0.5)
            lx = -SPACING / 2 + trim + 14 + rng.uniform(0, 6)
            while lx < SPACING / 2 - trim - 14:
                x, z = (cx + lx, cz + lat) if long_x else (cx - lat, cz + lx)
                model = "tree_oak" if rng.random() < 0.65 else "tree_pine"
                sc = rng.uniform(0.7, 0.95)
                scene.add("Street tree %s %+d %d" % (road_name(r), side, count),
                          {"LocalTransform": tf((x, SIDEWALK_TOP, z), (sc, sc, sc), rng.uniform(0, 2 * math.pi)), "MeshRef": model_mesh(model)})
                count += 1
                lx += rng.uniform(26, 34)
            # a bench in the middle of the road's pavement, its back to the buildings, facing the road
            yaw = (0.0 if side > 0 else math.pi) if long_x else (-math.pi / 2 if side > 0 else math.pi / 2)
            bx_, bz_ = (cx + 10 * side, cz + lat) if long_x else (cx - lat, cz + 10 * side)
            scene.add("Bench %s %+d" % (road_name(r), side), {"LocalTransform": tf((bx_, SIDEWALK_TOP, bz_), yaw=yaw), "MeshRef": model_mesh("bench")})
    for (i, j), h in sorted(node_half.items()):
        x, z = node_pos(i, j)
        c = h + 2.3
        scene.add("Hydrant %d_%d" % (i, j), {"LocalTransform": tf((x + c, 0.1, z - c), yaw=rng.uniform(0, 6.28)), "MeshRef": model_mesh("hydrant")})
        scene.add("Bin %d_%d" % (i, j), {"LocalTransform": tf((x - c, 0.1, z + c), yaw=rng.uniform(0, 6.28)), "MeshRef": model_mesh("bin")})
    return count


def block_bounds(i, j):
    """The lot area of block (i, j) (between junction columns i, i+1 and rows j, j+1): x0, x1, z0, z1."""
    x0, z0 = node_pos(i, j)
    x1, z1 = node_pos(i + 1, j + 1)
    # The roads on the four sides: x{i}_{j} (north z0), x{i}_{j+1} (south), z{i}_{j} (west x0), z{i+1}_{j} (east).
    m = SIDEWALK + 2.0
    return (x0 + road_half_width(("z", i, j)) + m, x1 - road_half_width(("z", i + 1, j)) - m,
            z0 + road_half_width(("x", i, j)) + m, z1 - road_half_width(("x", i, j + 1)) - m)


DOWNTOWN = [(0.55, 0.58, 0.64), (0.62, 0.60, 0.56), (0.48, 0.52, 0.58), (0.70, 0.68, 0.62), (0.40, 0.45, 0.52)]
SUBURB = [(0.74, 0.58, 0.45), (0.80, 0.72, 0.58), (0.62, 0.45, 0.38), (0.78, 0.76, 0.70), (0.55, 0.62, 0.50)]


def buildings(scene, rng):
    count = 0
    for j in range(GRID - 1):
        for i in range(GRID - 1):
            x0, x1, z0, z1 = block_bounds(i, j)
            downtown = abs(i - 1.5) <= 1.0 and abs(j - 1.5) <= 1.0
            lots = 3
            lw, ld = (x1 - x0) / lots, (z1 - z0) / lots
            for a in range(lots):
                for b in range(lots):
                    cx, cz = x0 + (a + 0.5) * lw, z0 + (b + 0.5) * ld
                    name = "Lot %d_%d.%d_%d" % (i, j, a, b)
                    if rng.random() < (0.12 if downtown else 0.25):
                        scene.add(name + " park", {"LocalTransform": tf((cx, 0.05, cz)), "MeshRef": slab(lw - 6, 0.1, ld - 6, "grass")})
                        for t in range(3):
                            tx, tz = cx + rng.uniform(-lw / 3, lw / 3), cz + rng.uniform(-ld / 3, ld / 3)
                            sc = rng.uniform(0.9, 1.35)
                            model = "tree_oak" if rng.random() < 0.6 else "tree_pine"
                            # The collider is the trunk; the Blender tree is a child that stands on the ground under it.
                            e = scene.add(name + " tree %d" % t, {"LocalTransform": tf((tx, 1.5, tz)), "BoxCollider3D": {"half": [0.25, 1.5, 0.25]}})
                            scene.add(name + " tree %d mesh" % t, {"ChildOf": {"parent": scene.ref(e)},
                                                                   "LocalTransform": tf((0, -1.5, 0), (sc, sc, sc), rng.uniform(0, 6.28)),
                                                                   "MeshRef": model_mesh(model)})
                        for t in range(2):
                            scene.add(name + " bush %d" % t, {"LocalTransform": tf((cx + rng.uniform(-lw / 3, lw / 3), 0.1, cz + rng.uniform(-ld / 3, ld / 3)),
                                                                                    (1.2, 1.2, 1.2), rng.uniform(0, 6.28)),
                                                              "MeshRef": model_mesh("bush")})
                        scene.add(name + " bench", {"LocalTransform": tf((cx, 0.1, cz + ld / 2 - 5), yaw=math.pi), "MeshRef": model_mesh("bench")})
                        continue
                    w, d = (lw - 8) * rng.uniform(0.7, 1.0), (ld - 8) * rng.uniform(0.7, 1.0)
                    centre = math.hypot(cx, cz)
                    if downtown:
                        h = 18 + 70 * math.exp(-(centre / 220.0) ** 2) * rng.uniform(0.5, 1.0) + rng.uniform(0, 14)
                        model, top = rng.choice(DOWNTOWN_MODELS)
                    else:
                        h = rng.uniform(5, 13)
                        model, top = rng.choice(SUBURB_MODELS + RESIDENTIAL_MODELS)
                    bx, bz = cx + rng.uniform(-2, 2), cz + rng.uniform(-2, 2)
                    # The collider is a box of the lot; the visual is a Kenney building stretched to the same box
                    # (the model spans 0..top+1 in y, 2 x 2 in plan, so the scale is the box over the model).
                    e = scene.add(name, {"LocalTransform": tf((bx, h / 2, bz)),
                                         "BoxCollider3D": {"half": [round(w / 2, 3), round(h / 2, 3), round(d / 2, 3)]}})
                    sy = h / (top + 1.0)
                    scene.add(name + " mesh", {"ChildOf": {"parent": scene.ref(e)},
                                               "LocalTransform": tf((0, -h / 2, 0), (w / 2, sy, d / 2)),
                                               "MeshRef": model_mesh(model)})
                    # A tree (or a bush) in a free corner of the lot, outside the building's box.
                    sx, sz = rng.choice((-1, 1)), rng.choice((-1, 1))
                    tx, tz = cx + sx * (lw / 2 - 2.2), cz + sz * (ld / 2 - 2.2)
                    sc = rng.uniform(0.9, 1.2)
                    scene.add(name + " tree", {"LocalTransform": tf((tx, 0.0, tz), (sc, sc, sc), rng.uniform(0, 6.28)),
                                               "MeshRef": model_mesh("tree_oak" if rng.random() < 0.6 else "tree_pine")})
                    count += 1
    return count


# --- prefabs --------------------------------------------------------------------------------------------

CAR_COLORS = [(0.7, 0.1, 0.1), (0.1, 0.25, 0.6), (0.85, 0.85, 0.82), (0.12, 0.12, 0.14), (0.8, 0.6, 0.1), (0.15, 0.45, 0.25)]


CAR_SCALE = 0.95
CHAR_SCALE = 0.3   # Blocky Characters are about 6 units tall; the capsule is 1.8 m


def headlights():
    """One spot light on the nose of a car, lit with the street lamps (the car drives toward -z, the light looks along it and a little down)."""
    return {"SpotLight3D": {"color": [1.0, 0.92, 0.75, 1], "offset": [0, 0.55, -1.9], "direction": [0, -0.14, -1], "intensity": 9, "radius": 34,
                            "outer_deg": 34, "inner_deg": 16},
            "TimedLight3D": {"on_hour": 18.5, "off_hour": 5.75, "fade_minutes": 20}}


def wreck_effects():
    """A car that dies burns: smoke and fire (the 3D emitter, off until the car dies), a crash sound and a camera shake, all on FireOn Died."""
    return {
        "ParticleEmitter3D": {"color_start": [2.5, 1.0, 0.25, 0.9], "color_end": [0.06, 0.06, 0.06, 0], "offset": [0, 0.5, -1.1],
                              "box": [0.4, 0.1, 0.4], "direction": [0, 1, 0], "gravity": [0, 1.2, 0], "spread_deg": 18, "rate": 14,
                              "speed_min": 1.0, "speed_max": 2.2, "lifetime_min": 2.5, "lifetime_max": 3.5, "drag": 0.6,
                              "size_start": 0.5, "size_end": 2.2, "max_particles": 128, "seed": 9, "collide": 0, "additive": 0, "emitting": 0},
        "ParticleBurst": {"count": 60, "on": "Died", "keep_emitting": 1},
        "SfxTrigger": {"clip": "guid:" + G_CRASHES, "gain_db": -2, "gain_jitter_db": 2, "on": "Died"},
        "ShakeTrigger": {"trauma": 0.35, "on": "Died"},
    }


def car_entities(model, traffic):
    """A car: a RigidBody3D/Vehicle3D root and the Kenney model as its child."""
    root = {
        "LocalTransform": {},
        "RigidBody3D": {"half": [0.9, 0.4, 1.5], "density": 300},
        "Vehicle3D": {"engine_torque": 900, "self_right": 1, "anti_roll": 8000},
        "VehicleSeat3D": {"seat": [0, 0.2, 0.1], "exit": [-1.7, 0, 0]},
    }
    root.update(engine_audio(traffic))
    root.update(headlights())
    root["RigidBody3D"].update(contact_events=1, contact_min_speed=4)
    root["ImpactAudio"] = {"clip": "guid:" + G_CRASHES, "min_speed": 4, "full_speed": 18, "quiet_db": -10, "loud_db": 0}
    # Crashes hurt: hard hits (ImpactDamage3D) wear the car down and hurt its driver, a dead car is a wreck that stops driving
    # (kit_wreck). A wrecked traffic car is cleared after a while and the spawner fills the slot; a parked car stays as scenery.
    root["ImpactDamage3D"] = {"min_speed": 6, "damage_per_ms": 5, "occupant_share": 0.4}
    root.update(wreck_effects())
    if traffic:
        root["Health3D"] = {"max": 150, "team": 0, "destroy_on_death": 1, "linger": 10}
        root["TrafficDriver3D"] = {}
        root["VehicleSeat3D"]["locked"] = 1
    else:
        # A car the player takes also runs people over: the zone ahead of the bumper hurts walkers above 5 m/s.
        root["Health3D"] = {"max": 200, "team": 0, "destroy_on_death": 0}
        root["VehicleRam3D"] = {"min_speed": 5, "damage_per_ms": 12}
        root["VehicleSeat3D"]["enter_radius"] = 4.0
        # The driver's horn (G, stick press) and headlights (L, D-pad right): kit_vehicle_extras.
        root["VehicleHorn3D"] = {"clip": "guid:" + audio_guid("horn"), "gain_db": -4, "attenuation_id": 1}
        root["VehicleLights3D"] = {}
    ents = [{"name": "Car", "components": root}]
    # One Kenney car (body and wheels in one model, wheels do not spin); the model's front faces +z, the car drives toward -z.
    ents.append({"name": "Model", "components": {"ChildOf": {"parent": "$0"}, "LocalTransform": tf((0, -0.4, 0), (CAR_SCALE,) * 3, math.pi),
                                                  "MeshRef": model_mesh(model)}})
    return ents


def police_car(scene, name, pos, yaw, min_level, dispatch):
    """A police car: a Vehicle3D driven by Police3D along the roads (drive_car 2)."""
    police = {"min_level": min_level, "attack_level": 10, "drive_nav": 0, "aim": 0, "drive_car": 2,
              "sight_range": 90, "attack_range": 0, "search_time": 15, "arrest_range": 0, "dispatch_range": dispatch}
    comps = {
        "LocalTransform": tf(pos, None, yaw),
        "RigidBody3D": {"half": [0.9, 0.4, 1.5], "density": 300},
        "Vehicle3D": {"engine_torque": 900, "self_right": 1, "anti_roll": 8000},
        "Health3D": {"max": 150, "team": 2, "destroy_on_death": 0},
        "Police3D": police,
    }
    comps["RigidBody3D"].update(contact_events=1, contact_min_speed=4)
    comps["ImpactAudio"] = {"clip": "guid:" + G_CRASHES, "min_speed": 4, "full_speed": 18, "quiet_db": -8, "loud_db": 0}
    comps.update(headlights())
    comps.update(wreck_effects())
    # Police cars wear down in crashes like any car (a dead one stops chasing, kit_wreck); the officer inside is not an entity, so no occupant share.
    comps["ImpactDamage3D"] = {"min_speed": 6, "damage_per_ms": 5, "occupant_share": 0}
    # The siren sits on the root: the audio bridge follows an entity's own LocalTransform, a child's is relative. It sounds from the
    # wanted level the car answers; its loud wail stands in for the engine.
    comps.update({"AudioSource": {"clip": "guid:" + audio_guid("siren"), "bus": "SFX", "loop": True, "play_on_start": True,
                                  "gain_db": -60, "priority": 170},
                  "AudioSpatial": {"attenuation_id": 1}, "SirenAudio": {"min_level": min_level, "active_db": -8}})
    car = scene.add(name, comps)
    scene.add(name + " model", {"ChildOf": {"parent": scene.ref(car)}, "LocalTransform": tf((0, -0.4, 0), (CAR_SCALE,) * 3, math.pi),
                                "MeshRef": model_mesh("police")})
    return car


def items_catalog():
    """What a killed walker may drop; the player uses the first consumable with H (ItemHotkey3D) and the HUD lists the bag."""
    write("items/loot.mcitems", {"format": "mce-items", "version": 1, "guid": G_ITEMS, "items": [
        {"id": "bandage", "name": "Bandage", "stack": 5, "weight": 0.1, "tags": ["consumable", "heal"], "stats": {"heal": 20}},
        {"id": "medkit", "name": "Medkit", "stack": 3, "weight": 0.4, "tags": ["consumable", "heal"], "stats": {"heal": 60}}],
        "tables": [{"id": "pedestrian", "rolls": [0, 1], "entries": [
            {"item": "bandage", "weight": 3, "qty": 1}, {"item": "medkit", "weight": 1, "qty": 1}]}]})


def prefabs():
    # Roadblock pieces: a police car that is a static box (the barricade does not roll away), and an officer who holds his ground.
    write("prefabs/roadblock_car.mcprefab", {"format": "mce-prefab", "version": 1, "guid": G_BLOCK_CAR, "name": "roadblock_car", "entities": [
        {"name": "Roadblock car", "components": {"LocalTransform": {}, "BoxCollider3D": {"half": [0.9, 0.95, 1.5]}}},
        {"name": "Model", "components": {"ChildOf": {"parent": "$0"}, "LocalTransform": tf((0, -0.5, 0), (CAR_SCALE,) * 3, math.pi),
                                         "MeshRef": model_mesh("police")}}]})
    write("prefabs/roadblock_officer.mcprefab", {"format": "mce-prefab", "version": 1, "guid": G_BLOCK_COP, "name": "roadblock_officer", "entities": [
        {"name": "Roadblock officer", "components": {
            "LocalTransform": {},
            "CharacterController3D": {"radius": 0.3, "half_height": 0.6, "speed": 4.5, "layer": 2, "mask": 65535},
            "Health3D": {"max": 80, "team": 2, "destroy_on_death": 1, "linger": 4},
            "Police3D": {"min_level": 3, "attack_level": 3, "drive_nav": 0, "sight_range": 90, "attack_range": 70, "search_time": 6,
                         "arrest_range": 0},
            "Weapon3D": {"kind": "Hitscan", "damage": 10, "range": 80, "rate": 3, "spread_deg": 1.5, "magazine": 0}}},
        {"name": "Model", "components": {"ChildOf": {"parent": "$0"}, "LocalTransform": tf((0, -0.9, 0), (CHAR_SCALE,) * 3, math.pi),
                                         "MeshRef": model_mesh("character-d")}}]})
    write("prefabs/traffic_car.mcprefab", {"format": "mce-prefab", "version": 1, "guid": G_CAR, "name": "traffic_car",
                                           "entities": car_entities("sedan", True)})
    walker = [{"name": "Pedestrian", "components": {
        "LocalTransform": {}, "Pedestrian3D": {},
        # A 1.8 m kinematic capsule: the crowd stands a walker with a body on it (its origin is the capsule's centre,
        # 0.9 m above the pavement) and drives it by velocity, so the model hangs 0.9 m below.
        "RigidBody3D": {"type": "Kinematic", "shape": "Capsule", "radius": 0.3, "half_height": 0.6},
        "Health3D": {"max": 30, "team": 3, "destroy_on_death": 1, "linger": 0.2},
        # A killed walker may drop a bandage or a medkit (about every other kill), a small green box at his feet.
        "LootDrop": {"catalog": "guid:" + G_ITEMS, "table": "pedestrian", "radius": 1.5, "scatter": 0.5, "lift": -0.7,
                     "mesh": "guid:" + G_BOX, "tint": [0.3, 1.0, 0.4, 1], "mesh_scale": 0.35}}},
        {"name": "Model", "components": {"ChildOf": {"parent": "$0"}, "LocalTransform": tf((0, -0.9, 0), (CHAR_SCALE,) * 3, math.pi),
                                         "MeshRef": model_mesh("character-b")}}]
    write("prefabs/pedestrian.mcprefab", {"format": "mce-prefab", "version": 1, "guid": G_WALKER, "name": "pedestrian",
                                          "entities": walker})


# --- day and night, sky, street lamps, weather (M-OPENWORLD E6) ---------------------------------------------

START_HOUR = 9.0      # the game starts on a morning
DAY_SECONDS = 720     # twelve real minutes for 24 hours
WEATHER_SEED = 55     # the first ten weather picks (150 s each) are clear, rain, clear, storm, overcast ... and none is snow


def lamp_material():
    write("materials/lamp.mcmat", {"format": "mce-material", "version": 1, "guid": G_LAMP, "name": "lamp", "base_color": [0.1, 0.1, 0.12, 1],
                                   "metallic": 0.1, "roughness": 0.3, "emissive": [2.2, 1.6, 0.8], "alpha_mode": "opaque", "alpha_cutoff": 0.5,
                                   "double_sided": False, "unlit": False, "textures": {}})


def atmosphere(s, net, node_half):
    """A world clock with a day cycle, a physical sky with a moon and stars, a street lamp at two corners of every junction that
    lights at dusk, and a seeded weather schedule (rain, storm, fog, lightning) with its rain and wind beds and a night bed."""
    s.add("Clock", {"LocalTransform": {}, "WorldClock": {"hour": START_HOUR, "day_length_seconds": DAY_SECONDS}, "DayCycle": {}})
    s.add("Weather", {"LocalTransform": {}, "Weather": {"transition_seconds": 20, "auto_change_seconds": 150, "seed": WEATHER_SEED},
                      "WindGust": {}, "GroundMist": {}, "Moon": {}, "WetSurfaces": {"strength": 4, "darken": 0.5, "puddles": 0.6},
                      "RainRipples": {}, "ThunderDelay": {"clip": "guid:" + audio_guid("thunder"), "volume": 1.5}, "WindDirection": {"degrees": 0, "drift_degrees_per_minute": 6}, "CloudLayer": {}})
    s.add("Rain", {"LocalTransform": tf((0, 20, 34)),
                   "Precipitation3D": {"rain_rate": 3000, "snow_rate": 0, "follow_camera": 1, "height": 12, "area": 14, "rain_speed": 18,
                                       "snow_speed": 1.5},
                   "ParticleEmitter3D": {"color_start": [0.55, 0.62, 0.75, 0.35], "color_end": [0.55, 0.62, 0.75, 0.15], "offset": [0, 0, 0],
                                         "box": [14, 0.5, 14], "direction": [0, -1, 0], "gravity": [0, -9, 0], "spread_deg": 2, "rate": 0,
                                         "speed_min": 16.2, "speed_max": 19.8, "lifetime_min": 0.667, "lifetime_max": 0.8, "drag": 0,
                                         "ground_height": 0, "bounce": 0, "size_start": 0.04, "size_end": 0.04, "max_particles": 4096,
                                         "seed": 1, "burst": 0, "burst_version": 0, "collide": 0, "additive": 0, "emitting": 0}})
    s.add("Rain splashes", {"LocalTransform": {}, "RainSplash": {},
                            "ParticleEmitter3D": {"color_start": [0.7, 0.78, 0.9, 0.5], "color_end": [0.7, 0.78, 0.9, 0], "size_start": 0.05,
                                                  "size_end": 0.03, "max_particles": 1024, "seed": 2, "collide": 0, "additive": 0,
                                                  "emitting": 0, "rate": 0}})
    s.add("Lightning", {"LocalTransform": {}, "Lightning": {"min_gap_seconds": 6, "max_gap_seconds": 16, "flash_ticks": 8, "flash_boost": 4,
                                                              "seed": 7}, "LightningBolt": {}})
    rain_bed = s.add("Rain bed", {"AudioSource": {"clip": "guid:" + audio_guid("rain_loop"), "bus": "SFX", "loop": True, "play_on_start": True,
                                                  "gain_db": -60, "priority": 190}})
    wind_bed = s.add("Wind bed", {"AudioSource": {"clip": "guid:" + audio_guid("wind_loop"), "bus": "SFX", "loop": True, "play_on_start": True,
                                                  "gain_db": -60, "priority": 190}})
    s.ents[next(i for i, e in enumerate(s.ents) if e["name"] == "Weather")]["components"]["WeatherAudio"] = {
        "rain_source": s.ref(rain_bed), "wind_source": s.ref(wind_bed), "rain_db": -10, "wind_db": -18, "fade_seconds": 6}
    # The night bed rises after 19:00 and fades out after 05:00; the city bed thins a little at night.
    s.add("Night bed", {"AudioSource": {"clip": "guid:" + audio_guid("night_crickets"), "bus": "SFX", "loop": True, "play_on_start": True,
                                        "gain_db": -60, "priority": 190},
                        "ClockAudio": {"on_hour": 19.5, "off_hour": 4.5, "fade_hours": 1.5, "on_db": -24, "off_db": -60}})
    for e in s.ents:
        if e["name"] == "City ambience":
            e["components"]["ClockAudio"] = {"on_hour": 19.5, "off_hour": 4.5, "fade_hours": 1.5, "on_db": -30, "off_db": -22}
    # Street lamps: two opposite corners of every junction pad (the pad is 14 m square), lit from dusk to dawn.
    for j in range(GRID):
        for i in range(GRID):
            x, z = node_pos(i, j)
            c = node_half[(i, j)] + 2.3
            # Two opposite corners of the pad; the arm of the Blender pole reaches 0.9 m towards the junction and carries the lamp head.
            for k, (dx, dz, yaw) in enumerate(((c, c, 3 * math.pi / 4), (-c, -c, -math.pi / 4))):
                name = "Lamp %d_%d_%d" % (i, j, k)
                s.add(name + " post", {"LocalTransform": tf((x + dx, 0.1, z + dz), yaw=yaw), "MeshRef": model_mesh("lamp_post")})
                hx, hz = x + dx + 0.9 * math.cos(yaw), z + dz - 0.9 * math.sin(yaw)
                s.add(name, {"LocalTransform": tf((hx, 5.24, hz), (0.34, 0.12, 0.34)),
                             "MeshRef": {"mesh": "guid:" + G_BOX, "material": "guid:" + G_LAMP, "cast_shadows": 0},
                             "PointLight3D": {"color": [1.0, 0.72, 0.42, 1], "intensity": 16, "radius": 28},
                             "TimedLight3D": {"on_hour": 18.5 + (i + j) % 3 * 0.25, "off_hour": 5.75, "fade_minutes": 20}})

# --- the loop: missions, money to find and money to spend (M-OPENWORLD E7/E8) ------------------------------------------
# Everything here is added after the older entities so their order (and the spawners' walkers) stays as it was.
# The player has a wallet (500). Pads are flat boxes on the avenue sidewalks (z = -9 or +9); a pad with a mission marker starts a
# mission when the player stands on it, the destination pads are the objectives, and the minimap shows both (MapMarker).

def pad(s, name, x, z, size, tint, extra=None):
    comps = {"LocalTransform": tf((x, 0.22, z), (size, 0.04, size)), "MeshRef": mesh(tint, 0)}
    if extra:
        comps.update(extra)
    return s.add(name, comps)


def economy_and_missions(s, player):
    actor = s.ref(player)
    key = [1000]

    def save_key():
        key[0] += 1
        return {"key": key[0]}

    def mission(title, start, stops, limit, reward, fee, wanted_fail, tint):
        """A marker pad at `start`, a chain of ReachZone objectives at `stops` and the Mission that ties them."""
        objectives = []
        for n, stop in enumerate(stops):
            what = stop[0] if isinstance(stop[0], str) else "reach"
            label = "%s %d" % (title, n + 1)
            if what == "kill_team":  # kill `count` of Health3D.team `team` yourself (no place to go)
                obj = {"kind": "Kill", "team": stop[1], "by_actor": 1, "count": stop[2]}
            elif what == "kill_target":  # a named man stands at (x, z); the step is done when he is dead
                man = s.add(title + " target", {
                    "LocalTransform": tf((stop[1], 0.95, stop[2])),
                    "RigidBody3D": {"type": "Kinematic", "shape": "Capsule", "radius": 0.3, "half_height": 0.6},
                    "Health3D": {"max": 40, "team": 4, "destroy_on_death": 0},
                    "MapMarker": {"icon": 0, "color": [1, 0.2, 0.2, 1], "label": "Target", "always_visible": 1}})
                s.add(title + " target model", {"ChildOf": {"parent": s.ref(man)}, "LocalTransform": tf((0, -0.9, 0), (CHAR_SCALE,) * 3, math.pi),
                                                "MeshRef": model_mesh("character-e")})
                obj = {"kind": "Kill", "target": s.ref(man), "count": 1}
            else:
                x, z = (stop[1], stop[2]) if what == "interact" else stop
                dest = pad(s, "%s stop %d" % (title, n + 1), x, z, 2.5 if what == "interact" else 5.0, tint,
                           {"MapMarker": {"icon": 0, "color": list(tint) + [1], "label": label}})
                obj = {"kind": "Interact", "target": s.ref(dest), "radius": 2.5} if what == "interact" else \
                      {"kind": "ReachZone", "target": s.ref(dest), "radius": 4.0}
            objectives.append(s.add("%s objective %d" % (title, n + 1), {"Objective": obj, "SaveKey": save_key()}))
        for a, b in zip(objectives, objectives[1:]):
            s.ents[a]["components"]["Objective"]["next"] = s.ref(b)
        comps = {"Mission": {"id": title.lower().replace(" ", "_"), "objective": s.ref(objectives[0]), "actor": actor,
                             "fail_on_death": actor, "time_limit": limit},
                 "MissionReward3D": {"cash": reward, "fail_fee": fee},
                 "MissionRetry": {"delay": 8},
                 "SaveKey": save_key()}
        ident = title.lower().replace(" ", "_")
        mission_hud(ident, title, len(stops), limit, reward)
        comps["UiScreen"] = {"document": "guid:" + guid("ui/mission_%s.mcui" % ident)}  # reads Mission/* from this entity
        if wanted_fail:
            comps["MissionWantedFail"] = {"level": wanted_fail}
        m = s.add("Mission " + title, comps)
        pad(s, "Mission " + title + " marker", start[0], start[1], 4.0, (1.0, 0.85, 0.1),
            {"MissionMarker": {"mission": s.ref(m), "radius": 3.0},
             "MapMarker": {"icon": 0, "color": [1, 0.85, 0.1, 1], "label": title, "always_visible": 1}})
        return m

    # Three jobs of rising length and pay. A failed one (time, death, police at the level) costs a fee and can be run again after 8 s.
    mission("Errand", (-45.0, -9.0), [(95.0, -9.0)], 100, 250, 0, 0, (0.3, 0.7, 1.0))
    mission("Courier", (60.0, 9.0), [(-120.0, 9.0), (150.0, -9.0)], 170, 700, 100, 3, (0.4, 1.0, 0.5))
    mission("Long haul", (-190.0, -9.0), [(180.0, 9.0)], 150, 1000, 200, 3, (1.0, 0.45, 0.3))

    # Three more kinds of job: kill a named man, kill three police officers yourself (raise your wanted level first), and fetch a package
    # (stand on the pad and press E, the pad stays more than 6 m from any parked car because E also gets into one).
    mission("Hit", (20.0, -9.0), [("kill_target", 165.0, 9.0), (-150.0, 9.0)], 180, 900, 150, 0, (1.0, 0.25, 0.25))
    mission("Cop killer", (-80.0, 9.0), [("kill_team", 2, 3), (200.0, -9.0)], 240, 1200, 200, 0, (0.55, 0.45, 1.0))
    mission("Pickup", (-20.0, -9.0), [("interact", -130.0, -9.0), (100.0, 9.0)], 150, 400, 50, 3, (0.95, 0.9, 0.3))

    # Money lying around: a pad each, back after two minutes.
    rng = random.Random(2027)
    spots = [(-170, 9), (-140, -9), (-95, 9), (-20, 9), (-10, -9), (-35, 9), (75, -9), (85, 9), (110, -9), (140, 9), (170, -9), (200, 9)]
    for n, (x, z) in enumerate(spots):
        cash = rng.choice((40, 60, 80, 100))
        pad(s, "Cash %d" % (n + 1), x, z, 1.2, (0.35, 0.9, 0.3),
            {"CashPickup3D": {"cash": cash, "radius": 1.6, "respawn": 120},
             "MapMarker": {"icon": 0, "color": [0.35, 0.9, 0.3, 1], "label": "$"}})

    # Shops. Medic: a hurt player pays 1 per hit point. Scrapyard: a stolen car (cars 2 and 3) driven here pays 400.
    pad(s, "Medic", -100.0, -9.0, 4.0, (0.95, 0.95, 0.95),
        {"HealShop3D": {"radius": 3.0, "price_per_hp": 1}, "MapMarker": {"icon": 0, "color": [1, 0.3, 0.3, 1], "label": "Medic", "always_visible": 1}})
    pad(s, "Scrapyard", 130.0, -9.0, 8.0, (0.6, 0.4, 0.2),
        {"ChopShop3D": {"radius": 6.0, "price": 400}, "MapMarker": {"icon": 0, "color": [1, 0.6, 0.1, 1], "label": "Scrapyard", "always_visible": 1}})
    # Repair shops (RepairShop3D): park a car on the pad (nearly stopped) and it is mended, a wrecked one drives again, for 150; with no
    # police in sight the wanted level drops too, 100 per level (pay and spray).
    for n, (x, z) in enumerate(((-55.0, 7.0), (-175.0, -7.0)), 1):
        pad(s, "Repair shop %d" % n, x, z, 8.0, (0.25, 0.5, 0.85),
            {"RepairShop3D": {"radius": 6.0, "price": 150, "price_per_level": 100, "wanted_drop": 0},
             "MapMarker": {"icon": 0, "color": [0.3, 0.6, 1, 1], "label": "Repair", "always_visible": 1}})


# --- the scene ------------------------------------------------------------------------------------------

def scene():
    rng = random.Random(1009)
    s = Scene()
    s.add("Environment", {"Environment3D": {"sun_direction": [-0.45, -0.8, -0.35], "sun_intensity": 3.0, "shadow_distance": 140,
                                            "fog_density": 0.0002, "exposure": 1.0, "sky_enabled": 0}, "LocalTransform": {}})
    net = s.add("Road network", {"LocalTransform": {}, "RoadNetwork3D": {"roads": "guid:" + G_ROADS}})
    node_half = city_surface(s)
    street_furniture(s, rng, node_half)
    n_buildings = buildings(s, rng)

    # The player, on the sidewalk of the middle avenue, with the car to take two steps away.
    player_start = (-60.0, 0.95, -10.0)
    player = s.add("Player", {
        "LocalTransform": tf(player_start),
        "CharacterController3D": {"radius": 0.3, "half_height": 0.6, "speed": 5.5, "use_input": True, "sprint_multiplier": 1.7,
                                  "acceleration": 40, "deceleration": 40, "layer": 1, "mask": 65535},
        "Rider3D": {"use_input": 1},
        "Health3D": {"max": 100, "team": 1, "regen": 4, "regen_delay": 5},
        "Wanted3D": {},
        "SaveKey": {"key": 999},  # the pause menu's SAVE / LOAD (mission entities count up from 1001)
        # Busted (arrested) or wasted (dead): wakes up at the station or hospital after 3 s, fined from the wallet, wanted level cleared.
        "Respawn3D": {"delay": 3, "fine_busted": 100, "fine_wasted": 200},
        # An arrest costs half the spare ammunition (Confiscate3D), a shop sells it back (AmmoShop3D).
        "Confiscate3D": {"ammo_kept": 0.5},
        "Wallet3D": {"cash": 500},
        # The bag: killed walkers drop bandages and medkits (items/loot.mcitems); H uses the first one. The HUD lists it.
        "Inventory": {"catalog": "guid:" + G_ITEMS},
        "ItemHotkey3D": {},
        "StreamingFocus": {},
        "FootstepAudio": {"clip": "guid:" + G_FOOTSTEPS, "stride": 1.9, "gain_db": -4},
        # The gun is the player's (so the shop and the confiscation see it) but aims along the camera: shots leave from the
        # camera pose and its muzzle offset, past the player, and point where the camera looks. aim_from is set below.
        "Weapon3D": {"kind": "Hitscan", "use_input": 1, "automatic": 1, "damage": 22, "range": 120, "rate": 6,
                     "magazine": 15, "reserve": 150, "reload_time": 1.4, "spread_deg": 0.4, "impulse": 40, "muzzle": [0, 0, -6.0],
                     "mask": 0xFFFF},
        "WeaponAudio": {"shot": "guid:" + G_SHOTS, "hit": "guid:" + G_HITS, "gain_db": -2, "hit_gain_db": -4},
        # The HUD document (health, wanted level, cash, ammo, minimap); bound fields are read from this entity.
        "UiScreen": {"document": "guid:" + G_HUD},
    })
    s.add("Player model", {"ChildOf": {"parent": s.ref(player)}, "LocalTransform": tf((0, -0.9, 0), (CHAR_SCALE,) * 3, math.pi),
                           "MeshRef": model_mesh("character-a")})
    # The camera orbits the player (Look); the player's gun aims along its view. It starts looking west along
    # the avenue (yaw pi/2: behind the player on +x), so the first frame shows the road, its traffic and walkers and the car
    # to take; at yaw 0 it hung over the avenue looking north into a block and no car or walker was ever in frame.
    cam = s.add("Camera", {
        "LocalTransform": {},
        "Camera3DComponent": {"fov_deg": 65, "far_z": 1500},
        "AudioListener": {},
        "CameraShake3D": {},
        # Shot feedback (FireOn Fired names the camera the gun aims from): a muzzle flash where the shot leaves, and a small kick.
        "ParticleEmitter3D": {"color_start": [6.0, 4.0, 1.2, 1.0], "color_end": [1.5, 0.4, 0.05, 0], "offset": [0.25, -0.3, -5.8],
                              "box": [0.03, 0.03, 0.03], "direction": [0, 0, -1], "gravity": [0, 0, 0], "spread_deg": 25, "rate": 0,
                              "speed_min": 1.0, "speed_max": 3.0, "lifetime_min": 0.05, "lifetime_max": 0.1, "drag": 0,
                              "size_start": 0.35, "size_end": 0.05, "max_particles": 64, "seed": 5, "collide": 0, "additive": 1, "emitting": 1},
        "ParticleBurst": {"count": 6, "on": "Fired"},
        "ShakeTrigger": {"trauma": 0.04, "on": "Fired"},
        "CameraFollow3D": {"target": s.ref(player), "offset": [0, 1.6, 0], "distance": 5.5, "yaw": math.pi / 2, "pitch": 0.3, "min_pitch": -0.3,
                           "max_pitch": 1.2, "orbit_speed": 2.2, "smoothing": 12, "collide": True,
                           "chase": 1, "chase_rate": 2.5, "chase_hold": 1.5, "chase_min_speed": 3},
    })
    s.ents[player]["components"]["Rider3D"]["camera"] = s.ref(cam)
    s.ents[player]["components"]["Weapon3D"]["aim_from"] = s.ref(cam)

    # Parked cars on the avenue's sidewalk (walkers turn round at them): take the first one.
    for n, (x, z, yaw) in enumerate(((-72.0, -9.2, math.pi / 2), (-110.0, 9.2, -math.pi / 2), (120.0, 9.2, -math.pi / 2))):
        comps = {c: v for c, v in car_entities(CAR_MODELS[n % len(CAR_MODELS)], False)[0]["components"].items()}
        comps["LocalTransform"] = tf((x, 0.9, z), None, yaw)
        if n > 0:
            # Cars 2 and 3 belong to someone: taking one is a crime when police see it (M-CARJACK), and a scrapyard pays for it (M-CHOPSHOP).
            comps["CarOwner3D"] = {"heat": 12, "needs_witness": 1}
        comps["MapMarker"] = {"color": [0.3, 0.6, 1.0, 1], "label": "Car"}
        car = s.add("Parked car %d" % (n + 1), comps)
        for part in car_entities(CAR_MODELS[n % len(CAR_MODELS)], False)[1:]:
            p = dict(part["components"])
            p["ChildOf"] = {"parent": s.ref(car)}
            s.add("Parked car %d %s" % (n + 1, part["name"]), p)

    # Police: a squad on the plaza at the middle junction. They idle until the player has a wanted level.
    for n in range(4):
        angle = n * math.pi / 2 + 0.4
        px, pz = 22 * math.cos(angle), 22 * math.sin(angle)
        cop = s.add("Police %d" % (n + 1), {
            "LocalTransform": tf((px, 0.95, pz)),
            "CharacterController3D": {"radius": 0.3, "half_height": 0.6, "speed": 4.5, "layer": 2, "mask": 65535},
            "NavAgent3D": {"speed": 4.5, "face_motion": 1},
            "Health3D": {"max": 80, "team": 2, "destroy_on_death": 1, "linger": 4},
            "Police3D": {"min_level": 1, "attack_level": 2, "sight_range": 55, "attack_range": 28, "search_time": 12},
            "MapMarker": {"color": [1.0, 0.2, 0.2, 1], "layer": 0, "always_visible": 1, "label": "Police"},
            "Weapon3D": {"kind": "Hitscan", "damage": 7, "range": 60, "rate": 2, "spread_deg": 3, "magazine": 0},
            "FootstepAudio": {"clip": "guid:" + G_FOOTSTEPS, "stride": 1.7, "gain_db": -6},
            "WeaponAudio": {"shot": "guid:" + G_SHOTS, "hit": "guid:" + G_HITS, "gain_db": -4, "hit_gain_db": -8},
        })
        s.add("Police %d model" % (n + 1), {"ChildOf": {"parent": s.ref(cop)}, "LocalTransform": tf((0, -0.9, 0), (CHAR_SCALE,) * 3, math.pi),
                                            "MeshRef": model_mesh("character-d")})
    # The police navigate where the navmesh is baked: the middle blocks around the plaza.
    s.add("Navmesh", {"LocalTransform": tf((0, 0, 0)),
                      "NavMeshSurface3D": {"bounds_half": [260, 30, 260], "cell_size": 0.5, "cell_height": 0.25, "agent_radius": 0.4,
                                           "agent_height": 1.8, "tile_size": 64, "layer_mask": 65535}})

    # Traffic and crowd gather around the player.
    s.add("Traffic", {"LocalTransform": {}, "TrafficSpawner3D": {
        "network": s.ref(net), "focus": s.ref(player), "prefab": "guid:" + G_CAR, "max_cars": 24, "min_radius": 45,
        "spawn_radius": 170, "despawn_radius": 230, "spacing": 18, "seed": 7}})
    s.add("Crowd", {"LocalTransform": {}, "CrowdSpawner3D": {
        "network": s.ref(net), "focus": s.ref(player), "prefab": "guid:" + G_WALKER, "max_count": 70, "spawn_min": 25,
        "spawn_max": 90, "despawn_radius": 130, "seed": 11}})
    # Added last so the entity order of the older scene (and the spawners' walkers) stays as it was.
    # Police cars: they idle at the plaza until level 2, then drive at the player (and at the last known place when it is lost).
    # They do not shoot or arrest (the foot units do); a car has no path following, so a wall in the way stops it.
    for n, (x, z, yaw) in enumerate(((30.0, 12.0, 0.0), (-30.0, -12.0, math.pi), (12.0, -30.0, math.pi / 2), (-12.0, 30.0, -math.pi / 2))):
        police_car(s, "Police car %d" % (n + 1), (x, 0.9, z), yaw, 2 + n // 2 * 1, 600)
    # Roadblocks across the avenue at both ends of the middle stretch. Each marker spawns two police cars (static bodies, side by side
    # across the road) and two officers behind them when the player reaches wanted level 3 from at least 45 m away, and takes them
    # down 10 s after the level drops (Roadblock3D). The marker's local X runs along world Z; its +Z points away from the plaza, so the officers (officer_back -2.5) stand in front of the cars, in line of fire.
    for n, (x, yaw) in enumerate(((-210.0, -math.pi / 2), (210.0, math.pi / 2))):
        s.add("Roadblock %d" % (n + 1), {
            "LocalTransform": tf((x, 0.95, 0.0), None, yaw),
            "Roadblock3D": {"car": "guid:" + G_BLOCK_CAR, "officer": "guid:" + G_BLOCK_COP, "min_level": 3, "cars": 2, "officers": 2,
                            "car_spacing": 3.6, "officer_back": -2.5, "spawn_min": 45, "spawn_max": 300, "hold": 10}})
    # Where a dead player wakes up (hospital, west avenue) and an arrested one (station, by the plaza), each marked by a flat pad.
    for name, kind, pos, tint in (("Hospital", "Hospital", (-150.0, 9.0), (0.9, 0.9, 0.95)), ("Police station", "Station", (40.0, -9.0), (0.15, 0.25, 0.7))):
        s.add(name, {"LocalTransform": tf((pos[0], 1.0, pos[1])), "RespawnPoint3D": {"kind": kind}})
        s.add(name + " pad", {"LocalTransform": tf((pos[0], 0.22, pos[1]), (5.0, 0.04, 5.0)), "MeshRef": mesh(tint, 0)})
    # An ammo shop on the avenue's sidewalk: standing on its pad refills the spare rounds at 2 cash each (AmmoShop3D).
    s.add("Ammo shop", {"LocalTransform": tf((-30.0, 1.0, -9.0)), "AmmoShop3D": {"radius": 3, "price_per_round": 2, "stock_to": 150},
                        "MapMarker": {"color": [1.0, 0.8, 0.2, 1], "label": "Ammo"}})
    s.add("Ammo shop pad", {"LocalTransform": tf((-30.0, 0.22, -9.0), (4.0, 0.04, 4.0)), "MeshRef": mesh((0.9, 0.7, 0.15), 0)})
    # The mix (bus layout: Master > SFX (engines, shots, steps, sirens, ambience); Music and Voice stay free for E7/E8): a limiter
    # on the master, the pause snapshot (the world goes muffled and quieter behind the pause screen) and the city bed.
    s.add("Mixer", {"AudioLimiter": {"ceiling_db": -1}})
    s.add("Pause mix", {"AudioSnapshot": {"bus": "SFX", "while_paused": True, "fade_s": 0.3, "gain_db": -12, "lowpass_hz": 900}})
    s.add("City ambience", {"AudioSource": {"clip": "guid:" + audio_guid("city_ambience"), "bus": "SFX", "loop": True,
                                            "play_on_start": True, "gain_db": -22, "priority": 200, "fade_s": 2}})
    atmosphere(s, net, node_half)
    economy_and_missions(s, player)
    pause_menu(s)
    write("scenes/city.mcscene", s.doc("city", G_SCENE))
    return len(s.ents), n_buildings


def hud_text(name, label, size, anchors, bind=None, fmt=None, color=None, align="Left"):
    w = {"name": name, "kind": "Text", "text": label, "style": {"text_size": size, "text_outline": 2, "text_outline_color": [0, 0, 0, 1]},
         "text_align": align}
    if anchors:
        w["anchors"] = anchors
    if color:
        w["style"]["text_color"] = color
    if bind:
        w["bind"], w["format"] = bind, fmt
    return w


def hud_box(x0, y0, x1, y1, ax=0, ay=0):
    return {"min": [ax, ay], "max": [ax, ay], "offset_min": [x0, y0], "offset_max": [x1, y1]}


def mission_hud(mission_id, title, steps, limit, reward):
    """One mission's HUD (bound to its Mission entity, which carries the UiScreen): while Active (state 1) its name, the stops
    reached, a drain bar and the clock; PASSED (state 2) and FAILED (state 3) banners after. `visible` is driven by Mission/state
    through a Panel's range [lo, hi]."""
    def shown(name, state, children):
        return {"name": name, "kind": "Panel", "visible": False, "style": {"tint": [0, 0, 0, 0]}, "range": [state, state, 0], "value": state,
                "bind": "Mission/state", "anchors": hud_box(24, 60, 500, 170), "children": children}

    def banner(label, size, color):
        w = hud_text("banner", label, size, None, None, None, color)
        w["fill"] = True  # a lone child fills its panel (no stack to size it)
        return w

    gold = [1.0, 0.85, 0.25, 1]
    active = shown("active", 1, [{"name": "rows", "kind": "Stack", "fill": True, "stack": {"axis": "Vertical", "spacing": 4}, "children": [
        hud_text("title", "%s  +$%d" % (title.upper(), reward), 26, None, None, None, gold),
        hud_text("steps", "STEPS 0 / %d" % steps, 22, None, "Mission/done", "STEPS %.0f / " + str(steps), [1, 1, 1, 1]),
        {"name": "bar", "kind": "Slider", "range": [0, limit, 0], "value": 0, "interactable": False, "bind": "Mission/elapsed"},
        hud_text("clock", "TIME 0 / %d s" % limit, 20, None, "Mission/elapsed", "TIME %.0f / " + str(limit) + " s", [0.8, 0.8, 0.8, 1])]}])
    done = shown("passed", 2, [banner("%s PASSED" % title.upper(), 30, [0.45, 1.0, 0.5, 1])])
    failed = shown("failed", 3, [banner("%s FAILED, retry at its pad" % title.upper(), 24, [1.0, 0.35, 0.3, 1])])
    write("ui/mission_%s.mcui" % mission_id, {"format": "mce-ui", "version": 1, "guid": guid("ui/mission_%s.mcui" % mission_id),
                                              "modal": False, "widgets": [active, done, failed]})


G_PAUSE = guid("ui/pause.mcui")
G_OPTIONS = guid("ui/options.mcui")


def menu_button(name, label, action, up=None, down=None, focus=False):
    b = {"name": name, "kind": "Button", "text": label, "style": {"text_size": 24}, "action": action}
    nav = {}
    if up:
        nav["up"] = "veil/menu/" + up
    if down:
        nav["down"] = "veil/menu/" + down
    if nav:
        b["nav"] = nav
    if focus:
        b["default_focus"] = True
    return b


def menu_doc(guid_, title, rows):
    return {"format": "mce-ui", "version": 1, "guid": guid_, "modal": True, "widgets": [
        {"name": "veil", "fill": True, "blocks_pointer": True, "style": {"tint": [0, 0, 0, 0.6]}, "children": [
            {"name": "menu", "kind": "Stack", "at": [0.5, 0.5, 360, 0], "stack": {"spacing": 14}, "children": [
                {"name": "title", "kind": "Text", "text": title, "font_role": "Title", "style": {"text_size": 36}}] + rows}]}]}


def menus():
    """The pause menu (Esc / gamepad Start) and its options page: Resume, Save, Load, Options, Quit; master volume and look speed."""
    items = [("resume", "RESUME", "Resume"), ("save", "SAVE", "Save"), ("load", "LOAD", "Load"),
             ("options", "OPTIONS", "Options"), ("quit", "QUIT TO DESKTOP", "Quit")]
    rows = [menu_button(n, label, act, items[i - 1][0] if i else None, items[i + 1][0] if i + 1 < len(items) else None, i == 0)
            for i, (n, label, act) in enumerate(items)]
    write("ui/pause.mcui", menu_doc(G_PAUSE, "PAUSED", rows))
    write("ui/options.mcui", menu_doc(G_OPTIONS, "OPTIONS", [
        {"name": "volume_label", "kind": "Text", "text": "MASTER VOLUME", "style": {"text_size": 20}},
        {"name": "volume", "kind": "Slider", "range": [-40, 0, 2], "convar": "audio.master_db", "default_focus": True,
         "nav": {"down": "veil/menu/look"}},
        {"name": "look_label", "kind": "Text", "text": "LOOK SPEED", "style": {"text_size": 20}},
        {"name": "look", "kind": "Slider", "range": [0.25, 3, 0.05], "convar": "input.look_sensitivity",
         "nav": {"up": "veil/menu/volume", "down": "veil/menu/back"}},
        menu_button("back", "BACK", "Back", "look")]))


def pause_menu(s):
    """Two hidden screens: the pause menu holds the game while visible; SAVE writes the 'quick' slot, LOAD reads it back
    (on top of this city scene). The options page opens over it and is closed by BACK."""
    def rule(action, act, act2="None", field="", scene_guid=None):
        r = {"when": "Action", "name": action, "act": act, "act2": act2, "field": field}
        if scene_guid:
            r["scene"] = "guid:" + scene_guid
        return r

    rules = [rule("Save", "SaveSlot", "Hide"), rule("Load", "LoadSlot", "Hide", scene_guid=G_SCENE)] + [{}] * 6
    s.add("Pause menu", {"UiScreen": {"document": "guid:" + G_PAUSE, "order": 10, "visible": False},
                         "UiFlow": {"show_on": "Pause", "hide_on": "Resume", "quit_on": "Quit", "pause": True},
                         "UiFlowRules": {"rules": rules}})
    s.add("Options menu", {"UiScreen": {"document": "guid:" + G_OPTIONS, "order": 11, "visible": False},
                           "UiFlow": {"show_on": "Options", "hide_on": "Back", "pause": True, "keep_audio": True}})


def hud():
    """The HUD: health, wanted level, cash and the minimap and the ammo (hud.mcui, bound to the Player and its gun)."""
    def wanted_stars():
        """Five stars at the right: star n is a Panel bound to Wanted3D/level that shows from level n up (range lo = n)."""
        stars = []
        for n in range(1, 6):
            x1 = -16 - (5 - n) * 34
            stars.append({"name": "star%d" % n, "kind": "Panel", "visible": False, "style": {"tint": [0, 0, 0, 0]},
                          "range": [n, 5, 1], "value": n, "bind": "Wanted3D/level", "anchors": hud_box(x1 - 34, 248, x1, 290, 1, 0),
                          "children": [{"name": "glyph", "kind": "Text", "text": "*", "fill": True, "text_align": "Center",
                                        "style": {"text_size": 52, "text_color": [1.0, 0.85, 0.25, 1], "text_outline": 2,
                                                  "text_outline_color": [0, 0, 0, 1]}}]})
        return stars

    write("ui/hud.mcui", {"format": "mce-ui", "version": 1, "guid": G_HUD, "modal": False, "widgets": [
        hud_text("health", "HEALTH 100", 28, hud_box(24, -64, 280, -24, 0, 1), "Health3D/current", "HEALTH %.0f", [0.45, 1.0, 0.5, 1]),
        # The aim point: shots leave the camera along its forward axis, which is the middle of the screen.
        {"name": "crosshair", "kind": "Text", "text": "+", "text_align": "Center", "anchors": hud_box(-20, -20, 20, 20, 0.5, 0.5),
         "style": {"text_size": 34, "text_color": [1, 1, 1, 0.85], "text_outline": 2, "text_outline_color": [0, 0, 0, 1]}},
        hud_text("kills", "KILLS 0", 22, hud_box(24, -96, 280, -68, 0, 1), "Wanted3D/kills", "KILLS %.0f", [1.0, 0.6, 0.45, 1]),
        hud_text("wanted", "WANTED", 22, hud_box(-280, 224, -16, 248, 1, 0), None, None, [1.0, 0.85, 0.25, 1], "Right"),
        *wanted_stars(),
        hud_text("cash", "$ 0", 30, hud_box(-280, 292, -16, 332, 1, 0), "Wallet3D/cash", "$ %.0f", [0.6, 1.0, 0.6, 1], "Right"),
        hud_text("bag", "BAG  (H to use)", 20, hud_box(-280, 338, -16, 362, 1, 0), None, None, [0.8, 0.8, 0.8, 1], "Right"),
        hud_text("slot_1", "", 22, hud_box(-280, 364, -16, 390, 1, 0), None, None, [0.7, 1.0, 0.8, 1], "Right"),
        hud_text("slot_2", "", 22, hud_box(-280, 392, -16, 418, 1, 0), None, None, [0.7, 1.0, 0.8, 1], "Right"),
        hud_text("slot_3", "", 22, hud_box(-280, 420, -16, 446, 1, 0), None, None, [0.7, 1.0, 0.8, 1], "Right"),
        hud_text("objective", "Walk onto a yellow pad to start a mission", 24, hud_box(24, 16, 560, 52), None, None, [1, 1, 1, 1]),
        hud_text("ammo", "AMMO 15", 30, hud_box(-300, -112, -24, -72, 1, 1), "Weapon3D/ammo", "AMMO %.0f", [1, 1, 1, 1], "Right"),
        hud_text("reserve", "/ 150", 24, hud_box(-300, -66, -24, -30, 1, 1), "Weapon3D/reserve", "RESERVE %.0f", [0.8, 0.8, 0.8, 1], "Right"),
        {"name": "mini", "kind": "Minimap", "anchors": hud_box(-216, 16, -16, 216, 1, 0),
         "minimap": {"track": "Player", "radius": 60, "zoom": 1, "rotate_with_heading": False, "clamp_to_rim": True,
                     "background": [0.05, 0.07, 0.06, 0.75]}}]})


models()
surface_assets()
lamp_material()
audio_assets()
hud()
menus()
items_catalog()
input_map()
write("roads/city.mcroads", roads_doc())
prefabs()
entities, nb = scene()
print("09_openworld: %d scene entities, %d buildings" % (entities, nb))
