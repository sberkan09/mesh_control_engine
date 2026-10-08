#!/usr/bin/env python3
"""Generates samples/04_cubes/assets: every model and texture of the M5a demo, procedurally.

    python3 samples/04_cubes/tools/gen_assets.py

Deterministic (no randomness, no clocks): re-running reproduces the committed bytes. Standard
library only (struct, zlib, json, math, uuid).

  models/sphere_row.glb   2 x 7 material spheres: dielectric and metal rows, roughness 0 -> 1
  models/floor.glb        30 m tiled floor: base color + normal + ORM (AO in R) maps
  models/panel_cube.glb   1 m cube: base color + normal + ORM + emissive maps (emissive strength 6)
  models/orrery.glb       5-node hierarchy, two clips ("spin": LINEAR rotations on three nodes +
                          CUBICSPLINE bob; "pulse": STEP scale + LINEAR rotation) - multi-node tracks
  models/bulb.glb         low-poly unlit sphere, tinted per point light
  textures/*.png          the maps above (linear maps carry "srgb": false sidecars) + an equirect sky
"""

import json
import math
import pathlib
import struct
import uuid
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1] / "assets"
NS = uuid.UUID("6d63652d-3034-6375-6265-732d6d356100")  # fixed namespace: stable GUIDs per path


# ---------------------------------------------------------------------------------------- PNG
def write_png(path, w, h, pixels):
    """pixels: bytes, RGBA8 rows top first."""
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


def clamp01(v):
    return 0.0 if v < 0 else (1.0 if v > 1 else v)


def to8(v):
    return int(round(clamp01(v) * 255))


def srgb8(v):
    v = clamp01(v)
    return to8(v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055)


def hash01(x, y, seed):
    h = (x * 374761393 + y * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def normal_from_height(hf, w, h, strength):
    """Tangent-space normal map (glTF: +X right, +Y up in the image) from a wrapping height field."""
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            dx = (hf[y * w + (x + 1) % w] - hf[y * w + (x - 1) % w]) * 0.5 * strength
            dy = (hf[((y + 1) % h) * w + x] - hf[((y - 1) % h) * w + x]) * 0.5 * strength
            n = (-dx, dy, 1.0)  # rows grow downward: d/d(up) = -d/d(row)
            l = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
            i = (y * w + x) * 4
            out[i:i + 4] = bytes((to8(n[0] / l * 0.5 + 0.5), to8(n[1] / l * 0.5 + 0.5), to8(n[2] / l * 0.5 + 0.5), 255))
    return out


def tiles_textures(n=256, tiles=4):
    """Floor tiles: per-tile tint, bevelled edges, rough grout."""
    cell = n // tiles
    albedo, orm, height = bytearray(n * n * 4), bytearray(n * n * 4), [0.0] * (n * n)
    for y in range(n):
        for x in range(n):
            tx, ty, u, v = x // cell, y // cell, x % cell, y % cell
            edge = min(u, v, cell - 1 - u, cell - 1 - v)
            grout = edge < 2
            bevel = clamp01((edge - 2) / 4.0)
            tint = 0.85 + 0.15 * hash01(tx, ty, 1)
            speck = 0.94 + 0.06 * hash01(x, y, 2)
            base = (0.42, 0.40, 0.37) if (tx + ty) % 2 else (0.55, 0.53, 0.50)
            if grout:
                c = (0.10, 0.10, 0.10)
            else:
                c = tuple(b * tint * speck for b in base)
            i = (y * n + x) * 4
            albedo[i:i + 4] = bytes((srgb8(c[0]), srgb8(c[1]), srgb8(c[2]), 255))
            ao = 0.45 if grout else 0.75 + 0.25 * bevel
            rough = 0.95 if grout else 0.22 + 0.25 * hash01(tx, ty, 3) + 0.08 * hash01(x // 3, y // 3, 4)
            orm[i:i + 4] = bytes((to8(ao), to8(rough), 0, 255))
            height[y * n + x] = 0.0 if grout else bevel
    return albedo, normal_from_height(height, n, n, 6.0), orm


def panel_textures(n=256):
    """Sci-fi panel: brushed metal plate with bevelled border, painted frame, glowing strips."""
    albedo, orm, emissive, height = bytearray(n * n * 4), bytearray(n * n * 4), bytearray(n * n * 4), [0.0] * (n * n)
    for y in range(n):
        for x in range(n):
            edge = min(x, y, n - 1 - x, n - 1 - y)
            frame = edge < 22
            bevel = clamp01((edge - 22) / 6.0) if not frame else clamp01(edge / 4.0) * 0.6
            strip = (not frame) and (abs(y - n // 2) < 3) and 40 < x < n - 40
            rivet = frame and ((x - 11) % 48 < 5 or (n - 12 - x) % 48 < 5) and ((y - 11) % 48 < 5 or (n - 12 - y) % 48 < 5)
            i = (y * n + x) * 4
            if frame:
                c = (0.55, 0.32, 0.08)
                metal, rough = 0.0, 0.6
            else:
                brushed = 0.9 + 0.1 * hash01(x // 1, y // 9, 5)
                c = (0.62 * brushed, 0.64 * brushed, 0.66 * brushed)
                metal, rough = 1.0, 0.28 + 0.1 * hash01(x // 2, y // 16, 6)
            if rivet:
                c, metal, rough = (0.7, 0.7, 0.72), 1.0, 0.2
            if strip:
                c, metal, rough = (0.05, 0.05, 0.05), 0.0, 0.4
            albedo[i:i + 4] = bytes((srgb8(c[0]), srgb8(c[1]), srgb8(c[2]), 255))
            ao = 0.6 + 0.4 * bevel
            orm[i:i + 4] = bytes((to8(ao), to8(rough), to8(metal), 255))
            e = (1.0, 0.55, 0.15) if strip else (0.0, 0.0, 0.0)
            emissive[i:i + 4] = bytes((srgb8(e[0]), srgb8(e[1]), srgb8(e[2]), 255))
            height[y * n + x] = bevel - (0.5 if strip else 0.0) + (0.3 if rivet else 0.0)
    return albedo, normal_from_height(height, n, n, 4.0), orm, emissive


def sky_equirect(w=512, h=256):
    """An LDR evening sky: horizon glow toward the sun azimuth, darker zenith, dim ground."""
    px = bytearray(w * h * 4)
    sun_az = math.radians(-35.0)
    for y in range(h):
        theta = (y + 0.5) / h * math.pi  # 0 = straight up
        up = math.cos(theta)
        for x in range(w):
            phi = ((x + 0.5) / w - 0.5) * 2 * math.pi
            glow = max(0.0, math.cos(phi - sun_az)) ** 4
            if up >= 0:
                t = up ** 0.45
                c = [0.95 * (1 - t) + 0.12 * t, 0.62 * (1 - t) + 0.22 * t, 0.42 * (1 - t) + 0.48 * t]
                c = [c[0] + 0.35 * glow * (1 - t), c[1] + 0.15 * glow * (1 - t), c[2]]
            else:
                c = [0.16, 0.14, 0.13]
            i = (y * w + x) * 4
            px[i:i + 4] = bytes((srgb8(c[0]), srgb8(c[1]), srgb8(c[2]), 255))
    return px


# ---------------------------------------------------------------------------------------- glTF
class Gltf:
    def __init__(self):
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "mce 04_cubes gen_assets.py"}, "scene": 0,
                    "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "materials": [], "accessors": [],
                    "bufferViews": [], "buffers": []}
        self.image_index = {}

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
        if uri not in self.image_index:
            self.doc.setdefault("images", []).append({"uri": uri})
            self.doc.setdefault("samplers", [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}])
            self.doc.setdefault("textures", []).append({"sampler": 0, "source": len(self.doc["images"]) - 1})
            self.image_index[uri] = len(self.doc["textures"]) - 1
        return self.image_index[uri]

    def material(self, m):
        self.doc["materials"].append(m)
        return len(self.doc["materials"]) - 1

    def geometry(self, geo):
        """Accessors for one vertex/index set; meshes that pass the same dict share the bytes."""
        pos, nrm, uv, idx = geo
        return {"attributes": {"POSITION": self.accessor(pos, 3, target=34962, minmax=True),
                               "NORMAL": self.accessor(nrm, 3, target=34962),
                               "TEXCOORD_0": self.accessor(uv, 2, target=34962)},
                "indices": self.accessor(idx, 1, 5123 if max(idx) < 65536 else 5125, target=34963)}

    def mesh(self, name, geo, material):
        acc = geo if isinstance(geo, dict) else self.geometry(geo)
        prim = {"attributes": dict(acc["attributes"]), "indices": acc["indices"], "material": material}
        self.doc["meshes"].append({"name": name, "primitives": [prim]})
        return len(self.doc["meshes"]) - 1

    def node(self, n, root=False, parent=None):
        self.doc["nodes"].append(n)
        i = len(self.doc["nodes"]) - 1
        if root:
            self.doc["scenes"][0]["nodes"].append(i)
        if parent is not None:
            self.doc["nodes"][parent].setdefault("children", []).append(i)
        return i

    def write(self, path):
        if not self.doc["materials"]:
            del self.doc["materials"]
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


def r6(v):
    return round(v, 6)


def uv_sphere(radius, seg, rings):
    pos, nrm, uv, idx = [], [], [], []
    for r in range(rings + 1):
        th = math.pi * r / rings
        for s in range(seg + 1):
            ph = 2 * math.pi * s / seg
            n = (math.sin(th) * math.cos(ph), math.cos(th), -math.sin(th) * math.sin(ph))
            pos += [r6(radius * c) for c in n]
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


def box(hx, hy, hz, uv_scale=1.0):
    pos, nrm, uv, idx = [], [], [], []
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)), ((0, 1, 0), (1, 0, 0), (0, 0, -1)),
             ((0, -1, 0), (1, 0, 0), (0, 0, 1)), ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    h = (hx, hy, hz)
    for n, u, v in faces:
        base = len(pos) // 3
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [n[k] * h[k] + u[k] * h[k] * su + v[k] * h[k] * sv for k in range(3)]
            pos += [r6(c) for c in p]
            nrm += list(n)
            uv += [r6((su * 0.5 + 0.5) * uv_scale), r6((0.5 - sv * 0.5) * uv_scale)]
        idx += [base, base + 1, base + 2, base + 2, base + 3, base]
    return pos, nrm, uv, idx


def plane(half, uv_repeat):
    pos = [-half, 0, half, half, 0, half, half, 0, -half, -half, 0, -half]
    nrm = [0, 1, 0] * 4
    uv = [0, uv_repeat, uv_repeat, uv_repeat, uv_repeat, 0, 0, 0]
    return pos, nrm, uv, [0, 1, 2, 2, 3, 0]


def quat_y(angle):
    return [0.0, r6(math.sin(angle / 2)), 0.0, r6(math.cos(angle / 2))]


def build_models():
    models = {}

    # ---- the material sphere row (FR-R3D-020's test sphere row)
    g = Gltf()
    sphere = g.geometry(uv_sphere(0.42, 40, 20))
    for row, (name, color, metallic) in enumerate((("dielectric", [0.75, 0.08, 0.06, 1], 0.0),
                                                   ("metal", [1.0, 0.77, 0.34, 1], 1.0))):
        for i in range(7):
            rough = i / 6.0
            mat = g.material({"name": "%s_r%d" % (name, i), "pbrMetallicRoughness": {
                "baseColorFactor": color, "metallicFactor": metallic, "roughnessFactor": r6(max(rough, 0.02))}})
            mesh = g.mesh("sphere_%s_%d" % (name, i), sphere, mat)
            g.node({"name": "%s_%d" % (name, i), "mesh": mesh, "translation": [r6((i - 3) * 1.0), 0.42, r6(row * 1.1)]},
                   root=True)
    models["sphere_row.glb"] = g

    # ---- the floor
    g = Gltf()
    mat = g.material({"name": "tiles", "pbrMetallicRoughness": {
        "baseColorTexture": {"index": g.texture("../textures/tiles_albedo.png")},
        "metallicRoughnessTexture": {"index": g.texture("../textures/tiles_orm.png")},
        "metallicFactor": 1.0, "roughnessFactor": 1.0},
        "normalTexture": {"index": g.texture("../textures/tiles_normal.png")},
        "occlusionTexture": {"index": g.texture("../textures/tiles_orm.png")}})
    g.node({"name": "floor", "mesh": g.mesh("floor", plane(15.0, 7.5), mat)}, root=True)
    models["floor.glb"] = g

    # ---- the panel cube
    g = Gltf()
    mat = g.material({"name": "panel", "pbrMetallicRoughness": {
        "baseColorTexture": {"index": g.texture("../textures/panel_albedo.png")},
        "metallicRoughnessTexture": {"index": g.texture("../textures/panel_orm.png")}},
        "normalTexture": {"index": g.texture("../textures/panel_normal.png")},
        "occlusionTexture": {"index": g.texture("../textures/panel_orm.png")},
        "emissiveTexture": {"index": g.texture("../textures/panel_emissive.png")},
        "emissiveFactor": [1.0, 1.0, 1.0],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 6.0}}})
    g.doc["extensionsUsed"] = ["KHR_materials_emissive_strength"]
    g.node({"name": "cube", "mesh": g.mesh("cube", box(0.5, 0.5, 0.5), mat), "translation": [0, 0.5, 0]}, root=True)
    models["panel_cube.glb"] = g

    # ---- the orrery: multi-node animation tracks
    g = Gltf()
    brass = g.material({"name": "brass", "pbrMetallicRoughness": {"baseColorFactor": [0.95, 0.7, 0.35, 1],
                                                                 "metallicFactor": 1.0, "roughnessFactor": 0.3}})
    stone = g.material({"name": "stone", "pbrMetallicRoughness": {"baseColorFactor": [0.25, 0.27, 0.3, 1],
                                                                 "metallicFactor": 0.0, "roughnessFactor": 0.7}})
    blue = g.material({"name": "planet", "pbrMetallicRoughness": {"baseColorFactor": [0.1, 0.35, 0.9, 1],
                                                                 "metallicFactor": 0.0, "roughnessFactor": 0.25}})
    root = g.node({"name": "orrery"}, root=True)
    g.node({"name": "base", "mesh": g.mesh("base", box(0.6, 0.15, 0.6), stone), "translation": [0, 0.15, 0]}, parent=root)
    g.node({"name": "pillar", "mesh": g.mesh("pillar", box(0.08, 0.7, 0.08), brass), "translation": [0, 0.9, 0]},
           parent=root)
    hub = g.node({"name": "hub", "translation": [0, 1.6, 0]}, parent=root)
    arm = g.node({"name": "arm", "mesh": g.mesh("arm", box(1.0, 0.04, 0.04), brass), "translation": [0, 0, 0]},
                 parent=hub)
    planet = g.node({"name": "planet", "mesh": g.mesh("planet", uv_sphere(0.28, 24, 12), blue),
                     "translation": [1.0, 0, 0]}, parent=arm)
    moon_arm = g.node({"name": "moon_arm"}, parent=planet)
    g.node({"name": "moon", "mesh": g.mesh("moon", box(0.08, 0.08, 0.08), brass), "translation": [0.48, 0, 0]},
           parent=moon_arm)
    g.node({"name": "counterweight", "mesh": g.mesh("counterweight", uv_sphere(0.16, 16, 8), stone),
            "translation": [-1.0, 0, 0]}, parent=arm)

    def anim(name, channels):
        samplers, chans = [], []
        for node, path, interp, times, values, comps in channels:
            samplers.append({"input": g.accessor(times, 1, minmax=True), "output": g.accessor(values, comps),
                             "interpolation": interp})
            chans.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})
        g.doc.setdefault("animations", []).append({"name": name, "samplers": samplers, "channels": chans})

    def spin(node, period, steps=4):
        times = [r6(period * k / steps) for k in range(steps + 1)]
        values = []
        for k in range(steps + 1):
            values += quat_y(2 * math.pi * k / steps)
        return (node, "rotation", "LINEAR", times, values, 4)

    # CUBICSPLINE bob: keys at 0, 1, 2 s; zero tangents at the ends, a smooth hump in the middle.
    bob_times = [0.0, 1.0, 2.0]
    bob_values = [0, 0, 0, 0, 1.6, 0, 0, 0, 0,
                  0, 0, 0, 0, 1.85, 0, 0, 0, 0,
                  0, 0, 0, 0, 1.6, 0, 0, 0, 0]  # per key: in-tangent, value, out-tangent (vec3 each)
    anim("spin", [spin(arm, 6.0), spin(moon_arm, 1.5), spin(planet, 3.0),
                  (hub, "translation", "CUBICSPLINE", bob_times, bob_values, 3)])
    pulse_times = [0.0, 0.5, 1.0, 1.5]
    pulse_scale = [1, 1, 1, 1.35, 1.35, 1.35, 1, 1, 1, 1.35, 1.35, 1.35]
    anim("pulse", [spin(arm, 2.0), (planet, "scale", "STEP", pulse_times, pulse_scale, 3)])
    models["orrery.glb"] = g

    # ---- the point-light bulb
    g = Gltf()
    mat = g.material({"name": "bulb", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]},
                      "extensions": {"KHR_materials_unlit": {}}})
    g.doc["extensionsUsed"] = ["KHR_materials_unlit"]
    g.node({"name": "bulb", "mesh": g.mesh("bulb", uv_sphere(0.06, 8, 5), mat)}, root=True)
    models["bulb.glb"] = g
    return models


def sidecar(path, importer, version, settings):
    rel = path.relative_to(ROOT).as_posix()
    doc = {"guid": str(uuid.uuid5(NS, rel)), "importer": importer, "importer_version": version, "settings": settings}
    pathlib.Path(str(path) + ".mceasset").write_text(json.dumps(doc, indent=2) + "\n")


def main():
    textures = ROOT / "textures"
    a, n, o = tiles_textures()
    for name, px, srgb in (("tiles_albedo", a, True), ("tiles_normal", n, False), ("tiles_orm", o, False)):
        write_png(textures / (name + ".png"), 256, 256, px)
        sidecar(textures / (name + ".png"), "texture", 1, {} if srgb else {"srgb": False})
    a, n, o, e = panel_textures()
    for name, px, srgb in (("panel_albedo", a, True), ("panel_normal", n, False), ("panel_orm", o, False),
                           ("panel_emissive", e, True)):
        write_png(textures / (name + ".png"), 256, 256, px)
        sidecar(textures / (name + ".png"), "texture", 1, {} if srgb else {"srgb": False})
    write_png(textures / "sky_equirect.png", 512, 256, sky_equirect())
    sidecar(textures / "sky_equirect.png", "texture", 1, {})
    for name, g in build_models().items():
        g.write(ROOT / "models" / name)
        sidecar(ROOT / "models" / name, "gltf", 2, {})
    print("04_cubes assets written to", ROOT)


if __name__ == "__main__":
    main()
