#!/usr/bin/env python3
"""Generates samples/05_ashen_court/assets: the Ashen Court kit, procedurally.

    python3 samples/05_ashen_court/tools/gen_assets.py

Deterministic (no randomness, no clocks): re-running reproduces the committed bytes. Standard
library only; the glTF/PNG writers are 04_cubes' (tools/gen_assets.py there).

ROADMAP M7 asks for CC0 kits (Kenney, Poly Haven) plus Blender kitbashing. Neither host is
reachable from the build container, so this kit is generated: modular pieces (wall segment,
fluted column, broken column, arch, dais, brazier, rubble) assembled by the sample into the
court, plus static-proxy characters (FR-R3D-060 stays Should: no skinning).

Every model's sidecar carries its triangle budget ("max_triangles", checked by the glTF
importer at cook); the table is BUDGETS below and in docs/milestones/M7-ashen-court.md.
"""

import json
import math
import pathlib
import sys
import uuid

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "04_cubes" / "tools"))
from gen_assets import (Gltf, box, capsule, clamp01, hash01, normal_from_height, r6, srgb8, to8,  # noqa: E402
                        uv_sphere, write_png)

ROOT = pathlib.Path(__file__).resolve().parents[1] / "assets"
NS = uuid.UUID("6d63652d-3035-6173-6865-6e636f757274")  # fixed namespace: stable GUIDs per path

# Per-mesh triangle budgets (cook-checked). The level total is computed by the sample at load.
BUDGETS = {
    "court_floor.glb": 12000,
    "wall_segment.glb": 1500,
    "column.glb": 6000,
    "broken_column.glb": 4000,
    "arch.glb": 4000,
    "dais.glb": 1000,
    "brazier.glb": 3000,
    "rubble.glb": 4000,
    "hero.glb": 6000,
    "sword.glb": 500,
    "husk.glb": 5000,
    "wisp.glb": 5000,
    "brute.glb": 6000,
    "orb.glb": 1000,
    "ember_heart.glb": 500,
}


# ---------------------------------------------------------------------------------------- textures
def fbm(x, y, seed, octaves=4):
    v, amp, f, tot = 0.0, 1.0, 1.0, 0.0
    for o in range(octaves):
        xi, yi = int(x * f), int(y * f)
        fx, fy = x * f - xi, y * f - yi
        a, b = hash01(xi, yi, seed + o), hash01(xi + 1, yi, seed + o)
        c, d = hash01(xi, yi + 1, seed + o), hash01(xi + 1, yi + 1, seed + o)
        sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        v += amp * ((a * (1 - sx) + b * sx) * (1 - sy) + (c * (1 - sx) + d * sx) * sy)
        tot += amp
        amp *= 0.5
        f *= 2.0
    return v / tot


def stone_textures(n, rows, cols_even, cols_odd, base, mortar, seed):
    """Running-bond ashlar / flagstones: per-block tint and wear, recessed mortar, soot."""
    albedo, orm, height = bytearray(n * n * 4), bytearray(n * n * 4), [0.0] * (n * n)
    rh = n // rows
    for y in range(n):
        row = y // rh
        cols = cols_odd if row % 2 else cols_even
        cw = n // cols
        off = (cw // 2) if row % 2 else 0
        for x in range(n):
            xx = (x + off) % n
            col = xx // cw
            u, v = xx % cw, y % rh
            edge = min(u, v, cw - 1 - u, rh - 1 - v)
            joint = edge < 2
            bevel = clamp01((edge - 2) / 5.0)
            noise = fbm(x / 24.0, y / 24.0, seed)
            tint = 0.8 + 0.25 * hash01(col, row, seed + 7)
            soot = clamp01(fbm(x / 60.0, y / 60.0, seed + 31) * 1.6 - 0.55)
            if joint:
                c = mortar
            else:
                c = tuple(b * tint * (0.85 + 0.3 * noise) * (1 - 0.6 * soot) for b in base)
            i = (y * n + x) * 4
            albedo[i:i + 4] = bytes((srgb8(c[0]), srgb8(c[1]), srgb8(c[2]), 255))
            ao = 0.4 if joint else 0.7 + 0.3 * bevel
            rough = 0.95 if joint else 0.65 + 0.3 * noise
            orm[i:i + 4] = bytes((to8(ao), to8(rough), 0, 255))
            height[y * n + x] = 0.0 if joint else bevel * (0.8 + 0.2 * noise)
    return albedo, normal_from_height(height, n, n, 5.0), orm


# ---------------------------------------------------------------------------------------- geometry
class Geo:
    """Accumulates triangles; helpers append transformed parts."""

    def __init__(self):
        self.pos, self.nrm, self.uv, self.idx = [], [], [], []

    def add(self, part, offset=(0, 0, 0), rot_y=0.0, scale=(1, 1, 1)):
        pos, nrm, uv, idx = part
        base = len(self.pos) // 3
        c, s = math.cos(rot_y), math.sin(rot_y)
        for k in range(len(pos) // 3):
            x, y, z = pos[3 * k] * scale[0], pos[3 * k + 1] * scale[1], pos[3 * k + 2] * scale[2]
            nx, ny, nz = nrm[3 * k] / scale[0], nrm[3 * k + 1] / scale[1], nrm[3 * k + 2] / scale[2]
            nl = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            nx, ny, nz = nx / nl, ny / nl, nz / nl
            self.pos += [r6(c * x + s * z + offset[0]), r6(y + offset[1]), r6(-s * x + c * z + offset[2])]
            self.nrm += [r6(c * nx + s * nz), r6(ny), r6(-s * nx + c * nz)]
        self.uv += uv
        self.idx += [i + base for i in idx]
        return self

    def tris(self):
        return len(self.idx) // 3

    def data(self):
        return self.pos, self.nrm, self.uv, self.idx


def lathe(profile, seg, flutes=0, flute_depth=0.0, uv_v_scale=1.0):
    """Surface of revolution around +Y. profile: [(radius, y)], bottom to top. Flutes modulate the
    radius of the inner profile points (not the first/last, which stay round)."""
    pos, nrm, uv, idx = [], [], [], []
    rings = len(profile)
    for r, (rad, y) in enumerate(profile):
        if r + 1 < rings:
            dr, dy = profile[r + 1][0] - rad, profile[r + 1][1] - y
        else:
            dr, dy = rad - profile[r - 1][0], y - profile[r - 1][1]
        for s in range(seg + 1):
            ph = 2 * math.pi * s / seg
            rr = rad
            if flutes and 0 < r < rings - 1:
                rr = rad * (1.0 - flute_depth * (0.5 + 0.5 * math.cos(ph * flutes)) ** 2)
            cx, cz = math.cos(ph), -math.sin(ph)
            pos += [r6(rr * cx), r6(y), r6(rr * cz)]
            ln = math.sqrt(dy * dy + dr * dr) or 1.0
            nrm += [r6(cx * dy / ln), r6(-dr / ln), r6(cz * dy / ln)]
            uv += [r6(s / seg * 2), r6(y * uv_v_scale)]
    for r in range(rings - 1):
        for s in range(seg):
            a, b = r * (seg + 1) + s, (r + 1) * (seg + 1) + s
            idx += [a, a + 1, b, b, a + 1, b + 1]
    return pos, nrm, uv, idx


def disc(radius, y, seg, up=True):
    pos, nrm, uv, idx = [0, r6(y), 0], [0, 1 if up else -1, 0], [0.5, 0.5], []
    for s in range(seg + 1):
        ph = 2 * math.pi * s / seg
        pos += [r6(radius * math.cos(ph)), r6(y), r6(-radius * math.sin(ph))]
        nrm += [0, 1 if up else -1, 0]
        uv += [r6(0.5 + 0.5 * math.cos(ph)), r6(0.5 + 0.5 * math.sin(ph))]
    for s in range(seg):
        idx += [0, s + 1, s + 2] if up else [0, s + 2, s + 1]
    return pos, nrm, uv, idx


def rock(radius, seed, subdiv=2, rough=0.35):
    """A noisy icosphere: rubble chunks and the broken column's jagged top."""
    t = (1 + math.sqrt(5)) / 2
    verts = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
             (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6),
             (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10),
             (8, 6, 7), (9, 8, 1)]
    verts = [tuple(c / math.sqrt(sum(q * q for q in v)) for c in v) for v in verts]
    for _ in range(subdiv):
        cache, nf = {}, []

        def mid(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                m = tuple((verts[a][k] + verts[b][k]) / 2 for k in range(3))
                l = math.sqrt(sum(q * q for q in m))
                verts.append(tuple(c / l for c in m))
                cache[key] = len(verts) - 1
            return cache[key]
        for a, b, c in faces:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            nf += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        faces = nf
    pos, nrm, uv, idx = [], [], [], []
    disp = []
    for k, v in enumerate(verts):
        d = 1.0 + rough * (hash01(int(v[0] * 97 + 300), int(v[1] * 89 + 300), seed + int(v[2] * 83 + 300)) - 0.5)
        disp.append(tuple(c * radius * d for c in v))
    # flat-shaded facets read as broken stone
    for a, b, c in faces:
        pa, pb, pc = disp[a], disp[b], disp[c]
        u = [pb[k] - pa[k] for k in range(3)]
        w = [pc[k] - pa[k] for k in range(3)]
        n = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]]
        l = math.sqrt(sum(q * q for q in n)) or 1.0
        base = len(pos) // 3
        for p in (pa, pb, pc):
            pos += [r6(q) for q in p]
            nrm += [r6(q / l) for q in n]
            uv += [r6(p[0] * 0.5), r6(p[1] * 0.5 + p[2] * 0.5)]
        idx += [base, base + 1, base + 2]
    return pos, nrm, uv, idx


def grid_plane(half, cells, uv_repeat):
    pos, nrm, uv, idx = [], [], [], []
    for j in range(cells + 1):
        for i in range(cells + 1):
            x, z = -half + 2 * half * i / cells, -half + 2 * half * j / cells
            pos += [r6(x), 0.0, r6(z)]
            nrm += [0, 1, 0]
            uv += [r6(uv_repeat * i / cells), r6(uv_repeat * j / cells)]
    for j in range(cells):
        for i in range(cells):
            a, b = j * (cells + 1) + i, (j + 1) * (cells + 1) + i
            idx += [a, b, a + 1, a + 1, b, b + 1]
    return pos, nrm, uv, idx


def column_geo(height, radius, seg=32):
    g = Geo()
    shaft = [(radius, 0.5)] + [(radius * (1.0 - 0.12 * (k / 15)), 0.5 + (height - 1.1) * k / 15) for k in range(16)]
    g.add(lathe(shaft, seg, flutes=16, flute_depth=0.06, uv_v_scale=0.5))
    base = [(radius * 1.45, 0.0), (radius * 1.45, 0.22), (radius * 1.3, 0.3), (radius * 1.2, 0.42), (radius * 1.05, 0.5)]
    g.add(lathe(base, seg))
    top = height - 0.6
    cap = [(radius * 0.88, top), (radius * 1.0, top + 0.12), (radius * 1.25, top + 0.32), (radius * 1.45, top + 0.42),
           (radius * 1.45, top + 0.6)]
    g.add(lathe(cap, seg))
    g.add(box(radius * 1.6, 0.08, radius * 1.6), (0, height + 0.02, 0))
    return g


def build_models():
    models = {}

    def stone_mat(g, name, tex, tint=(1, 1, 1, 1)):
        return g.material({"name": name, "pbrMetallicRoughness": {
            "baseColorFactor": list(tint),
            "baseColorTexture": {"index": g.texture("../textures/%s_albedo.png" % tex)},
            "metallicRoughnessTexture": {"index": g.texture("../textures/%s_orm.png" % tex)},
            "metallicFactor": 1.0, "roughnessFactor": 1.0},
            "normalTexture": {"index": g.texture("../textures/%s_normal.png" % tex)},
            "occlusionTexture": {"index": g.texture("../textures/%s_orm.png" % tex)}})

    def flat_mat(g, name, color, metallic=0.0, rough=0.6, emissive=None, strength=1.0):
        m = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(color), "metallicFactor": metallic,
                                                    "roughnessFactor": rough}}
        if emissive:
            m["emissiveFactor"] = list(emissive)
            if strength != 1.0:
                m["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": strength}}
                g.doc["extensionsUsed"] = ["KHR_materials_emissive_strength"]
        return g.material(m)

    def single(name, geo, mat_fn):
        g = Gltf()
        g.node({"name": name, "mesh": g.mesh(name, geo.data() if isinstance(geo, Geo) else geo, mat_fn(g))}, root=True)
        return g

    # ---- the court floor: 48 m of flagstones, 48 x 48 cells (gentle vertex wear for the shadows)
    floor = grid_plane(24.0, 48, 12.0)
    models["court_floor.glb"] = single("court_floor", floor, lambda g: stone_mat(g, "flagstone", "flagstone"))

    # ---- a 4 m wall segment: 5 m tall, 1 m thick, a plinth, a string course and two merlons
    w = Geo()
    w.add(box(2.0, 2.25, 0.5, 2.0), (0, 2.25, 0))
    w.add(box(2.05, 0.3, 0.6, 1.0), (0, 0.3, 0))
    w.add(box(2.05, 0.12, 0.56, 1.0), (0, 3.6, 0))
    for mx in (-1.25, 0.75):
        w.add(box(0.5, 0.4, 0.5, 0.5), (mx + 0.25, 4.9, 0))
    models["wall_segment.glb"] = single("wall_segment", w, lambda g: stone_mat(g, "ashlar", "ashlar"))

    # ---- columns
    models["column.glb"] = single("column", column_geo(5.0, 0.42), lambda g: stone_mat(g, "column", "ashlar",
                                                                                         (1.05, 1.0, 0.95, 1)))
    bc = Geo()
    shaft = [(0.42, 0.5)] + [(0.42 * (1.0 - 0.05 * k / 6), 0.5 + 1.6 * k / 6) for k in range(7)]
    bc.add(lathe(shaft, 32, flutes=16, flute_depth=0.06, uv_v_scale=0.5))
    bc.add(lathe([(0.61, 0.0), (0.61, 0.22), (0.55, 0.3), (0.5, 0.42), (0.44, 0.5)], 32))
    bc.add(rock(0.5, 11, 2, 0.6), (0, 2.1, 0), 0.0, (0.85, 0.5, 0.85))
    bc.add(rock(0.45, 12, 2, 0.5), (0.9, 0.25, 0.4), 0.7, (1.2, 0.5, 0.8))
    models["broken_column.glb"] = single("broken_column", bc, lambda g: stone_mat(g, "column", "ashlar",
                                                                                   (0.9, 0.85, 0.8, 1)))

    # ---- an arch: two 1 m piers 4 m apart (inner span 3 m) and a 24-voussoir half ring on top
    a = Geo()
    for px in (-2.0, 2.0):
        a.add(box(0.5, 1.75, 0.6, 1.5), (px, 1.75, 0))
        a.add(box(0.6, 0.12, 0.7, 1.0), (px, 3.56, 0))
    inner, outer, n = 1.5, 2.5, 24
    for k in range(n):
        t0, t1 = math.pi * k / n, math.pi * (k + 1) / n
        tm = 0.5 * (t0 + t1)
        rm = 0.5 * (inner + outer)
        length = rm * (t1 - t0) * 0.98
        a.add(box(0.5 * (outer - inner), 0.5 * length, 0.6, 0.5), (rm * math.cos(tm), 3.6 + rm * math.sin(tm), 0.0),
              0.0, (1, 1, 1))
        # rotate each voussoir about Z: rebuild the last 24 vertices in place
        start = len(a.pos) - 24 * 3
        cz, sz = math.cos(tm), math.sin(tm)
        cx0, cy0 = rm * math.cos(tm), 3.6 + rm * math.sin(tm)
        for v in range(24):
            i = start + 3 * v
            x, y = a.pos[i] - cx0, a.pos[i + 1] - cy0
            a.pos[i], a.pos[i + 1] = r6(cx0 + cz * x - sz * y), r6(cy0 + sz * x + cz * y)
            nx, ny = a.nrm[i], a.nrm[i + 1]
            a.nrm[i], a.nrm[i + 1] = r6(cz * nx - sz * ny), r6(sz * nx + cz * ny)
    models["arch.glb"] = single("arch", a, lambda g: stone_mat(g, "arch", "ashlar", (0.95, 0.92, 0.9, 1)))

    # ---- the dais: three steps, 10 m -> 6 m square
    d = Geo()
    for k, half in enumerate((5.0, 4.2, 3.4)):
        d.add(box(half, 0.2, half, half / 2), (0, 0.2 + 0.4 * k, 0))
    models["dais.glb"] = single("dais", d, lambda g: stone_mat(g, "dais", "flagstone", (0.9, 0.88, 0.86, 1)))

    # ---- a brazier: a bronze bowl on three legs, glowing coals (the point lights sit above it)
    g = Gltf()
    bronze = flat_mat(g, "bronze", (0.55, 0.35, 0.18, 1), 1.0, 0.45)
    coals = flat_mat(g, "coals", (0.05, 0.02, 0.01, 1), 0.0, 0.9, (1.0, 0.35, 0.08), 8.0)
    bowl = Geo()
    bowl.add(lathe([(0.12, 0.85), (0.45, 0.95), (0.62, 1.1), (0.66, 1.25), (0.6, 1.25)], 32))
    for k in range(3):
        ang = 2 * math.pi * k / 3
        bowl.add(box(0.05, 0.5, 0.05), (0.38 * math.cos(ang), 0.48, -0.38 * math.sin(ang)), ang)
    bowl.add(lathe([(0.3, 0.0), (0.3, 0.06), (0.1, 0.1)], 24))
    root = g.node({"name": "brazier", "mesh": g.mesh("brazier", bowl.data(), bronze)}, root=True)
    g.node({"name": "coals", "mesh": g.mesh("coals", rock(0.5, 21, 2, 0.5), coals), "translation": [0, 1.12, 0],
            "scale": [1.0, 0.35, 1.0]}, parent=root)
    models["brazier.glb"] = g

    # ---- rubble: a cluster of broken blocks
    r = Geo()
    for k, (x, z, s) in enumerate(((0, 0, 0.6), (0.8, 0.3, 0.45), (-0.6, 0.5, 0.4), (0.3, -0.7, 0.35), (-0.4, -0.5, 0.3),
                                   (1.2, -0.4, 0.25))):
        r.add(rock(s, 40 + k, 2, 0.5), (x, s * 0.45, z), k * 0.9, (1.2, 0.7, 1.0))
    models["rubble.glb"] = single("rubble", r, lambda g: stone_mat(g, "rubble", "ashlar", (0.8, 0.76, 0.72, 1)))

    # ---- characters: static proxies standing on their capsule (centre at the origin, feet at -0.9)
    g = Gltf()
    steel = flat_mat(g, "steel", (0.62, 0.64, 0.68, 1), 1.0, 0.3)
    cloth = flat_mat(g, "tabard", (0.55, 0.08, 0.06, 1), 0.0, 0.7)
    gold = flat_mat(g, "gilt", (1.0, 0.75, 0.3, 1), 1.0, 0.25)
    hero = g.node({"name": "hero", "mesh": g.mesh("hero_body", capsule(0.3, 0.55, 32, 16), cloth)}, root=True)
    g.node({"name": "helm", "mesh": g.mesh("hero_helm", uv_sphere(0.24, 32, 16), steel), "translation": [0, 0.7, 0]},
           parent=hero)
    g.node({"name": "crest", "mesh": g.mesh("hero_crest", box(0.03, 0.12, 0.18), gold), "translation": [0, 0.95, 0]},
           parent=hero)
    g.node({"name": "pauldrons", "mesh": g.mesh("hero_pauldrons", box(0.42, 0.08, 0.2), steel),
            "translation": [0, 0.45, 0]}, parent=hero)
    g.node({"name": "shield", "mesh": g.mesh("hero_shield", lathe([(0.0, -0.03), (0.3, -0.02), (0.32, 0.03)], 32), gold),
            "translation": [-0.36, 0.05, 0.05], "rotation": [0, 0, r6(math.sin(math.pi / 4)), r6(math.cos(math.pi / 4))]},
           parent=hero)
    models["hero.glb"] = g

    g = Gltf()
    blade = flat_mat(g, "blade", (0.8, 0.82, 0.86, 1), 1.0, 0.18, (1.0, 0.45, 0.1), 0.6)
    hilt = flat_mat(g, "hilt", (0.3, 0.18, 0.08, 1), 0.0, 0.6)
    sword = g.node({"name": "sword", "mesh": g.mesh("sword_blade", box(0.04, 0.5, 0.012), blade),
                    "translation": [0, 0.62, 0]}, root=True)
    g.node({"name": "guard", "mesh": g.mesh("sword_guard", box(0.16, 0.025, 0.03), hilt), "translation": [0, -0.5, 0]},
           parent=sword)
    g.node({"name": "grip", "mesh": g.mesh("sword_grip", box(0.025, 0.1, 0.025), hilt), "translation": [0, -0.62, 0]},
           parent=sword)
    models["sword.glb"] = g

    def ember_eyes(g, parent, y, z, spread):
        eye = flat_mat(g, "ember_eye", (0.1, 0.02, 0.0, 1), 0.0, 0.5, (1.0, 0.4, 0.05), 10.0)
        for sx in (-spread, spread):
            g.node({"name": "eye", "mesh": g.mesh("eye", uv_sphere(0.045, 10, 6), eye), "translation": [sx, y, z]},
                   parent=parent)

    # Husk (grunt): a hunched ash-grey figure with a club arm.
    g = Gltf()
    ash = flat_mat(g, "ash", (0.32, 0.3, 0.29, 1), 0.0, 0.95)
    body = Geo().add(capsule(0.32, 0.45, 32, 16))
    body.add(rock(0.22, 51, 2, 0.4), (0, 0.6, 0.12))
    husk = g.node({"name": "husk", "mesh": g.mesh("husk_body", body.data(), ash)}, root=True)
    ember_eyes(g, husk, 0.62, 0.3, 0.08)
    g.node({"name": "club", "mesh": g.mesh("husk_club", rock(0.18, 52, 2, 0.4), ash), "translation": [0.42, 0.0, 0.2],
            "scale": [0.7, 1.8, 0.7]}, parent=husk)
    models["husk.glb"] = g

    # Wisp (caster): a hovering cowl over a burning core.
    g = Gltf()
    robe = flat_mat(g, "robe", (0.12, 0.1, 0.14, 1), 0.0, 0.85)
    core = flat_mat(g, "core", (0.2, 0.05, 0.0, 1), 0.0, 0.4, (1.0, 0.55, 0.15), 12.0)
    cowl = lathe([(0.05, -0.9), (0.42, -0.85), (0.36, -0.2), (0.3, 0.3), (0.22, 0.6), (0.0, 0.75)], 32)
    wisp = g.node({"name": "wisp", "mesh": g.mesh("wisp_cowl", cowl, robe)}, root=True)
    g.node({"name": "core", "mesh": g.mesh("wisp_core", uv_sphere(0.16, 24, 12), core), "translation": [0, 0.35, 0.18]},
           parent=wisp)
    models["wisp.glb"] = g

    # Brute (charger): a broad horned bulk of slag.
    g = Gltf()
    slag = flat_mat(g, "slag", (0.18, 0.13, 0.11, 1), 0.2, 0.7)
    horn = flat_mat(g, "horn", (0.75, 0.68, 0.55, 1), 0.0, 0.5)
    bulk = Geo().add(capsule(0.55, 0.35, 32, 16), scale=(1.0, 1.0, 0.85))
    bulk.add(rock(0.35, 61, 2, 0.4), (0, 0.55, 0.25))
    brute = g.node({"name": "brute", "mesh": g.mesh("brute_body", bulk.data(), slag)}, root=True)
    for sx in (-1, 1):
        g.node({"name": "horn", "mesh": g.mesh("brute_horn", lathe([(0.1, 0.0), (0.07, 0.25), (0.0, 0.5)], 16), horn),
                "translation": [sx * 0.3, 0.75, 0.3],
                "rotation": [0, 0, r6(math.sin(-sx * 0.5)), r6(math.cos(-sx * 0.5))]}, parent=brute)
    ember_eyes(g, brute, 0.6, 0.62, 0.14)
    models["brute.glb"] = g

    # Wisp projectile and the health pickup.
    models["orb.glb"] = single("orb", uv_sphere(0.18, 20, 10),
                               lambda g: flat_mat(g, "orb", (0.3, 0.1, 0.0, 1), 0.0, 0.3, (1.0, 0.5, 0.1), 14.0))
    heart = Geo().add(rock(0.22, 71, 1, 0.0), scale=(0.8, 1.2, 0.8))
    models["ember_heart.glb"] = single("ember_heart", heart, lambda g: flat_mat(
        g, "heart", (0.6, 0.05, 0.05, 1), 0.0, 0.2, (1.0, 0.15, 0.1), 6.0))
    return models


def tri_count(g):
    total = 0
    for mesh in g.doc["meshes"]:
        for p in mesh["primitives"]:
            total += g.doc["accessors"][p["indices"]]["count"] // 3
    # instanced meshes (several nodes, one mesh) count once per node
    uses = {}
    for n in g.doc["nodes"]:
        if "mesh" in n:
            uses[n["mesh"]] = uses.get(n["mesh"], 0) + 1
    total = 0
    for mi, mesh in enumerate(g.doc["meshes"]):
        for p in mesh["primitives"]:
            total += uses.get(mi, 0) * (g.doc["accessors"][p["indices"]]["count"] // 3)
    return total


def sidecar(path, importer, version, settings):
    rel = path.relative_to(ROOT).as_posix()
    doc = {"guid": str(uuid.uuid5(NS, rel)), "importer": importer, "importer_version": version, "settings": settings}
    pathlib.Path(str(path) + ".mceasset").write_text(json.dumps(doc, indent=2) + "\n")


def main():
    textures = ROOT / "textures"
    for name, args in (("flagstone", (256, 4, 3, 3, (0.42, 0.38, 0.34), (0.12, 0.11, 0.1), 3)),
                       ("ashlar", (256, 8, 4, 4, (0.5, 0.45, 0.4), (0.18, 0.16, 0.14), 9))):
        a, n, o = stone_textures(*args)
        for suffix, px, srgb in (("albedo", a, True), ("normal", n, False), ("orm", o, False)):
            p = textures / ("%s_%s.png" % (name, suffix))
            write_png(p, 256, 256, px)
            sidecar(p, "texture", 1, {} if srgb else {"srgb": False})
    report = []
    for name, g in build_models().items():
        tris = tri_count(g)
        budget = BUDGETS[name]
        assert tris <= budget, "%s: %d triangles over its budget %d" % (name, tris, budget)
        g.write(ROOT / "models" / name)
        sidecar(ROOT / "models" / name, "gltf", 2, {"max_triangles": budget})
        report.append((name, tris, budget))
    for name, tris, budget in report:
        print("  %-20s %6d / %6d" % (name, tris, budget))
    print("05_ashen_court assets written to", ROOT)


if __name__ == "__main__":
    main()
