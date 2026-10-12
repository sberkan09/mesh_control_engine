#!/usr/bin/env python3
"""Generates projects/Room3D/assets: the room's models, its two textures and the main scene.

    python3 projects/Room3D/tools/gen_assets.py

Room3D is the minimal 3D example (M-KIT3D): a walled room with crates, pillars, lamps and a fan,
a meadow of rocks around it, and a hero to walk. Everything is drawn by the engine's kit3d
components; the game module only moves the hero and the camera.

Deterministic (no randomness, no clocks) and self-contained (standard library only), so an SDK
copy of the example can regenerate its content. GUIDs are uuid5 of each asset path.
"""

import json
import math
import pathlib
import struct
import uuid
import zlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent / "assets"
NS = uuid.UUID("726f6f6d-3364-2d6d-6365-2d6b69743364")
SCENE_GUID = "0017e8b2-29ec-4fa2-9467-77f636830a19"  # Room3D.mceproj's start_scene
FONT_GUID = None  # read from the font's sidecar


def r6(v):
    return round(v, 6)


def guid_of(rel):
    return str(uuid.uuid5(NS, rel))


# ---- PNG ------------------------------------------------------------------------------------------

def write_png(path, w, h, pixels):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pixels[y * w * 4:(y + 1) * w * 4]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def checker(n, a, b, cells):
    px = bytearray()
    for y in range(n):
        for x in range(n):
            on = ((x * cells // n) + (y * cells // n)) % 2
            c = a if on else b
            # A thin grout line around each cell.
            if (x * cells) % n < cells * 2 or (y * cells) % n < cells * 2:
                c = tuple(int(v * 0.7) for v in c[:3]) + (255,)
            px += bytes(c)
    return px


def planks(n):
    px = bytearray()
    for y in range(n):
        for x in range(n):
            board = (y * 6) // n
            grain = 0.85 + 0.15 * math.sin(x * 0.21 + board * 1.7) * math.sin(y * 0.05 + x * 0.013)
            edge = 0.6 if (y * 6) % n < 6 or x < 3 or x > n - 4 else 1.0
            r, g, b = 150 * grain * edge, 102 * grain * edge, 60 * grain * edge
            px += bytes((int(r), int(g), int(b), 255))
    return px


# ---- glTF -----------------------------------------------------------------------------------------

class Gltf:
    def __init__(self):
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "mce Room3D gen_assets.py"}, "scene": 0,
                    "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "materials": [], "accessors": [],
                    "bufferViews": [], "buffers": []}
        self.images = {}

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.bin += data
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values, comps, ctype=5126, target=None, minmax=False):
        fmt = {5126: "f", 5123: "H", 5125: "I"}[ctype]
        data = struct.pack("<%d%s" % (len(values), fmt), *values)
        acc = {"bufferView": self._view(data, target), "componentType": ctype, "count": len(values) // comps,
               "type": {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4"}[comps]}
        if minmax:
            acc["min"] = [min(values[i::comps]) for i in range(comps)]
            acc["max"] = [max(values[i::comps]) for i in range(comps)]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def texture(self, uri):
        if uri not in self.images:
            self.doc.setdefault("images", []).append({"uri": uri})
            self.doc.setdefault("samplers", [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}])
            self.doc.setdefault("textures", []).append({"sampler": 0, "source": len(self.doc["images"]) - 1})
            self.images[uri] = len(self.doc["textures"]) - 1
        return self.images[uri]

    def material(self, name, color=(1, 1, 1, 1), rough=0.7, metal=0.0, texture=None, emissive=None):
        pbr = {"baseColorFactor": [r6(c) for c in color], "roughnessFactor": rough, "metallicFactor": metal}
        if texture:
            pbr["baseColorTexture"] = {"index": self.texture(texture)}
        m = {"name": name, "pbrMetallicRoughness": pbr}
        if emissive:
            m["emissiveFactor"] = [r6(c) for c in emissive]
        self.doc["materials"].append(m)
        return len(self.doc["materials"]) - 1

    def mesh(self, name, geo, material):
        pos, nrm, uv, idx = geo
        prim = {"attributes": {"POSITION": self.accessor(pos, 3, target=34962, minmax=True),
                               "NORMAL": self.accessor(nrm, 3, target=34962),
                               "TEXCOORD_0": self.accessor(uv, 2, target=34962)},
                "indices": self.accessor(idx, 1, 5123 if max(idx) < 65536 else 5125, target=34963),
                "material": material}
        self.doc["meshes"].append({"name": name, "primitives": [prim]})
        return len(self.doc["meshes"]) - 1

    def node(self, n, parent=None):
        self.doc["nodes"].append(n)
        i = len(self.doc["nodes"]) - 1
        if parent is None:
            self.doc["scenes"][0]["nodes"].append(i)
        else:
            self.doc["nodes"][parent].setdefault("children", []).append(i)
        return i

    def animation(self, name, channels):
        samplers, chans = [], []
        for node, path, times, values, comps in channels:
            samplers.append({"input": self.accessor(times, 1, minmax=True), "output": self.accessor(values, comps),
                             "interpolation": "LINEAR"})
            chans.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})
        self.doc.setdefault("animations", []).append({"name": name, "samplers": samplers, "channels": chans})

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(self.doc, separators=(",", ":"), sort_keys=True).encode()
        while len(js) % 4:
            js += b" "
        out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin))
        out += struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(self.bin), 0x004E4942) + self.bin
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(out)


# ---- geometry -------------------------------------------------------------------------------------

def box(hx, hy, hz, uv_scale=1.0):
    pos, nrm, uv, idx = [], [], [], []
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)), ((0, 1, 0), (1, 0, 0), (0, 0, -1)),
             ((0, -1, 0), (1, 0, 0), (0, 0, 1)), ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    h = (hx, hy, hz)
    for n, u, v in faces:
        base = len(pos) // 3
        su_len = sum(abs(u[k]) * h[k] for k in range(3)) * 2
        sv_len = sum(abs(v[k]) * h[k] for k in range(3)) * 2
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [n[k] * h[k] + u[k] * h[k] * su + v[k] * h[k] * sv for k in range(3)]
            pos += [r6(c) for c in p]
            nrm += list(n)
            uv += [r6((su * 0.5 + 0.5) * su_len * uv_scale), r6((0.5 - sv * 0.5) * sv_len * uv_scale)]
        idx += [base, base + 1, base + 2, base + 2, base + 3, base]
    return pos, nrm, uv, idx


def sphere(radius, seg, rings, bumps=0.0, seed=0):
    pos, nrm, uv, idx = [], [], [], []
    for r in range(rings + 1):
        th = math.pi * r / rings
        for s in range(seg + 1):
            ph = 2 * math.pi * (s % seg) / seg
            n = (math.sin(th) * math.cos(ph), math.cos(th), -math.sin(th) * math.sin(ph))
            k = 1.0
            if bumps:
                k += bumps * (math.sin(3 * ph + seed) * math.sin(2 * th + seed * 0.7) + 0.5 * math.sin(7 * ph - 5 * th))
            pos += [r6(radius * k * c) for c in n]
            nrm += [r6(c) for c in n]
            uv += [r6(s / seg), r6(r / rings)]
    for r in range(rings):
        for s in range(seg):
            a, b = r * (seg + 1) + s, (r + 1) * (seg + 1) + s
            if r != 0:
                idx += [a, b, a + 1]
            if r != rings - 1:
                idx += [a + 1, b, b + 1]
    return pos, nrm, uv, idx


def cylinder(radius, half, seg):
    pos, nrm, uv, idx = [], [], [], []
    for s in range(seg + 1):
        ph = 2 * math.pi * (s % seg) / seg
        c, sn = math.cos(ph), -math.sin(ph)
        for y in (-half, half):
            pos += [r6(radius * c), r6(y), r6(radius * sn)]
            nrm += [r6(c), 0.0, r6(sn)]
            uv += [r6(s / seg), 0.0 if y < 0 else 1.0]
    for s in range(seg):
        a = s * 2
        idx += [a, a + 2, a + 1, a + 1, a + 2, a + 3]
    return pos, nrm, uv, idx


def quat_axis(axis, angle):
    s = math.sin(angle / 2)
    return [r6(axis[0] * s), r6(axis[1] * s), r6(axis[2] * s), r6(math.cos(angle / 2))]


# ---- models ----------------------------------------------------------------------------------------

def build_models():
    models = {}

    g = Gltf()
    m = g.material("floor", texture="../textures/floor.png", rough=0.85)
    g.node({"name": "floor", "mesh": g.mesh("floor", box(12, 0.25, 12, 0.25), m), "translation": [0, -0.25, 0]})
    models["floor.glb"] = g

    g = Gltf()
    m = g.material("meadow", color=(0.32, 0.45, 0.22, 1), rough=0.95)
    g.node({"name": "meadow", "mesh": g.mesh("meadow", box(150, 0.5, 150, 0.05), m), "translation": [0, -0.5, 0]})
    models["meadow.glb"] = g

    g = Gltf()
    m = g.material("plaster", color=(0.86, 0.82, 0.74, 1), rough=0.9)
    g.node({"name": "wall", "mesh": g.mesh("wall", box(6, 1.5, 0.25, 0.5), m), "translation": [0, 1.5, 0]})
    models["wall.glb"] = g

    g = Gltf()
    m = g.material("crate", texture="../textures/crate.png", rough=0.8)
    g.node({"name": "crate", "mesh": g.mesh("crate", box(0.5, 0.5, 0.5, 1.0), m), "translation": [0, 0.5, 0]})
    models["crate.glb"] = g

    g = Gltf()
    m = g.material("stone", color=(0.6, 0.6, 0.62, 1), rough=0.7)
    g.node({"name": "pillar", "mesh": g.mesh("pillar", cylinder(0.35, 1.5, 24), m), "translation": [0, 1.5, 0]})
    models["pillar.glb"] = g

    g = Gltf()
    pole = g.material("iron", color=(0.15, 0.15, 0.16, 1), rough=0.5, metal=1.0)
    glow = g.material("glow", color=(1, 0.85, 0.6, 1), emissive=(4.0, 3.0, 1.6))
    lamp = g.node({"name": "lamp"})
    g.node({"name": "pole", "mesh": g.mesh("pole", cylinder(0.05, 1.0, 12), pole), "translation": [0, 1.0, 0]}, lamp)
    g.node({"name": "bulb", "mesh": g.mesh("bulb", sphere(0.16, 16, 8), glow), "translation": [0, 2.1, 0]}, lamp)
    models["lamp.glb"] = g

    # The fan: a hub and four blades on a node the "spin" clip turns.
    g = Gltf()
    metal = g.material("brass", color=(0.8, 0.62, 0.3, 1), rough=0.35, metal=1.0)
    fan = g.node({"name": "fan", "translation": [0, 2.6, 0]})
    rotor = g.node({"name": "rotor"}, fan)
    g.node({"name": "hub", "mesh": g.mesh("hub", sphere(0.12, 12, 6), metal)}, rotor)
    blade = None
    for k in range(4):
        a = k * math.pi / 2
        blade_mesh = blade if blade is not None else g.mesh("blade", box(0.5, 0.02, 0.09), metal)
        blade = blade_mesh
        g.node({"name": "blade%d" % k, "mesh": blade_mesh, "rotation": quat_axis((0, 1, 0), a),
                "translation": [r6(0.55 * math.cos(a)), 0, r6(-0.55 * math.sin(a))]}, rotor)
    times = [r6(0.25 * k) for k in range(5)]
    values = []
    for k in range(5):
        values += quat_axis((0, 1, 0), 2 * math.pi * k / 4)
    g.animation("spin", [(rotor, "rotation", times, values, 4)])
    models["fan.glb"] = g

    # The hero: feet at the origin; "idle" breathes, "walk" swings the arms and bobs the body.
    g = Gltf()
    cloth = g.material("cloth", color=(0.2, 0.36, 0.7, 1), rough=0.8)
    skin = g.material("skin", color=(0.95, 0.75, 0.6, 1), rough=0.6)
    root = g.node({"name": "hero"})
    body = g.node({"name": "body", "translation": [0, 0.95, 0]}, root)
    g.node({"name": "torso", "mesh": g.mesh("torso", box(0.26, 0.42, 0.16), cloth)}, body)
    g.node({"name": "head", "mesh": g.mesh("head", sphere(0.18, 16, 10), skin), "translation": [0, 0.62, 0]}, body)
    arm_l = g.node({"name": "arm_l", "translation": [0.34, 0.36, 0]}, body)
    arm_r = g.node({"name": "arm_r", "translation": [-0.34, 0.36, 0]}, body)
    arm = g.mesh("arm", box(0.07, 0.34, 0.07), cloth)
    g.node({"name": "arm_l_mesh", "mesh": arm, "translation": [0, -0.34, 0]}, arm_l)
    g.node({"name": "arm_r_mesh", "mesh": arm, "translation": [0, -0.34, 0]}, arm_r)
    leg = g.mesh("leg", box(0.09, 0.42, 0.09), cloth)
    g.node({"name": "leg_l", "mesh": leg, "translation": [0.13, -0.42 - 0.1, 0]}, body)
    g.node({"name": "leg_r", "mesh": leg, "translation": [-0.13, -0.42 - 0.1, 0]}, body)
    t = [0.0, 0.25, 0.5, 0.75, 1.0]
    swing = lambda sign: sum((quat_axis((1, 0, 0), sign * 0.6 * math.sin(2 * math.pi * k / 4)) for k in range(5)), [])
    bob = sum(([0, r6(0.95 + 0.04 * abs(math.sin(2 * math.pi * k / 4))), 0] for k in range(5)), [])
    g.animation("walk", [(arm_l, "rotation", t, swing(1), 4), (arm_r, "rotation", t, swing(-1), 4),
                         (body, "translation", t, bob, 3)])
    breathe = sum(([0, r6(0.95 + 0.015 * math.sin(2 * math.pi * k / 4)), 0] for k in range(5)), [])
    g.animation("idle", [(body, "translation", [r6(2.0 * k / 4) for k in range(5)], breathe, 3)])
    models["hero.glb"] = g

    # A dense rock (≈ 3,000 triangles): the meadow's scatter, the LOD gate's far view.
    g = Gltf()
    m = g.material("rock", color=(0.5, 0.48, 0.45, 1), rough=0.9)
    g.node({"name": "rock", "mesh": g.mesh("rock", sphere(1.0, 48, 32, bumps=0.12, seed=1.3), m),
            "translation": [0, 0.5, 0], "scale": [1, 0.7, 1]})
    models["rock.glb"] = g
    return models


def tri_count(g):
    n = 0
    for mesh in g.doc["meshes"]:
        for p in mesh["primitives"]:
            n += g.doc["accessors"][p["indices"]]["count"] // 3
    return n


def sidecar(path, importer, version, settings):
    rel = path.relative_to(ROOT).as_posix()
    doc = {"guid": guid_of(rel), "importer": importer, "importer_version": version, "settings": settings}
    pathlib.Path(str(path) + ".mceasset").write_text(json.dumps(doc, indent=2) + "\n")
    return doc["guid"]


# ---- the scene --------------------------------------------------------------------------------------

def ref(guid):
    return "guid:" + guid


def yaw(deg):
    return quat_axis((0, 1, 0), math.radians(deg))


def build_scene(g, font):
    ents = []

    def add(name, **components):
        ents.append({"name": name, "components": components})

    add("Environment", Environment3D={
        "sun_direction": [-0.45, -0.8, -0.35], "sun_intensity": 2.6, "sun_color": [1.0, 0.93, 0.82],
        "ambient_sky": [0.32, 0.38, 0.5], "ambient_ground": [0.18, 0.16, 0.12], "exposure": 1.1,
        "fog_density": 0.006, "fog_color": [0.62, 0.7, 0.82], "shadow_cascades": 3, "shadow_distance": 50,
        "bloom_intensity": 0.06})
    add("Camera", Camera3DComponent={"fov_deg": 60, "near_z": 0.05, "far_z": 400, "mouse_look": 1},
        LocalTransform={"position": [0, 2.5, 9]}, FollowCamera={"distance": 5.0, "height": 1.6, "pitch": 0.3})
    add("Hud", Camera2DComponent={"pixels_per_meter": 48})
    add("Title", LocalTransform={"position": [0, 6.6, 0]},
        TextLabel={"font": ref(font), "text": "ROOM 3D   WASD MOVE  MOUSE LOOK  SPACE JUMP", "color": [1, 0.95, 0.8, 1],
                   "height": 0.45})
    add("Hero", LocalTransform={"position": [0, 0, 4]}, MeshRef={"mesh": ref(g["hero.glb"]), "interpolate": 1},
        Animator={"clip": "idle", "fade": 0.2}, Hero={})
    add("Floor", LocalTransform={}, MeshRef={"mesh": ref(g["floor.glb"])},
        BoxCollider3D={"half": [12, 0.25, 12], "offset": [0, -0.25, 0]})
    add("Meadow", LocalTransform={"position": [0, -0.02, 0]}, MeshRef={"mesh": ref(g["meadow.glb"]), "cast_shadows": 0},
        BoxCollider3D={"half": [150, 0.5, 150], "offset": [0, -0.5, 0]})
    # Walls: north and south full, east and west with a doorway.
    for name, pos, deg in (("Wall N", [0, 0, -12], 0), ("Wall N2", [6, 0, -12], 0), ("Wall S", [-6, 0, 12], 0),
                           ("Wall E", [12, 0, -6], 90), ("Wall W", [-12, 0, 6], 90)):
        add(name, LocalTransform={"position": pos, "rotation": yaw(deg)}, MeshRef={"mesh": ref(g["wall.glb"])},
            BoxCollider3D={"half": [6, 1.5, 0.25], "offset": [0, 1.5, 0]})
    for i, (x, z, d) in enumerate(((-4, -4, 10), (-3, -4.2, 0), (-3.5, -3, 35), (5, 3, 20), (6, -6, 0), (5.2, -6.4, 15))):
        add("Crate %d" % (i + 1), LocalTransform={"position": [x, 0, z], "rotation": yaw(d)},
            MeshRef={"mesh": ref(g["crate.glb"])}, BoxCollider3D={"half": [0.5, 0.5, 0.5], "offset": [0, 0.5, 0]})
    add("Crate stack", LocalTransform={"position": [-3.4, 1, -4.1], "rotation": yaw(20)},
        MeshRef={"mesh": ref(g["crate.glb"])}, BoxCollider3D={"half": [0.5, 0.5, 0.5], "offset": [0, 0.5, 0]})
    for i, (x, z) in enumerate(((-8, -8), (8, -8), (-8, 8), (8, 8))):
        add("Pillar %d" % (i + 1), LocalTransform={"position": [x, 0, z]}, MeshRef={"mesh": ref(g["pillar.glb"])},
            BoxCollider3D={"half": [0.35, 1.5, 0.35], "offset": [0, 1.5, 0]})
    for i, (x, z, c) in enumerate(((-6, 0, [1, 0.7, 0.4, 1]), (6, 0, [1, 0.7, 0.4, 1]), (0, -9, [0.6, 0.8, 1, 1]))):
        add("Lamp %d" % (i + 1), LocalTransform={"position": [x, 0, z]}, MeshRef={"mesh": ref(g["lamp.glb"])},
            PointLight3D={"color": c, "offset": [0, 2.1, 0], "intensity": 4, "radius": 9})
    add("Fan", LocalTransform={"position": [0, 0, 0]}, MeshRef={"mesh": ref(g["fan.glb"])},
        Animator={"clip": "spin", "speed": 0.6},
        SpotLight3D={"offset": [0, 2.4, 0], "direction": [0, -1, 0], "intensity": 8, "radius": 6, "outer_deg": 35,
                     "inner_deg": 22})
    add("Rocks", LocalTransform={}, RockField={"count": 900, "inner": 16, "outer": 140, "seed": 7,
                                              "rock": ref(g["rock.glb"])})
    for i, e in enumerate(ents):
        e["id"] = i + 1
    return {"format": "mce-scene", "version": 1, "guid": SCENE_GUID, "name": "main", "entities": ents}


def main():
    textures = ROOT / "textures"
    write_png(textures / "floor.png", 128, 128, checker(128, (196, 182, 160, 255), (160, 146, 126, 255), 4))
    sidecar(textures / "floor.png", "texture", 1, {"srgb": True})
    write_png(textures / "crate.png", 128, 128, planks(128))
    sidecar(textures / "crate.png", "texture", 1, {"srgb": True})
    guids = {}
    for name, g in build_models().items():
        g.write(ROOT / "models" / name)
        guids[name] = sidecar(ROOT / "models" / name, "gltf", 2, {"max_triangles": max(64, tri_count(g) + 64)})
    font = json.loads((ROOT / "fonts" / "ProggyClean.ttf.mceasset").read_text())["guid"]
    scene = build_scene(guids, font)
    (ROOT / "scenes" / "main.mcscene").write_text(json.dumps(scene, indent=2) + "\n")
    print("Room3D assets written to", ROOT)


if __name__ == "__main__":
    main()
