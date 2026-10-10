#!/usr/bin/env python3
"""Generates projects/SolsticeReach/assets: the island, its architecture, props and textures.

    python3 projects/SolsticeReach/tools/gen_assets.py

Deterministic: re-running reproduces the committed bytes (no randomness, no clocks; NumPy for the
large textures, Pillow to encode them). The glTF writer and the small geometry helpers are the
sample generators' (samples/04_cubes/tools, samples/05_ashen_court/tools); the terrain is
tools/island.py's height field, the one the game module ports for its collider.

CC0 kits are unreachable from the build container (docs/design/solstice-reach.md §4.3), so every
model is procedural. Each model's sidecar carries its triangle budget (`max_triangles`, checked
by the glTF importer at cook); the table is BUDGETS below.
"""

import importlib.util
import json
import math
import pathlib
import sys
import uuid

import numpy as np
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
ENGINE = HERE.parents[2]
sys.path.insert(0, str(ENGINE / "samples" / "04_cubes" / "tools"))
from gen_assets import Gltf, box, capsule, r6, uv_sphere, write_png  # noqa: E402

_spec = importlib.util.spec_from_file_location("ashen_gen", ENGINE / "samples" / "05_ashen_court" / "tools" / "gen_assets.py")
ashen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ashen)
Geo, lathe, disc, rock, stone_textures, column_geo = ashen.Geo, ashen.lathe, ashen.disc, ashen.rock, ashen.stone_textures, ashen.column_geo

sys.path.insert(0, str(HERE))
import island  # noqa: E402

ROOT = HERE.parent / "assets"
NS = uuid.UUID("6d63652d-736f-6c73-7469-636572656163")  # fixed namespace: stable GUIDs per path

BUDGETS = {
    **{"island_%02d.glb" % i: 5200 for i in range(16)},
    "sea.glb": 600,
    "vault.glb": 9000,
    "dais.glb": 1200,
    "column.glb": 6000,
    "arch.glb": 1200,
    "wall_low.glb": 400,
    "stair_flight.glb": 400,
    "pier.glb": 1500,
    "windmill.glb": 5000,
    "cistern.glb": 600,
    "lantern.glb": 1800,
    "banner.glb": 800,
    "boat.glb": 1500,
    "olive.glb": 2000,
    "cypress.glb": 900,
    "pine.glb": 1200,
    "rock_a.glb": 1400,
    "rock_b.glb": 1400,
    "rock_c.glb": 1400,
    "grass_tuft.glb": 6,
    "flower.glb": 4,
    "sunshard.glb": 200,
    "gull.glb": 600,
    "mirror_base.glb": 2200,
    "mirror_dish.glb": 2600,
    "beam.glb": 12,
    "keeper.glb": 4500,
}


# ----------------------------------------------------------------------------------------- noise (numpy)
def hash_grid(ix, iz, seed):
    n = (ix.astype(np.uint64) * np.uint64(374761393) + iz.astype(np.uint64) * np.uint64(668265263)
         + np.uint64(seed) * np.uint64(2246822519)) & np.uint64(0xFFFFFFFF)
    n = (n ^ (n >> np.uint64(13))) & np.uint64(0xFFFFFFFF)
    n = (n * np.uint64(1274126177)) & np.uint64(0xFFFFFFFF)
    n = n ^ (n >> np.uint64(16))
    return n.astype(np.float64) / 4294967296.0


def vnoise_np(x, z, seed, period):
    """Tiling value noise on a lattice of `period` cells (x, z in cells)."""
    ix, iz = np.floor(x).astype(np.int64), np.floor(z).astype(np.int64)
    fx, fz = x - ix, z - iz
    sx, sz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
    a = hash_grid(ix % period, iz % period, seed)
    b = hash_grid((ix + 1) % period, iz % period, seed)
    c = hash_grid(ix % period, (iz + 1) % period, seed)
    d = hash_grid((ix + 1) % period, (iz + 1) % period, seed)
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sz


def fbm_np(n, seed, base_period=8, octaves=5, gain=0.5, ridged=False):
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float64)
    v, amp, period, tot = np.zeros((n, n)), 1.0, base_period, 0.0
    for o in range(octaves):
        layer = vnoise_np(xs / n * period, ys / n * period, seed + o * 97, period)
        if ridged:
            layer = 1.0 - np.abs(layer * 2 - 1)
        v += amp * layer
        tot += amp
        amp *= gain
        period *= 2
    return v / tot


def normal_map(height, strength):
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * strength
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * strength
    n = np.dstack((-dx, dy, np.ones_like(height)))
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return np.dstack((n * 0.5 + 0.5, np.ones_like(height)))


def srgb(lin):
    lin = np.clip(lin, 0, 1)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055)


def to_u8(a):
    return (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)


def save_rgba(path, rgba_float, is_srgb):
    rgb = rgba_float[..., :3]
    out = np.dstack((srgb(rgb) if is_srgb else rgb, rgba_float[..., 3:4]))
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(to_u8(out), "RGBA").save(path, optimize=False, compress_level=9)


def orm(ao, rough, metal):
    return np.dstack((ao, rough, metal, np.ones_like(ao)))


def material_textures():
    """albedo / normal / orm triplets for the natural surfaces (512^2, 4 m tiles)."""
    out = {}
    n = 512
    # Grass: two greens by a low-frequency patch map, blade streaks, a few bare spots.
    patch = fbm_np(n, 101, 4, 4)
    blades = fbm_np(n, 102, 64, 3, ridged=True)
    bare = np.clip(fbm_np(n, 103, 6, 3) * 1.8 - 1.1, 0, 1)
    g1, g2 = np.array([0.16, 0.30, 0.08]), np.array([0.30, 0.38, 0.11])
    col = g1[None, None] * (1 - patch[..., None]) + g2[None, None] * patch[..., None]
    col *= 0.75 + 0.5 * blades[..., None]
    col = col * (1 - bare[..., None]) + np.array([0.36, 0.30, 0.20])[None, None] * bare[..., None]
    out["grass"] = (np.dstack((col, np.ones((n, n)))), normal_map(blades * 0.6 + patch * 0.4, 2.5),
                    orm(0.85 + 0.15 * blades, 0.92 - 0.1 * bare, np.zeros((n, n))))
    # Dry grass: straw with the same blade structure.
    col = np.array([0.52, 0.44, 0.22])[None, None] * (0.7 + 0.6 * blades[..., None]) * (0.85 + 0.3 * patch[..., None])
    out["dry_grass"] = (np.dstack((col, np.ones((n, n)))), normal_map(blades, 2.0), orm(0.9 + 0.1 * blades, np.full((n, n), 0.95), np.zeros((n, n))))
    # Limestone rock: ridged cracks, strata bands, lichen.
    cracks = fbm_np(n, 104, 6, 5, ridged=True)
    strata = 0.5 + 0.5 * np.sin(np.linspace(0, 9 * math.pi, n))[:, None] * np.ones((1, n))
    lichen = np.clip(fbm_np(n, 105, 12, 3) * 2.2 - 1.4, 0, 1)
    base = np.array([0.62, 0.58, 0.50])[None, None] * (0.6 + 0.5 * cracks[..., None]) * (0.9 + 0.2 * strata[..., None])
    base = base * (1 - lichen[..., None]) + np.array([0.45, 0.50, 0.20])[None, None] * lichen[..., None]
    out["rock"] = (np.dstack((base, np.ones((n, n)))), normal_map(cracks * 0.8 + strata * 0.2, 6.0), orm(0.6 + 0.4 * cracks, 0.75 + 0.2 * cracks, np.zeros((n, n))))
    # Sand: pale, fine ripples.
    ripple = 0.5 + 0.5 * np.sin(np.linspace(0, 40 * math.pi, n))[None, :] * np.ones((n, 1)) * (0.5 + 0.5 * fbm_np(n, 106, 3, 2))
    fine = fbm_np(n, 107, 128, 2)
    col = np.array([0.76, 0.68, 0.50])[None, None] * (0.9 + 0.15 * ripple[..., None]) * (0.92 + 0.16 * fine[..., None])
    out["sand"] = (np.dstack((col, np.ones((n, n)))), normal_map(ripple * 0.5 + fine * 0.5, 1.5), orm(np.ones((n, n)), np.full((n, n), 0.9), np.zeros((n, n))))
    # Bark: vertical streaks.
    xs = np.mgrid[0:n, 0:n][1].astype(np.float64)
    streak = fbm_np(n, 108, 24, 4, ridged=True)
    streak = 0.5 * streak + 0.5 * vnoise_np(xs / n * 48, np.mgrid[0:n, 0:n][0] / n * 3, 109, 48)
    col = np.array([0.30, 0.22, 0.14])[None, None] * (0.6 + 0.8 * streak[..., None])
    out["bark"] = (np.dstack((col, np.ones((n, n)))), normal_map(streak, 4.0), orm(0.7 + 0.3 * streak, np.full((n, n), 0.9), np.zeros((n, n))))
    # Canvas: a weave.
    m = 256
    ys2, xs2 = np.mgrid[0:m, 0:m].astype(np.float64)
    weave = 0.5 + 0.25 * np.sin(xs2 * math.pi / 2) + 0.25 * np.sin(ys2 * math.pi / 2)
    stain = fbm_np(m, 110, 4, 3)
    col = np.array([0.80, 0.72, 0.58])[None, None] * (0.85 + 0.2 * weave[..., None]) * (0.85 + 0.25 * stain[..., None])
    out["canvas"] = (np.dstack((col, np.ones((m, m)))), normal_map(weave, 0.8), orm(np.ones((m, m)), np.full((m, m), 0.95), np.zeros((m, m))))
    return out


def stars_equirect(w=2048, h=1024):
    """A night sky: stars by hash, a milky-way band, faint nebula colour. Linear radiance; the Sky
    scales it with texture_intensity."""
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float64)
    lat = (0.5 - ys / h) * math.pi
    img = np.zeros((h, w, 3))
    band_axis = np.sin(xs / w * 2 * math.pi + 0.7) * 0.35
    band = np.exp(-((lat - band_axis) ** 2) / (2 * 0.16 ** 2))
    neb = fbm_np(max(w, h), 201, 6, 5)[:h, :w]
    img += band[..., None] * (0.05 + 0.12 * neb[..., None]) * np.array([0.55, 0.6, 0.85])[None, None]
    img += (0.012 * fbm_np(max(w, h), 202, 3, 3)[:h, :w])[..., None] * np.array([0.3, 0.35, 0.6])[None, None]
    # Stars: 1 in ~400 texels, brightness and colour by hash; brighter in the band.
    r = hash_grid(xs.astype(np.int64), ys.astype(np.int64), 303)
    mag = hash_grid(xs.astype(np.int64) + 7, ys.astype(np.int64) + 3, 304)
    temp = hash_grid(xs.astype(np.int64) + 11, ys.astype(np.int64) + 5, 305)
    density = 0.0022 + 0.004 * band
    star = (r < density).astype(np.float64)
    bright = (0.4 + 4.0 * mag ** 6) * star
    colour = np.dstack((0.8 + 0.3 * temp, 0.85 + 0.1 * temp, 1.1 - 0.4 * temp))
    img += bright[..., None] * colour
    # Horizon dimming so the band does not cut through the ground gradient.
    img *= np.clip((lat + 0.15) / 0.3, 0, 1)[..., None]
    return np.dstack((img, np.ones((h, w))))


def grass_mask(n=256):
    """A tuft of blades with alpha; the green comes from the material factor."""
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float64)
    u, v = xs / n, 1.0 - ys / n
    alpha = np.zeros((n, n))
    shade = np.zeros((n, n))
    for k in range(11):
        x0 = 0.12 + 0.76 * (k / 10.0)
        lean = (island.h01(k, 1, 5) - 0.5) * 0.9
        height = 0.55 + 0.42 * island.h01(k, 2, 5)
        t = np.clip(v / height, 0, 1)
        cx = x0 + lean * t * t
        width = 0.035 * (1 - t) + 0.004
        blade = np.clip(1 - np.abs(u - cx) / width, 0, 1) * (v < height)
        alpha = np.maximum(alpha, blade)
        shade = np.maximum(shade, blade * (0.45 + 0.55 * t))
    rgb = np.dstack((0.55 + 0.45 * shade, 0.6 + 0.4 * shade, 0.45 + 0.35 * shade))
    return np.dstack((rgb, (alpha > 0.35).astype(np.float64)))


def flower_mask(n=128):
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float64)
    u, v = xs / n - 0.5, 1.0 - ys / n
    alpha = np.zeros((n, n))
    rgb = np.zeros((n, n, 3))
    stem = (np.abs(u) < 0.025) & (v < 0.55)
    alpha[stem] = 1
    rgb[stem] = [0.25, 0.45, 0.15]
    for k, (cx, cy, col) in enumerate(((-0.22, 0.72, (0.95, 0.35, 0.4)), (0.18, 0.8, (0.98, 0.85, 0.3)), (0.0, 0.62, (0.9, 0.5, 0.7)))):
        for p in range(5):
            ang = 2 * math.pi * p / 5 + k
            px, py = cx + 0.09 * math.cos(ang), cy + 0.09 * math.sin(ang)
            petal = ((u - px) ** 2 + (v - py) ** 2) < 0.055 ** 2
            alpha[petal] = 1
            rgb[petal] = col
        centre = ((u - cx) ** 2 + (v - cy) ** 2) < 0.045 ** 2
        rgb[centre] = [1.0, 0.9, 0.5]
        alpha[centre] = 1
    return np.dstack((rgb, alpha))


# -------------------------------------------------------------------------------------------- terrain
def terrain_chunks():
    """16 chunks of 50 m at 1 m, each split into sand / grass / dry grass / rock primitives."""
    half, cells = island.HALF, 200
    xs = [-half + 2 * half * i / cells for i in range(cells + 1)]
    hs = [[island.height(x, z) for x in xs] for z in xs]
    chunks = {}
    for cj in range(4):
        for ci in range(4):
            pos, nrm, uv = [], [], []
            idx_by = {"sand": [], "grass": [], "dry_grass": [], "rock": []}
            i0, j0 = ci * 50, cj * 50
            for j in range(51):
                for i in range(51):
                    gi, gj = i0 + i, j0 + j
                    x, z, y = xs[gi], xs[gj], hs[gj][gi]
                    pos += [r6(x), r6(y), r6(z)]
                    hl = hs[gj][max(gi - 1, 0)]
                    hr = hs[gj][min(gi + 1, cells)]
                    hd = hs[max(gj - 1, 0)][gi]
                    hu = hs[min(gj + 1, cells)][gi]
                    n = (-(hr - hl) * 0.5, 1.0, -(hu - hd) * 0.5)
                    l = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
                    nrm += [r6(n[0] / l), r6(n[1] / l), r6(n[2] / l)]
                    uv += [r6(x * 0.25), r6(z * 0.25)]
            for j in range(50):
                for i in range(50):
                    a, b = j * 51 + i, (j + 1) * 51 + i
                    for tri in ((a, b, a + 1), (a + 1, b, b + 1)):
                        cy = sum(pos[3 * t + 1] for t in tri) / 3.0
                        ny = sum(nrm[3 * t + 1] for t in tri) / 3.0
                        cx = sum(pos[3 * t] for t in tri) / 3.0
                        cz = sum(pos[3 * t + 2] for t in tri) / 3.0
                        if cy < 1.3:
                            kind = "sand"
                        elif ny < 0.80:
                            kind = "rock"
                        elif cy > 34.0 or island.vnoise(cx * 0.03 + 50, cz * 0.03 + 50, 77) > 0.68:
                            kind = "dry_grass"
                        else:
                            kind = "grass"
                        idx_by[kind] += list(tri)
            chunks["island_%02d.glb" % (cj * 4 + ci)] = (pos, nrm, uv, idx_by)
    return chunks


def rings_disc(radius, rings, y=0.0, uv_scale=0.02):
    pos, nrm, uv, idx = [0, r6(y), 0], [0, 1, 0], [0, 0], []
    seg = 48
    radii = [radius * (k + 1) / rings for k in range(rings)]
    for r, rad in enumerate(radii):
        for s in range(seg):
            ph = 2 * math.pi * s / seg
            pos += [r6(rad * math.cos(ph)), r6(y), r6(-rad * math.sin(ph))]
            nrm += [0, 1, 0]
            uv += [r6(rad * math.cos(ph) * uv_scale), r6(rad * math.sin(ph) * uv_scale)]
    for s in range(seg):
        idx += [0, 1 + s, 1 + (s + 1) % seg]
    for r in range(rings - 1):
        for s in range(seg):
            a, b = 1 + r * seg + s, 1 + r * seg + (s + 1) % seg
            c, d = 1 + (r + 1) * seg + s, 1 + (r + 1) * seg + (s + 1) % seg
            idx += [a, c, b, b, c, d]
    return pos, nrm, uv, idx


# -------------------------------------------------------------------------------------------- models
def tex_mat(g, name, tex, tint=(1, 1, 1, 1), rough=1.0, double=False, alpha=None):
    m = {"name": name, "pbrMetallicRoughness": {
        "baseColorFactor": list(tint),
        "baseColorTexture": {"index": g.texture("../textures/%s_albedo.png" % tex)},
        "metallicRoughnessTexture": {"index": g.texture("../textures/%s_orm.png" % tex)},
        "metallicFactor": 1.0, "roughnessFactor": rough},
        "normalTexture": {"index": g.texture("../textures/%s_normal.png" % tex)},
        "occlusionTexture": {"index": g.texture("../textures/%s_orm.png" % tex)}}
    if double:
        m["doubleSided"] = True
    if alpha:
        m["alphaMode"] = alpha
    return g.material(m)


def flat_mat(g, name, color, metallic=0.0, rough=0.6, emissive=None, strength=1.0, alpha=None, double=False):
    m = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(color), "metallicFactor": metallic,
                                                "roughnessFactor": rough}}
    if emissive:
        m["emissiveFactor"] = list(emissive)
        if strength != 1.0:
            m["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": strength}}
            g.doc["extensionsUsed"] = ["KHR_materials_emissive_strength"]
    if alpha:
        m["alphaMode"] = alpha
    if double:
        m["doubleSided"] = True
    return g.material(m)


def mesh_prims(g, name, prims):
    """One glTF mesh from [(geo, material)] pairs: a submesh per material."""
    out = []
    for geo, mat in prims:
        data = geo.data() if isinstance(geo, Geo) else geo
        if not data[3]:
            continue
        acc = g.geometry(data)
        out.append({"attributes": dict(acc["attributes"]), "indices": acc["indices"], "material": mat})
    g.doc["meshes"].append({"name": name, "primitives": out})
    return len(g.doc["meshes"]) - 1


def single(name, geo, mat_fn):
    g = Gltf()
    g.node({"name": name, "mesh": g.mesh(name, geo.data() if isinstance(geo, Geo) else geo, mat_fn(g))}, root=True)
    return g


def anim(g, name, channels):
    samplers, chans = [], []
    for node, path, interp, times, values, comps in channels:
        samplers.append({"input": g.accessor(times, 1, minmax=True), "output": g.accessor(values, comps),
                         "interpolation": interp})
        chans.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})
    g.doc.setdefault("animations", []).append({"name": name, "samplers": samplers, "channels": chans})


def quat_axis(axis, angle):
    s = math.sin(angle / 2)
    return [r6(axis[0] * s), r6(axis[1] * s), r6(axis[2] * s), r6(math.cos(angle / 2))]


def spin_channel(node, axis, period, steps=8):
    times = [r6(period * k / steps) for k in range(steps + 1)]
    values = []
    for k in range(steps + 1):
        values += quat_axis(axis, 2 * math.pi * k / steps)
    return (node, "rotation", "LINEAR", times, values, 4)


def sway_channel(node, axis, amplitude, period, steps=8, base=None):
    times = [r6(period * k / steps) for k in range(steps + 1)]
    values = []
    for k in range(steps + 1):
        a = amplitude * math.sin(2 * math.pi * k / steps)
        q = quat_axis(axis, a)
        values += q
    return (node, "rotation", "LINEAR", times, values, 4)


def cone(radius, y0, y1, seg):
    return lathe([(radius, y0), (0.0001, y1)], seg)


def build_models():
    models = {}

    # ---- terrain chunks and the sea
    for name, (pos, nrm, uv, idx_by) in terrain_chunks().items():
        g = Gltf()
        mats = {k: tex_mat(g, k, k) for k in idx_by}
        acc_geo = g.geometry((pos, nrm, uv, [0, 0, 0]))  # positions shared; indices per primitive below
        prims = []
        for kind, idx in idx_by.items():
            if not idx:
                continue
            prims.append({"attributes": dict(acc_geo["attributes"]),
                          "indices": g.accessor(idx, 1, 5123 if max(idx) < 65536 else 5125, target=34963),
                          "material": mats[kind]})
        g.doc["meshes"].append({"name": name[:-4], "primitives": prims})
        g.node({"name": name[:-4], "mesh": len(g.doc["meshes"]) - 1}, root=True)
        models[name] = g
    # The terrain stops at [-HALF, HALF]^2; without a floor under the rest of the disc the translucent water
    # shows the seabed inside that square and the clear colour outside it (a hard square edge). The seabed
    # disc sits just under the terrain's sea floor and carries the terrain's sand.
    g = Gltf()
    g.node({"name": "sea", "mesh": mesh_prims(g, "sea", [
        (rings_disc(420.0, 1, island.SEA_FLOOR - 0.05, uv_scale=0.25), tex_mat(g, "seabed", "sand")),
        (rings_disc(420.0, 5), flat_mat(g, "sea", (0.05, 0.30, 0.34, 0.84), 0.0, 0.06, alpha="BLEND")),
    ])}, root=True)
    models["sea.glb"] = g

    # ---- the Vault: floor, entablature ring, dome with an oculus; 16 columns are placed by the scene
    v = Geo()
    v.add(disc(11.0, 0.0, 48))
    v.add(lathe([(11.0, 0.0), (11.0, 0.35), (10.6, 0.35), (10.6, 0.7), (9.9, 0.7)], 48))  # the step
    v.add(lathe([(8.8, 5.0), (10.3, 5.0), (10.3, 5.9), (8.8, 5.9)], 48))                    # entablature
    dome = []
    for k in range(13):
        t = k / 12.0
        ang = t * (math.pi / 2 - 0.2)
        dome.append((10.2 * math.cos(ang), 5.9 + 10.2 * math.sin(ang) * 0.75))
    v.add(lathe(dome, 48, uv_v_scale=0.5))
    inner = [(r6(r * 0.97), r6(y - 0.3)) for r, y in reversed(dome)]
    v.add(lathe(inner, 48, uv_v_scale=0.5))
    g = Gltf()
    g.node({"name": "vault", "mesh": mesh_prims(g, "vault", [(v, tex_mat(g, "vault", "ashlar", (1.05, 1.0, 0.92, 1)))])}, root=True)
    models["vault.glb"] = g
    d = Geo()
    d.add(lathe([(2.6, 0.0), (2.6, 0.25), (2.0, 0.25), (2.0, 0.5), (1.4, 0.5), (1.4, 0.75), (0.0, 0.75)], 32))
    models["dais.glb"] = single("dais", d, lambda g: tex_mat(g, "dais", "flagstone"))

    # ---- columns, arches, walls, stairs
    models["column.glb"] = single("column", column_geo(5.0, 0.42), lambda g: tex_mat(g, "column", "ashlar", (1.05, 1.0, 0.95, 1)))
    a = Geo()
    for px in (-2.0, 2.0):
        a.add(box(0.5, 2.0, 0.6, 1.5), (px, 2.0, 0))
    a.add(box(2.6, 0.35, 0.65, 2.0), (0, 4.35, 0))
    a.add(box(2.7, 0.1, 0.75, 1.0), (0, 4.8, 0))
    models["arch.glb"] = single("arch", a, lambda g: tex_mat(g, "arch", "ashlar"))
    w = Geo()
    w.add(box(2.0, 0.45, 0.3, 2.0), (0, 0.45, 0))
    w.add(box(2.05, 0.08, 0.36, 1.0), (0, 0.95, 0))
    models["wall_low.glb"] = single("wall_low", w, lambda g: tex_mat(g, "wall", "ashlar", (0.9, 0.9, 0.85, 1)))
    s = Geo()
    for k in range(10):
        s.add(box(1.5, 0.15, 0.2, 1.0), (0, 0.15 + 0.3 * k, 0.2 + 0.4 * k))
    models["stair_flight.glb"] = single("stair_flight", s, lambda g: tex_mat(g, "stair", "flagstone"))

    # ---- the pier and a boat
    p = Geo()
    p.add(box(2.0, 0.12, 12.0, 4.0), (0, 1.3, 0))
    for z in (-10.0, -5.0, 0.0, 5.0, 10.0):
        for x in (-1.8, 1.8):
            p.add(lathe([(0.16, -7.5), (0.16, 1.9), (0.0001, 2.0)], 10), (x, 0, z))
    g = Gltf()
    wood = tex_mat(g, "wood", "bark", (0.9, 0.8, 0.7, 1))
    g.node({"name": "pier", "mesh": mesh_prims(g, "pier", [(p, wood)])}, root=True)
    models["pier.glb"] = g
    b = Geo()
    hull = [(0.3, 0.0), (0.9, 0.3), (1.1, 0.7), (1.05, 0.9)]
    b.add(lathe(hull, 20), (0, 0, 0), 0, (1.0, 1.0, 2.6))
    b.add(disc(1.05, 0.9, 20), (0, 0, 0), 0, (0.95, 1.0, 2.5))
    b.add(lathe([(0.06, 0.9), (0.04, 4.8), (0.0001, 4.9)], 8), (0, 0, -0.4))
    g = Gltf()
    paint = flat_mat(g, "paint", (0.08, 0.25, 0.5, 1), 0.0, 0.5)
    g.node({"name": "boat", "mesh": mesh_prims(g, "boat", [(b, paint)])}, root=True)
    models["boat.glb"] = g

    # ---- the windmill: a tapered tower, a cap, four sails that turn
    g = Gltf()
    stone = tex_mat(g, "tower", "ashlar", (1.0, 0.95, 0.88, 1))
    canvas = tex_mat(g, "canvas", "canvas", double=True)
    wood = flat_mat(g, "wood", (0.35, 0.25, 0.15, 1), 0.0, 0.8)
    tower = Geo()
    tower.add(lathe([(2.6, 0.0), (2.6, 0.4), (2.3, 0.4), (1.9, 9.0)], 32, uv_v_scale=0.5))
    tower.add(lathe([(2.2, 9.0), (2.2, 9.3), (0.0001, 11.4)], 32))
    root = g.node({"name": "windmill", "mesh": mesh_prims(g, "tower", [(tower, stone)])}, root=True)
    sails = Geo()
    sails.add(lathe([(0.35, -0.3), (0.35, 0.4), (0.0001, 0.5)], 12), (0, 0, 0), 0, (1, 1, 1))
    for k in range(4):
        ang = k * math.pi / 2
        arm = Geo()
        arm.add(box(0.08, 4.5, 0.08), (0, 4.5, 0.15))
        sails.add(arm.data(), (0, 0, 0), 0)
        # rotate the arm around Z by hand: Geo.add rotates about Y only
        pos, nrm, uv, idx = arm.data()
        c, s_ = math.cos(ang), math.sin(ang)
        rp = [r6(c * pos[i] - s_ * pos[i + 1]) if i % 3 == 0 else r6(s_ * pos[i - 1] + c * pos[i]) if i % 3 == 1 else pos[i] for i in range(len(pos))]
        rn = [r6(c * nrm[i] - s_ * nrm[i + 1]) if i % 3 == 0 else r6(s_ * nrm[i - 1] + c * nrm[i]) if i % 3 == 1 else nrm[i] for i in range(len(nrm))]
        sails.pos, sails.nrm, sails.uv, sails.idx = sails.pos[:-len(pos)], sails.nrm[:-len(nrm)], sails.uv[:-len(uv)], sails.idx[:-len(idx)]
        base = len(sails.pos) // 3
        sails.pos += rp
        sails.nrm += rn
        sails.uv += uv
        sails.idx += [i + base for i in idx]
    cloth = Geo()
    for k in range(4):
        ang = k * math.pi / 2
        pos, nrm, uv, idx = box(0.01, 1.7, 0.45)
        pos = [pos[i] if i % 3 != 1 else pos[i] + 2.6 for i in range(len(pos))]
        pos = [pos[i] + 0.0 if i % 3 != 2 else pos[i] + 0.6 for i in range(len(pos))]
        c, s_ = math.cos(ang), math.sin(ang)
        rp = [r6(c * pos[i] - s_ * pos[i + 1]) if i % 3 == 0 else r6(s_ * pos[i - 1] + c * pos[i]) if i % 3 == 1 else pos[i] for i in range(len(pos))]
        rn = [r6(c * nrm[i] - s_ * nrm[i + 1]) if i % 3 == 0 else r6(s_ * nrm[i - 1] + c * nrm[i]) if i % 3 == 1 else nrm[i] for i in range(len(nrm))]
        base = len(cloth.pos) // 3
        cloth.pos += rp
        cloth.nrm += rn
        cloth.uv += uv
        cloth.idx += [i + base for i in idx]
    sails_node = g.node({"name": "sails", "mesh": mesh_prims(g, "sails", [(sails, wood), (cloth, canvas)]),
                         "translation": [0, 9.2, 2.3]}, parent=root)
    anim(g, "turn", [spin_channel(sails_node, (0, 0, 1), 14.0, 16)])
    models["windmill.glb"] = g

    # ---- the cistern: a square basin and its water
    c = Geo()
    for sx, sz, hx, hz in ((0, 5.25, 5.5, 0.25), (0, -5.25, 5.5, 0.25), (5.25, 0, 0.25, 5.0), (-5.25, 0, 0.25, 5.0)):
        c.add(box(hx, 0.6, hz, 2.0), (sx, 0.6, sz))
    c.add(box(5.0, 0.1, 5.0, 4.0), (0, 0.1, 0))
    water = Geo()
    water.add(box(4.98, 0.01, 4.98, 1.0), (0, 1.0, 0))
    g = Gltf()
    stone = tex_mat(g, "basin", "flagstone")
    pool = flat_mat(g, "pool", (0.04, 0.22, 0.26, 0.8), 0.0, 0.05, alpha="BLEND")
    g.node({"name": "cistern", "mesh": mesh_prims(g, "cistern", [(c, stone), (water, pool)])}, root=True)
    models["cistern.glb"] = g

    # ---- the lantern: an iron post, a glass, a flame
    g = Gltf()
    iron = flat_mat(g, "iron", (0.05, 0.05, 0.06, 1), 1.0, 0.45)
    glass = flat_mat(g, "glass", (1.0, 0.75, 0.45, 0.35), 0.0, 0.1, alpha="BLEND", double=True)
    flame = flat_mat(g, "flame", (1.0, 0.7, 0.3, 1), 0.0, 1.0, emissive=(1.0, 0.62, 0.28), strength=12.0)
    post = Geo()
    post.add(lathe([(0.12, 0.0), (0.12, 0.1), (0.05, 0.1), (0.05, 2.2), (0.08, 2.25), (0.08, 2.35)], 10))
    post.add(box(0.09, 0.02, 0.35), (0, 2.35, 0.2))
    post.add(lathe([(0.16, 2.0), (0.16, 2.05), (0.13, 2.05), (0.13, 2.4), (0.18, 2.45), (0.0001, 2.6)], 10), (0, -0.2, 0.42))
    gl = Geo()
    gl.add(lathe([(0.12, 1.86), (0.12, 2.22)], 10), (0, 0, 0.42))
    fl = Geo()
    fl.add(uv_sphere(0.05, 8, 6), (0, 1.98, 0.42))
    g.node({"name": "lantern", "mesh": mesh_prims(g, "lantern", [(post, iron), (fl, flame), (gl, glass)])}, root=True)
    models["lantern.glb"] = g

    # ---- the banner: a pole and a cloth that flutters
    g = Gltf()
    wood = flat_mat(g, "pole", (0.3, 0.22, 0.14, 1), 0.0, 0.8)
    cloth_m = flat_mat(g, "cloth", (0.62, 0.12, 0.10, 1), 0.0, 0.9, double=True)
    pole = Geo()
    pole.add(lathe([(0.06, 0.0), (0.05, 4.4), (0.0001, 4.5)], 8))
    pole.add(box(0.7, 0.03, 0.03), (0, 4.25, 0))
    root = g.node({"name": "banner", "mesh": mesh_prims(g, "pole", [(pole, wood)])}, root=True)
    flag = Geo()
    flag.add(box(0.62, 0.9, 0.012), (0, -0.92, 0))
    cloth_node = g.node({"name": "cloth", "mesh": mesh_prims(g, "cloth", [(flag, cloth_m)]), "translation": [0, 4.25, 0]}, parent=root)
    anim(g, "flutter", [sway_channel(cloth_node, (1, 0, 0), 0.22, 1.1, 8)])
    anim(g, "gust", [sway_channel(cloth_node, (1, 0, 0), 0.55, 0.7, 8)])
    models["banner.glb"] = g

    # ---- trees
    g = Gltf()
    bark = tex_mat(g, "bark", "bark")
    leaf = flat_mat(g, "olive_leaf", (0.42, 0.50, 0.30, 1), 0.0, 1.0)
    trunk = Geo()
    trunk.add(lathe([(0.45, 0.0), (0.32, 0.6), (0.26, 1.6), (0.3, 2.4), (0.2, 3.0)], 12, uv_v_scale=0.5))
    trunk.add(lathe([(0.14, 2.6), (0.08, 3.9)], 8), (0.6, 0, 0.2), 0.0)
    trunk.add(lathe([(0.14, 2.6), (0.08, 3.9)], 8), (-0.5, 0, -0.3), 0.0)
    crown = Geo()
    for k, (ox, oy, oz, rr) in enumerate(((0, 3.9, 0, 1.6), (1.1, 3.6, 0.5, 1.2), (-0.9, 3.5, -0.6, 1.25), (0.2, 4.6, -0.9, 1.0))):
        crown.add(rock(rr, 300 + k, 1, 0.3), (ox, oy, oz), 0.0, (1.2, 0.8, 1.2))
    g.node({"name": "olive", "mesh": mesh_prims(g, "olive", [(trunk, bark), (crown, leaf)])}, root=True)
    models["olive.glb"] = g
    g = Gltf()
    bark = tex_mat(g, "bark", "bark")
    dark = flat_mat(g, "cypress_leaf", (0.09, 0.16, 0.08, 1), 0.0, 1.0)
    trunk = Geo()
    trunk.add(lathe([(0.22, 0.0), (0.14, 2.0)], 8))
    crown = Geo()
    crown.add(lathe([(0.05, 1.3), (0.9, 3.0), (1.05, 5.5), (0.75, 8.5), (0.3, 10.8), (0.0001, 11.8)], 10, uv_v_scale=0.3))
    g.node({"name": "cypress", "mesh": mesh_prims(g, "cypress", [(trunk, bark), (crown, dark)])}, root=True)
    models["cypress.glb"] = g
    g = Gltf()
    bark = tex_mat(g, "bark", "bark")
    needle = flat_mat(g, "pine_leaf", (0.14, 0.26, 0.12, 1), 0.0, 1.0)
    trunk = Geo()
    trunk.add(lathe([(0.3, 0.0), (0.18, 5.5)], 8))
    crown = Geo()
    for k, (y0, rr) in enumerate(((2.2, 2.6), (4.0, 2.1), (5.6, 1.5), (7.0, 0.9))):
        crown.add(cone(rr, y0, y0 + 2.6, 10))
    g.node({"name": "pine", "mesh": mesh_prims(g, "pine", [(trunk, bark), (crown, needle)])}, root=True)
    models["pine.glb"] = g

    # ---- rocks, grass, flowers, sunshards
    for name, (rad, seed, sc) in (("rock_a", (1.4, 21, (1.3, 0.8, 1.0))), ("rock_b", (0.9, 22, (1.0, 0.7, 1.4))), ("rock_c", (2.2, 23, (1.1, 0.6, 1.0)))):
        geo = Geo()
        geo.add(rock(rad, seed, 2, 0.45), (0, 0, 0), 0.0, sc)
        models[name + ".glb"] = single(name, geo, lambda g: tex_mat(g, "rock", "rock"))
    g = Gltf()
    gm = {"name": "grass", "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.75, 0.3, 1],
                                                     "baseColorTexture": {"index": g.texture("../textures/grass_tuft.png")},
                                                     "metallicFactor": 0.0, "roughnessFactor": 1.0},
          "alphaMode": "MASK", "alphaCutoff": 0.5, "doubleSided": True}
    gmat = g.material(gm)
    tuft = Geo()
    for k in range(3):
        quad = ([-0.35, 0, 0, 0.35, 0, 0, 0.35, 0.6, 0, -0.35, 0.6, 0], [0, 0, 1] * 4, [0, 1, 1, 1, 1, 0, 0, 0], [0, 1, 2, 2, 3, 0])
        tuft.add(quad, (0, 0, 0), k * math.pi / 3)
    g.node({"name": "grass_tuft", "mesh": g.mesh("grass_tuft", tuft.data(), gmat)}, root=True)
    models["grass_tuft.glb"] = g
    g = Gltf()
    fm = {"name": "flower", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1],
                                                      "baseColorTexture": {"index": g.texture("../textures/flower.png")},
                                                      "metallicFactor": 0.0, "roughnessFactor": 1.0},
          "alphaMode": "MASK", "alphaCutoff": 0.5, "doubleSided": True}
    fmat = g.material(fm)
    fl = Geo()
    for k in range(2):
        quad = ([-0.2, 0, 0, 0.2, 0, 0, 0.2, 0.4, 0, -0.2, 0.4, 0], [0, 0, 1] * 4, [0, 1, 1, 1, 1, 0, 0, 0], [0, 1, 2, 2, 3, 0])
        fl.add(quad, (0, 0, 0), k * math.pi / 2)
    g.node({"name": "flower", "mesh": g.mesh("flower", fl.data(), fmat)}, root=True)
    models["flower.glb"] = g
    sh = Geo()
    sh.add(lathe([(0.0001, 0.0), (0.12, 0.15), (0.17, 0.5), (0.1, 0.9), (0.0001, 1.15)], 6), (0, 0, 0), 0.0)
    sh.add(lathe([(0.0001, 0.0), (0.08, 0.1), (0.1, 0.35), (0.0001, 0.65)], 5), (0.14, 0.0, 0.1), 0.4)
    models["sunshard.glb"] = single("sunshard", sh, lambda g: flat_mat(g, "shard", (1.0, 0.8, 0.45, 1), 0.0, 0.25, emissive=(1.0, 0.72, 0.32), strength=5.0))

    # ---- the gull: a body and two wings that beat
    g = Gltf()
    white = flat_mat(g, "feather", (0.92, 0.92, 0.9, 1), 0.0, 0.8)
    grey = flat_mat(g, "wingtip", (0.35, 0.35, 0.38, 1), 0.0, 0.8, double=True)
    body = Geo()
    body.add(uv_sphere(0.16, 10, 6), (0, 0, 0), 0.0, (1.0, 0.8, 2.2))
    body.add(cone(0.05, 0, 0.3, 6), (0, -0.02, 0.4), 0.0, (1, 1, 1))
    root = g.node({"name": "gull", "mesh": mesh_prims(g, "body", [(body, white)])}, root=True)
    for side in (-1, 1):
        wing = Geo()
        wing.add(box(0.55, 0.012, 0.16), (side * 0.6, 0, 0))
        node = g.node({"name": "wing_%s" % ("l" if side < 0 else "r"), "mesh": mesh_prims(g, "wing", [(wing, grey)])}, parent=root)
        anim_name = "glide"
        if "animations" not in g.doc or all(a["name"] != anim_name for a in g.doc["animations"]):
            g.doc.setdefault("animations", []).append({"name": anim_name, "samplers": [], "channels": []})
        a = next(a for a in g.doc["animations"] if a["name"] == anim_name)
        ch = sway_channel(node, (0, 0, 1), 0.45 * -side, 0.9, 8)
        a["samplers"].append({"input": g.accessor(ch[3], 1, minmax=True), "output": g.accessor(ch[4], 4), "interpolation": "LINEAR"})
        a["channels"].append({"sampler": len(a["samplers"]) - 1, "target": {"node": node, "path": "rotation"}})
    models["gull.glb"] = g

    # ---- the heliostat: a pedestal with a plate, and the bronze dish on its own pivot
    mb = Geo()
    mb.add(lathe([(1.7, 0.0), (1.7, 0.18), (1.2, 0.18), (1.2, 0.3), (0.55, 0.3), (0.45, 1.6), (0.6, 1.75)], 32))
    models["mirror_base.glb"] = single("mirror_base", mb, lambda g: tex_mat(g, "pedestal", "flagstone"))
    g = Gltf()
    bronze = flat_mat(g, "bronze", (1.0, 0.72, 0.38, 1), 1.0, 0.12)
    dark = flat_mat(g, "dark_bronze", (0.35, 0.22, 0.1, 1), 1.0, 0.5)
    dish = Geo()
    bowl = [(0.0001, 0.0)] + [(1.5 * k / 10, 0.35 * (k / 10) ** 2) for k in range(1, 11)]
    dish.add(lathe(bowl, 40))
    back = [(r6(r), r6(y - 0.08)) for r, y in reversed(bowl)]
    dish.add(lathe(back, 40))
    dish.add(lathe([(1.5, 0.27), (1.5, 0.35)], 40))
    axle = Geo()
    axle.add(box(0.9, 0.06, 0.06), (0, -0.15, 0))
    axle.add(uv_sphere(0.16, 12, 8), (0, -0.2, 0))
    g.node({"name": "mirror_dish", "mesh": mesh_prims(g, "mirror_dish", [(dish, bronze), (axle, dark)])}, root=True)
    models["mirror_dish.glb"] = g
    beam = Geo()
    beam.add(box(0.5, 0.5, 0.5), (0, 0, 0.5))
    models["beam.glb"] = single("beam", beam, lambda g: flat_mat(g, "beam", (1.0, 0.85, 0.55, 0.32), 0.0, 1.0,
                                                                  emissive=(1.0, 0.8, 0.5), strength=3.0, alpha="BLEND", double=True))

    # ---- the Keeper: a rigid proxy with a walk and an idle clip (no skinning, FR-R3D-060)
    g = Gltf()
    cloak = flat_mat(g, "cloak", (0.18, 0.16, 0.22, 1), 0.0, 0.9)
    skin = flat_mat(g, "skin", (0.7, 0.55, 0.42, 1), 0.0, 0.7)
    wood = flat_mat(g, "staff", (0.3, 0.22, 0.14, 1), 0.0, 0.8)
    lamp = flat_mat(g, "lamp", (1.0, 0.7, 0.35, 1), 0.0, 1.0, emissive=(1.0, 0.65, 0.3), strength=8.0)
    root = g.node({"name": "keeper"}, root=True)
    body = Geo()
    body.add(lathe([(0.2, 0.0), (0.34, 0.1), (0.3, 1.0), (0.22, 1.35), (0.26, 1.45), (0.0001, 1.5)], 16))
    hips = g.node({"name": "body", "mesh": mesh_prims(g, "body", [(body, cloak)]), "translation": [0, 0.1, 0]}, parent=root)
    head = Geo()
    head.add(uv_sphere(0.14, 12, 8), (0, 0, 0.02))
    hood = Geo()
    hood.add(lathe([(0.17, -0.12), (0.2, 0.05), (0.14, 0.22), (0.0001, 0.26)], 12), (0, 0, -0.03))
    g.node({"name": "head", "mesh": mesh_prims(g, "head", [(head, skin), (hood, cloak)]), "translation": [0, 1.52, 0]}, parent=hips)
    staff = Geo()
    staff.add(lathe([(0.025, 0.0), (0.02, 1.9)], 8))
    staff_node = g.node({"name": "staff", "mesh": mesh_prims(g, "staff", [(staff, wood)]), "translation": [0.36, 0.0, 0.1]}, parent=hips)
    lantern = Geo()
    lantern.add(box(0.07, 0.1, 0.07), (0, 1.7, 0.12))
    g.node({"name": "lamp", "mesh": mesh_prims(g, "lamp", [(lantern, lamp)])}, parent=staff_node)
    walk_t = [r6(0.8 * k / 8) for k in range(9)]
    bob = []
    for k in range(9):
        bob += [0, r6(0.1 + 0.05 * abs(math.sin(2 * math.pi * k / 8 * 2))), 0]
    anim(g, "walk", [(hips, "translation", "LINEAR", walk_t, bob, 3), sway_channel(staff_node, (1, 0, 0), 0.25, 0.8, 8)])
    idle_t = [0.0, 1.5, 3.0]
    anim(g, "idle", [(hips, "translation", "LINEAR", idle_t, [0, 0.1, 0, 0, 0.12, 0, 0, 0.1, 0], 3)])
    models["keeper.glb"] = g
    return models


def tri_count(g):
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
    for name, (alb, nrm, o) in material_textures().items():
        for suffix, px, is_srgb in (("albedo", alb, True), ("normal", nrm, False), ("orm", o, False)):
            p = textures / ("%s_%s.png" % (name, suffix))
            save_rgba(p, px, is_srgb)
            sidecar(p, "texture", 1, {} if is_srgb else {"srgb": False})
    for name, args in (("flagstone", (256, 4, 3, 3, (0.55, 0.50, 0.44), (0.2, 0.18, 0.16), 3)),
                       ("ashlar", (256, 8, 4, 4, (0.72, 0.66, 0.56), (0.32, 0.29, 0.25), 9))):
        a, n, o = stone_textures(*args)
        for suffix, px, is_srgb in (("albedo", a, True), ("normal", n, False), ("orm", o, False)):
            p = textures / ("%s_%s.png" % (name, suffix))
            write_png(p, 256, 256, px)
            sidecar(p, "texture", 1, {} if is_srgb else {"srgb": False})
    save_rgba(textures / "stars_equirect.png", stars_equirect(), True)
    sidecar(textures / "stars_equirect.png", "texture", 1, {})
    save_rgba(textures / "grass_tuft.png", grass_mask(), True)
    sidecar(textures / "grass_tuft.png", "texture", 1, {})
    save_rgba(textures / "flower.png", flower_mask(), True)
    sidecar(textures / "flower.png", "texture", 1, {})
    report = []
    for name, g in build_models().items():
        tris = tri_count(g)
        budget = BUDGETS[name]
        assert tris <= budget, "%s: %d triangles over its budget %d" % (name, tris, budget)
        g.write(ROOT / "models" / name)
        sidecar(ROOT / "models" / name, "gltf", 2, {"max_triangles": budget})
        report.append((name, tris, budget))
    total = 0
    for name, tris, budget in report:
        total += tris
        print("  %-18s %6d / %6d" % (name, tris, budget))
    print("SolsticeReach assets written to %s (%d triangles in the kit)" % (ROOT, total))


if __name__ == "__main__":
    main()
