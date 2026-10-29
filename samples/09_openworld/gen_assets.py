#!/usr/bin/env python3
"""Rebuilds assets/ of the 09_openworld sample (python3 gen_assets.py). The sample is data only.

A 1 km x 1 km city slice (M-OPENWORLD E1): a 5 x 5 junction grid, 250 m apart, with two avenues
(two lanes each way) crossing at the middle and one lane each way elsewhere, traffic lights at the
nine inner junctions, blocks of boxes for buildings and parks, a player on foot with a gun and a
parked car to take, a few police, traffic and a pedestrian crowd that gather around the player.
Cars, characters and buildings are CC0 Kenney models (`mce content add`, see CREDITS.md); road, ground and trees are
tinted boxes of one unit cube, so the rest of the sample stays self-contained.
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
G_AMMO = guid("ui/ammo.mcui")

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
    names = set(CAR_MODELS) | set(CHARACTERS) | {n for n, _ in DOWNTOWN_MODELS + SUBURB_MODELS + RESIDENTIAL_MODELS}
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
        {"name": "Sprint", "bindings": [key("LShift"), {"gamepad_button": "LeftShoulder"}]},
        {"name": "TriggerL", "bindings": [key("LCtrl"), {"gamepad_axis": "LeftTrigger"}]},
        {"name": "Pause", "bindings": [key("Escape"), {"gamepad_button": "Start"}]},
    ]
    ui = [
        {"name": "Navigate", "type": "axis2d", "bindings": [
            key("Right"), key("Left", -1), key("Up", axis="y"), key("Down", -1, "y"),
            key("D"), key("A", -1), key("W", axis="y"), key("S", -1, "y")]},
        {"name": "Confirm", "bindings": [key("Return"), key("Space"), {"gamepad_button": "South"}]},
        {"name": "Cancel", "bindings": [key("Escape"), {"gamepad_button": "East"}]},
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

AUDIO_FILES = {"engine_loop": ("wav", {}), "shot_1": ("wav", {}), "shot_2": ("wav", {}), "siren": ("wav", {}),
               "city_ambience": ("wav", {"compress": "vorbis", "quality": 0.4})}
FOOTSTEPS = ["footstep_%d" % i for i in range(1, 7)]
AUDIO_EXT = {name: ext for name, (ext, _) in AUDIO_FILES.items()}
AUDIO_EXT.update({name: "ogg" for name in FOOTSTEPS})


def audio_guid(name):
    return guid("audio/%s.%s" % (name, AUDIO_EXT[name]))


G_FOOTSTEPS = guid("audio/footsteps.mcsound")
G_SHOTS = guid("audio/shots.mcsound")


def audio_assets():
    for name, (ext, settings) in AUDIO_FILES.items():
        write("audio/%s.%s.mceasset" % (name, ext), {"guid": audio_guid(name), "importer": ext, "importer_version": 2, "settings": settings})
    for name in FOOTSTEPS:
        write("audio/%s.ogg.mceasset" % name, {"guid": audio_guid(name), "importer": "ogg", "importer_version": 2, "settings": {}})
    write("audio/footsteps.mcsound", {"format": "mce-sound", "version": 1, "guid": G_FOOTSTEPS,
                                      "clips": ["guid:" + audio_guid(n) for n in FOOTSTEPS], "pick": "random", "no_repeat": True,
                                      "pitch": [0.92, 1.08], "gain_db": [-3, 0]})
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


def city_surface(scene):
    # Ground: one big slab, the physics floor for everything.
    half = GRID // 2 * SPACING + 60
    scene.add("Ground", {"LocalTransform": tf((0, -0.5, 0)), "BoxCollider3D": {"half": [half, 0.5, half], "friction": 1.0}})
    scene.add("Ground mesh", {"LocalTransform": tf((0, -0.25, 0), (2 * half, 0.5, 2 * half)), "MeshRef": mesh(GRASS, 0)})
    # Junction pads and the road strips on top of it; sidewalks beside the roads.
    node_half = {}
    for r in road_list():
        for n in road_ends(r):
            node_half[n] = max(node_half.get(n, 0), road_half_width(r))
    for (i, j), h in sorted(node_half.items()):
        x, z = node_pos(i, j)
        pad = 2 * (h + SIDEWALK) + 2
        scene.add("Corner %d_%d" % (i, j), {"LocalTransform": tf((x, 0.05, z), (pad, 0.1, pad)), "MeshRef": mesh(KERB, 0)})
        scene.add("Junction top %d_%d" % (i, j), {"LocalTransform": tf((x, 0.07, z), (2 * h + 2, 0.14, 2 * h + 2)),
                                                  "MeshRef": mesh(ASPHALT, 0)})
    for r in road_list():
        kind, i, j = r
        a, b = road_ends(r)
        (ax, az), (bx, bz) = node_pos(*a), node_pos(*b)
        h = road_half_width(r)
        length = SPACING
        cx, cz = (ax + bx) / 2, (az + bz) / 2
        long_x = kind == "x"
        scene.add("Road " + road_name(r), {"LocalTransform": tf((cx, 0.07, cz), (length if long_x else 2 * h, 0.14, 2 * h if long_x else length)),
                                           "MeshRef": mesh(ASPHALT, 0)})
        # A dashed centre line per 10 m would be 25 boxes a road; one thin strip reads as a double line.
        scene.add("Line " + road_name(r), {"LocalTransform": tf((cx, 0.145, cz), (length - 2 * h if long_x else 0.25, 0.01,
                                                                                 0.25 if long_x else length - 2 * h)),
                                           "MeshRef": mesh((0.85, 0.75, 0.2), 0)})
        for side in (-1, 1):
            off = side * (h + SIDEWALK / 2)
            trim = max(node_half[a], node_half[b])
            span = length - 2 * trim - 0
            scene.add("Sidewalk %s %+d" % (road_name(r), side),
                      {"LocalTransform": tf((cx + (0 if long_x else off), 0.1, cz + (off if long_x else 0)),
                                            (span if long_x else SIDEWALK, 0.2, SIDEWALK if long_x else span)),
                       "MeshRef": mesh(KERB, 0)})
    return node_half


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
                        scene.add(name + " park", {"LocalTransform": tf((cx, 0.05, cz), (lw - 6, 0.1, ld - 6)), "MeshRef": mesh(PARK, 0)})
                        for t in range(3):
                            tx, tz = cx + rng.uniform(-lw / 3, lw / 3), cz + rng.uniform(-ld / 3, ld / 3)
                            prop(scene, name + " tree trunk %d" % t, (tx, 1.5, tz), (0.6, 3.0, 0.6), (0.35, 0.24, 0.14), collider=True)
                            scene.add(name + " crown %d" % t, {"LocalTransform": tf((tx, 4.6, tz), (3.4, 3.0, 3.4)),
                                                              "MeshRef": mesh((0.12, 0.34, 0.12))})
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
                    count += 1
    return count


# --- prefabs --------------------------------------------------------------------------------------------

CAR_COLORS = [(0.7, 0.1, 0.1), (0.1, 0.25, 0.6), (0.85, 0.85, 0.82), (0.12, 0.12, 0.14), (0.8, 0.6, 0.1), (0.15, 0.45, 0.25)]


CAR_SCALE = 0.95
CHAR_SCALE = 0.3   # Blocky Characters are about 6 units tall; the capsule is 1.8 m


def car_entities(model, traffic):
    """A car: a RigidBody3D/Vehicle3D root and the Kenney model as its child."""
    root = {
        "LocalTransform": {},
        "RigidBody3D": {"half": [0.9, 0.4, 1.5], "density": 300},
        "Vehicle3D": {"engine_torque": 900, "self_right": 1, "anti_roll": 8000},
        "VehicleSeat3D": {"seat": [0, 0.2, 0.1], "exit": [-1.7, 0, 0]},
    }
    root.update(engine_audio(traffic))
    if traffic:
        root["TrafficDriver3D"] = {}
        root["VehicleSeat3D"]["locked"] = 1
    else:
        root["VehicleSeat3D"]["enter_radius"] = 4.0
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
    car = scene.add(name, comps)
    scene.add(name + " model", {"ChildOf": {"parent": scene.ref(car)}, "LocalTransform": tf((0, -0.4, 0), (CAR_SCALE,) * 3, math.pi),
                                "MeshRef": model_mesh("police")})
    return car


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
        "Health3D": {"max": 30, "team": 3, "destroy_on_death": 1, "linger": 0.2}}},
        {"name": "Model", "components": {"ChildOf": {"parent": "$0"}, "LocalTransform": tf((0, -0.9, 0), (CHAR_SCALE,) * 3, math.pi),
                                         "MeshRef": model_mesh("character-b")}}]
    write("prefabs/pedestrian.mcprefab", {"format": "mce-prefab", "version": 1, "guid": G_WALKER, "name": "pedestrian",
                                          "entities": walker})


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
        for n, (x, z) in enumerate(stops):
            dest = pad(s, "%s stop %d" % (title, n + 1), x, z, 5.0, tint,
                       {"MapMarker": {"icon": 0, "color": list(tint) + [1], "label": "%s %d" % (title, n + 1)}})
            objectives.append(s.add("%s objective %d" % (title, n + 1), {
                "Objective": {"kind": "ReachZone", "target": s.ref(dest), "radius": 4.0}, "SaveKey": save_key()}))
        for a, b in zip(objectives, objectives[1:]):
            s.ents[a]["components"]["Objective"]["next"] = s.ref(b)
        comps = {"Mission": {"id": title.lower().replace(" ", "_"), "objective": s.ref(objectives[0]), "actor": actor,
                             "fail_on_death": actor, "time_limit": limit},
                 "MissionReward3D": {"cash": reward, "fail_fee": fee},
                 "MissionRetry": {"delay": 8},
                 "SaveKey": save_key()}
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

    # Money lying around: a pad each, back after two minutes.
    rng = random.Random(2027)
    spots = [(-170, 9), (-140, -9), (-95, 9), (-20, 9), (-10, -9), (-35, 9), (75, -9), (85, 9), (110, -9), (140, 9), (170, -9), (200, 9)]
    for n, (x, z) in enumerate(spots):
        cash = rng.choice((40, 60, 80, 100))
        pad(s, "Cash %d" % (n + 1), x, z, 1.2, (0.35, 0.9, 0.3),
            {"CashPickup3D": {"cash": cash, "radius": 1.6, "respawn": 120},
             "MapMarker": {"icon": 0, "color": [0.35, 0.9, 0.3, 1], "label": "$"}})

    # Shops. Medic: a hurt player pays 1 per hit point. Scrapyard: a stolen car (cars 2 and 3) driven here pays 400.
    # (No ammo shop: the gun is on the camera, which carries no wallet.)
    pad(s, "Medic", -100.0, -9.0, 4.0, (0.95, 0.95, 0.95),
        {"HealShop3D": {"radius": 3.0, "price_per_hp": 1}, "MapMarker": {"icon": 0, "color": [1, 0.3, 0.3, 1], "label": "Medic", "always_visible": 1}})
    pad(s, "Scrapyard", 130.0, -9.0, 8.0, (0.6, 0.4, 0.2),
        {"ChopShop3D": {"radius": 6.0, "price": 400}, "MapMarker": {"icon": 0, "color": [1, 0.6, 0.1, 1], "label": "Scrapyard", "always_visible": 1}})


# --- the scene ------------------------------------------------------------------------------------------

def scene():
    rng = random.Random(1009)
    s = Scene()
    s.add("Environment", {"Environment3D": {"sun_direction": [-0.45, -0.8, -0.35], "sun_intensity": 3.0, "shadow_distance": 140,
                                            "fog_density": 0.0002, "exposure": 1.0}, "LocalTransform": {}})
    net = s.add("Road network", {"LocalTransform": {}, "RoadNetwork3D": {"roads": "guid:" + G_ROADS}})
    city_surface(s)
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
        # Busted (arrested) or wasted (dead): wakes up at the station or hospital after 3 s, fined from the wallet, wanted level cleared.
        # (The ammo confiscation component is left out: the gun is on the camera, not on the player.)
        "Respawn3D": {"delay": 3, "fine_busted": 100, "fine_wasted": 200},
        "Wallet3D": {"cash": 500},
        "StreamingFocus": {},
        "FootstepAudio": {"clip": "guid:" + G_FOOTSTEPS, "stride": 1.9, "gain_db": -4},
        # The HUD document (health, wanted level, cash, minimap); bound fields are read from this entity.
        "UiScreen": {"document": "guid:" + G_HUD},
    })
    s.add("Player model", {"ChildOf": {"parent": s.ref(player)}, "LocalTransform": tf((0, -0.9, 0), (CHAR_SCALE,) * 3, math.pi),
                           "MeshRef": model_mesh("character-a")})
    # The camera orbits the player (Look) and carries the gun: shots leave along the camera's view. It starts looking west along
    # the avenue (yaw pi/2: behind the player on +x), so the first frame shows the road, its traffic and walkers and the car
    # to take; at yaw 0 it hung over the avenue looking north into a block and no car or walker was ever in frame.
    cam = s.add("Camera", {
        "LocalTransform": {},
        "Camera3DComponent": {"fov_deg": 65, "far_z": 1500},
        "AudioListener": {},
        "WeaponAudio": {"shot": "guid:" + G_SHOTS, "gain_db": -2},
        "CameraFollow3D": {"target": s.ref(player), "offset": [0, 1.6, 0], "distance": 5.5, "yaw": math.pi / 2, "pitch": 0.3, "min_pitch": -0.3,
                           "max_pitch": 1.2, "orbit_speed": 2.2, "smoothing": 12, "collide": True},
        "Weapon3D": {"kind": "Hitscan", "use_input": 1, "automatic": 1, "owner": s.ref(player), "damage": 22, "range": 120, "rate": 6,
                     "magazine": 15, "reserve": 150, "reload_time": 1.4, "spread_deg": 0.4, "impulse": 40, "muzzle": [0, 0, -6.0],
                     "mask": 0xFFFF},
        # The ammo readout: Weapon3D lives on the camera, so its screen sits here and reads the gun from this entity.
        "UiScreen": {"document": "guid:" + G_AMMO},
    })
    s.ents[player]["components"]["Rider3D"]["camera"] = s.ref(cam)

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
            "WeaponAudio": {"shot": "guid:" + G_SHOTS, "gain_db": -4},
        })
        if n % 2 == 0:   # two of the four carry a siren; it sounds from the wanted level the unit answers (A9 gives it a car)
            s.ents[cop]["components"].update({
                "AudioSource": {"clip": "guid:" + audio_guid("siren"), "bus": "SFX", "loop": True, "play_on_start": True, "gain_db": -60,
                                "priority": 170},
                "AudioSpatial": {"attenuation_id": 1},
                "SirenAudio": {"min_level": 1, "active_db": -9},
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
    # The mix (bus layout: Master > SFX (engines, shots, steps, sirens, ambience); Music and Voice stay free for E7/E8): a limiter
    # on the master, the pause snapshot (the world goes muffled and quieter behind the pause screen) and the city bed.
    s.add("Mixer", {"AudioLimiter": {"ceiling_db": -1}})
    s.add("Pause mix", {"AudioSnapshot": {"bus": "SFX", "while_paused": True, "fade_s": 0.3, "gain_db": -12, "lowpass_hz": 900}})
    s.add("City ambience", {"AudioSource": {"clip": "guid:" + audio_guid("city_ambience"), "bus": "SFX", "loop": True,
                                            "play_on_start": True, "gain_db": -22, "priority": 200, "fade_s": 2}})
    economy_and_missions(s, player)
    write("scenes/city.mcscene", s.doc("city", G_SCENE))
    return len(s.ents), n_buildings


def hud():
    """The HUD: health, wanted level, cash and the minimap (hud.mcui, bound to the Player), and the ammo (ammo.mcui, bound to the camera's gun)."""
    def text(name, label, size, anchors, bind=None, fmt=None, color=None, align="Left"):
        w = {"name": name, "kind": "Text", "text": label, "style": {"text_size": size, "text_outline": 2, "text_outline_color": [0, 0, 0, 1]},
             "text_align": align, "anchors": anchors}
        if color:
            w["style"]["text_color"] = color
        if bind:
            w["bind"], w["format"] = bind, fmt
        return w

    def box(x0, y0, x1, y1, ax=0, ay=0):
        return {"min": [ax, ay], "max": [ax, ay], "offset_min": [x0, y0], "offset_max": [x1, y1]}

    def wanted_stars():
        """Five stars at the right: star n is a Panel bound to Wanted3D/level that shows from level n up (range lo = n)."""
        stars = []
        for n in range(1, 6):
            x1 = -16 - (5 - n) * 34
            stars.append({"name": "star%d" % n, "kind": "Panel", "visible": False, "style": {"tint": [0, 0, 0, 0]},
                          "range": [n, 5, 1], "value": n, "bind": "Wanted3D/level", "anchors": box(x1 - 34, 248, x1, 290, 1, 0),
                          "children": [{"name": "glyph", "kind": "Text", "text": "*", "fill": True, "text_align": "Center",
                                        "style": {"text_size": 52, "text_color": [1.0, 0.85, 0.25, 1], "text_outline": 2,
                                                  "text_outline_color": [0, 0, 0, 1]}}]})
        return stars

    write("ui/hud.mcui", {"format": "mce-ui", "version": 1, "guid": G_HUD, "modal": False, "widgets": [
        text("health", "HEALTH 100", 28, box(24, -64, 280, -24, 0, 1), "Health3D/current", "HEALTH %.0f", [0.45, 1.0, 0.5, 1]),
        text("wanted", "WANTED", 22, box(-280, 224, -16, 248, 1, 0), None, None, [1.0, 0.85, 0.25, 1], "Right"),
        *wanted_stars(),
        text("cash", "$ 0", 30, box(-280, 292, -16, 332, 1, 0), "Wallet3D/cash", "$ %.0f", [0.6, 1.0, 0.6, 1], "Right"),
        text("objective", "OBJECTIVE: take the car on the avenue", 24, box(24, 16, 560, 52), None, None, [1, 1, 1, 1]),
        {"name": "mini", "kind": "Minimap", "anchors": box(-216, 16, -16, 216, 1, 0),
         "minimap": {"track": "Player", "radius": 60, "zoom": 1, "rotate_with_heading": False, "clamp_to_rim": True,
                     "background": [0.05, 0.07, 0.06, 0.75]}}]})
    write("ui/ammo.mcui", {"format": "mce-ui", "version": 1, "guid": G_AMMO, "modal": False, "widgets": [
        text("ammo", "AMMO 15", 30, box(-300, -112, -24, -72, 1, 1), "Weapon3D/ammo", "AMMO %.0f", [1, 1, 1, 1], "Right"),
        text("reserve", "/ 150", 24, box(-300, -66, -24, -30, 1, 1), "Weapon3D/reserve", "RESERVE %.0f", [0.8, 0.8, 0.8, 1], "Right")]})


models()
audio_assets()
hud()
input_map()
write("roads/city.mcroads", roads_doc())
prefabs()
entities, nb = scene()
print("09_openworld: %d scene entities, %d buildings" % (entities, nb))
