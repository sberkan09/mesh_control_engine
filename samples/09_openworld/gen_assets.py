#!/usr/bin/env python3
"""Rebuilds assets/ of the 09_openworld sample (python3 gen_assets.py). The sample is data only.

A 1 km x 1 km city slice (M-OPENWORLD E1): a 5 x 5 junction grid, 250 m apart, with two avenues
(two lanes each way) crossing at the middle and one lane each way elsewhere, traffic lights at the
nine inner junctions, blocks of boxes for buildings and parks, a player on foot with a gun and a
parked car to take, a few police, traffic and a pedestrian crowd that gather around the player.
Everything is placeholder geometry (one unit cube, tinted), so the sample is self-contained; the
open-licence content set of the milestone file replaces the boxes (docs/milestones/M-OPENWORLD.md).
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
G_ROADS = guid("roads/city.mcroads")
G_CAR = guid("prefabs/traffic_car.mcprefab")
G_WALKER = guid("prefabs/pedestrian.mcprefab")
G_INPUT = guid("input/openworld.mcinput")
G_SCENE = guid("scenes/city.mcscene")

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
                        tint = rng.choice(DOWNTOWN)
                    else:
                        h = rng.uniform(5, 13)
                        tint = rng.choice(SUBURB)
                    prop(scene, name, (cx + rng.uniform(-2, 2), h / 2, cz + rng.uniform(-2, 2)), (w, h, d), tint, collider=True)
                    # A roof slab a shade darker, so the skyline has some edge.
                    scene.add(name + " roof", {"LocalTransform": tf((cx, h + 0.4, cz), (w + 0.6, 0.8, d + 0.6)),
                                               "MeshRef": mesh(tuple(c * 0.6 for c in tint))})
                    count += 1
    return count


# --- prefabs --------------------------------------------------------------------------------------------

CAR_COLORS = [(0.7, 0.1, 0.1), (0.1, 0.25, 0.6), (0.85, 0.85, 0.82), (0.12, 0.12, 0.14), (0.8, 0.6, 0.1), (0.15, 0.45, 0.25)]


def car_entities(color, traffic):
    """A sedan: a RigidBody3D/Vehicle3D root and boxes for body, cabin and wheels (children, not spun)."""
    root = {
        "LocalTransform": {},
        "RigidBody3D": {"half": [0.9, 0.4, 1.5], "density": 300},
        "Vehicle3D": {"engine_torque": 900, "self_right": 1, "anti_roll": 8000},
        "VehicleSeat3D": {"seat": [0, 0.2, 0.1], "exit": [-1.7, 0, 0]},
    }
    if traffic:
        root["TrafficDriver3D"] = {}
        root["VehicleSeat3D"]["locked"] = 1
    else:
        root["VehicleSeat3D"]["enter_radius"] = 4.0
    ents = [{"name": "Car", "components": root}]
    parts = [("Body", (0, 0, 0), (1.8, 0.8, 3.0), color), ("Cabin", (0, 0.65, 0.2), (1.6, 0.55, 1.6), tuple(c * 0.45 + 0.1 for c in color)),
             ("Glass", (0, 0.68, 0.2), (1.62, 0.3, 1.2), (0.05, 0.08, 0.1))]
    for n, p, s, c in parts:
        ents.append({"name": n, "components": {"ChildOf": {"parent": "$0"}, "LocalTransform": tf(p, s), "MeshRef": mesh(c)}})
    for n, (x, z) in (("FL", (-0.95, -1.2)), ("FR", (0.95, -1.2)), ("RL", (-0.95, 1.2)), ("RR", (0.95, 1.2))):
        ents.append({"name": "Wheel " + n, "components": {"ChildOf": {"parent": "$0"}, "LocalTransform": tf((x, -0.2, z), (0.25, 0.7, 0.7)),
                                                           "MeshRef": mesh((0.04, 0.04, 0.04))}})
    return ents


def prefabs():
    write("prefabs/traffic_car.mcprefab", {"format": "mce-prefab", "version": 1, "guid": G_CAR, "name": "traffic_car",
                                           "entities": car_entities(CAR_COLORS[0], True)})
    walker = [{"name": "Pedestrian", "components": {
        "LocalTransform": {}, "Pedestrian3D": {},
        "RigidBody3D": {"type": "Kinematic", "shape": "Capsule", "radius": 0.3, "half_height": 0.6},
        "Health3D": {"max": 30, "team": 3, "destroy_on_death": 1, "linger": 0.2}}},
        {"name": "Body", "components": {"ChildOf": {"parent": "$0"}, "LocalTransform": tf((0, 0, 0), (0.5, 1.1, 0.35)),
                                        "MeshRef": mesh((0.25, 0.4, 0.7))}},
        {"name": "Head", "components": {"ChildOf": {"parent": "$0"}, "LocalTransform": tf((0, 0.78, 0), (0.28, 0.28, 0.28)),
                                        "MeshRef": mesh((0.85, 0.65, 0.5))}}]
    write("prefabs/pedestrian.mcprefab", {"format": "mce-prefab", "version": 1, "guid": G_WALKER, "name": "pedestrian",
                                          "entities": walker})


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
        "StreamingFocus": {},
    })
    s.add("Player body", {"ChildOf": {"parent": s.ref(player)}, "LocalTransform": tf((0, 0, 0), (0.55, 1.2, 0.4)),
                          "MeshRef": mesh((0.9, 0.55, 0.15))})
    s.add("Player head", {"ChildOf": {"parent": s.ref(player)}, "LocalTransform": tf((0, 0.78, 0), (0.3, 0.3, 0.3)),
                          "MeshRef": mesh((0.9, 0.72, 0.58))})
    # The camera orbits the player (Look) and carries the gun: shots leave along the camera's view.
    cam = s.add("Camera", {
        "LocalTransform": {},
        "Camera3DComponent": {"fov_deg": 65, "far_z": 1500},
        "CameraFollow3D": {"target": s.ref(player), "offset": [0, 1.6, 0], "distance": 5.5, "pitch": 0.3, "min_pitch": -0.3,
                           "max_pitch": 1.2, "orbit_speed": 2.2, "smoothing": 12, "collide": True},
        "Weapon3D": {"kind": "Hitscan", "use_input": 1, "automatic": 1, "owner": s.ref(player), "damage": 22, "range": 120, "rate": 6,
                     "magazine": 15, "reserve": 150, "reload_time": 1.4, "spread_deg": 0.4, "impulse": 40, "muzzle": [0, 0, -6.0],
                     "mask": 0xFFFF},
    })
    s.ents[player]["components"]["Rider3D"]["camera"] = s.ref(cam)

    # Parked cars on the avenue's sidewalk: take the first one.
    for n, (x, z, yaw) in enumerate(((-72.0, -9.2, math.pi / 2), (-110.0, 9.2, -math.pi / 2), (120.0, 9.2, -math.pi / 2))):
        comps = {c: v for c, v in car_entities(CAR_COLORS[n % len(CAR_COLORS)], False)[0]["components"].items()}
        comps["LocalTransform"] = tf((x, 0.9, z), None, yaw)
        car = s.add("Parked car %d" % (n + 1), comps)
        for part in car_entities(CAR_COLORS[n % len(CAR_COLORS)], False)[1:]:
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
            "Weapon3D": {"kind": "Hitscan", "damage": 7, "range": 60, "rate": 2, "spread_deg": 3, "magazine": 0},
        })
        s.add("Police %d body" % (n + 1), {"ChildOf": {"parent": s.ref(cop)}, "LocalTransform": tf((0, 0, 0), (0.55, 1.2, 0.4)),
                                           "MeshRef": mesh((0.08, 0.15, 0.45))})
        s.add("Police %d head" % (n + 1), {"ChildOf": {"parent": s.ref(cop)}, "LocalTransform": tf((0, 0.78, 0), (0.3, 0.3, 0.3)),
                                           "MeshRef": mesh((0.9, 0.72, 0.58))})
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
    write("scenes/city.mcscene", s.doc("city", G_SCENE))
    return len(s.ents), n_buildings


models()
input_map()
write("roads/city.mcroads", roads_doc())
prefabs()
entities, nb = scene()
print("09_openworld: %d scene entities, %d buildings" % (entities, nb))
