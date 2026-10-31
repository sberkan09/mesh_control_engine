#!/usr/bin/env python3
"""Textures and meshes of the 09_openworld buildings (M-OPENWORLD art pass).

    python3 samples/09_openworld/tools/gen_buildings.py          writes the facade textures (needs numpy and Pillow)

`gen_assets.py` imports `building_glb()` and writes one small .glb per lot: the wall boxes are built at the lot's exact size, and
the UVs are in bays and floors, not in the box's own units, so a window is the same size on every building (the Kenney models were
stretched to the lot, which smeared their windows). The bay is snapped to a whole number per wall, the floor height to a whole
number of floors, so no window is cut off at a corner.

Textures (assets/models/buildings/Textures/, 512 px, tiling, drawn here, seeded; original to this repo, CC0), per facade style
`<style>_color.jpg`, `_normal.jpg` (import srgb false), `_orm.jpg` (R occlusion, G roughness, B metallic; srgb false) and
`_emissive.jpg` (the lit windows):
    one tile = 4 bays x 4 floors (12 m x 12.8 m); the ground band `shop_*` is 4 bays x 6 m.
Styles: glass_tower, concrete_office, brick, stucco, modern_white; shop_a, shop_b; `trim` (concrete) and `roof` (gravel) are plain.
"""
import json
import math
import os
import random
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_textures import fractal, normal_map  # noqa: E402

OUT = os.path.normpath(os.path.join(HERE, "..", "assets", "models", "buildings", "Textures"))
N = 512
BAY = 3.0        # metres per bay (snapped per wall)
FLOOR = 3.2      # metres per floor (snapped per building)
TILE_BAYS = 4
TILE_FLOORS = 4
GROUND_H = 4.5   # metres: the shop band
SHOP_TILE_H = 6.0

# name: (wall colour, frame colour, glass colour, window width, window height, sill, wall roughness, kind)
STYLES = {
    "glass_tower":     ((0.10, 0.12, 0.14), (0.05, 0.05, 0.06), (0.05, 0.09, 0.13), 0.94, 0.78, False, 0.55, "curtain"),
    "concrete_office": ((0.52, 0.52, 0.50), (0.08, 0.08, 0.09), (0.06, 0.09, 0.12), 0.88, 0.48, True, 0.85, "ribbon"),
    "brick":           ((0.42, 0.19, 0.14), (0.78, 0.76, 0.70), (0.07, 0.09, 0.11), 0.46, 0.60, True, 0.9, "punched"),
    "stucco":          ((0.72, 0.64, 0.50), (0.88, 0.87, 0.83), (0.07, 0.10, 0.12), 0.44, 0.58, True, 0.9, "punched"),
    "modern_white":    ((0.76, 0.76, 0.74), (0.10, 0.10, 0.11), (0.06, 0.10, 0.14), 0.70, 0.62, False, 0.7, "punched"),
}
STYLE_NAMES = list(STYLES)
SHOPS = {"shop_a": (0.55, 0.12, 0.10), "shop_b": (0.12, 0.30, 0.42)}   # the fascia colour above the shop windows


def _save(prefix, color, normal, orm, emissive=None):
    os.makedirs(OUT, exist_ok=True)
    def u8(a):
        return (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(u8(color)).save(os.path.join(OUT, prefix + "_color.jpg"), quality=88, subsampling=0)
    Image.fromarray(u8(normal)).save(os.path.join(OUT, prefix + "_normal.jpg"), quality=90, subsampling=0)
    Image.fromarray(u8(orm)).save(os.path.join(OUT, prefix + "_orm.jpg"), quality=90, subsampling=0)
    if emissive is not None:
        Image.fromarray(u8(emissive)).save(os.path.join(OUT, prefix + "_emissive.jpg"), quality=88, subsampling=0)


def _facade(name, seed):
    wall, frame, glass, ww, wh, sill, wrough, kind = STYLES[name]
    rng = np.random.default_rng(seed)
    cw, ch = N // TILE_BAYS, N // TILE_FLOORS          # one cell: one bay, one floor
    color = np.zeros((N, N, 3), np.float32)
    height = np.ones((N, N), np.float32)
    ao = np.ones((N, N), np.float32)
    rough = np.full((N, N), wrough, np.float32)
    metal = np.zeros((N, N), np.float32)
    emis = np.zeros((N, N, 3), np.float32)
    grain = fractal(rng, 1.3)
    fine = fractal(rng, 0.25)
    base = np.array(wall, np.float32)
    color[:] = base * (1.0 + 0.16 * (grain - 0.5) + 0.07 * (fine - 0.5))[..., None]
    if name == "brick":
        yy, xx = np.mgrid[0:N, 0:N]
        course = (yy // 10)
        joint = ((yy % 10) < 2) | (((xx + (course % 2) * 12) % 24) < 2)
        tone = rng.uniform(0.82, 1.15, (N // 10 + 1, N // 24 + 2))[course, ((xx + (course % 2) * 12) // 24) % (N // 24 + 2)]
        color = np.where(joint[..., None], np.array((0.50, 0.46, 0.42), np.float32) * (0.9 + 0.2 * grain)[..., None], color * tone[..., None])
        height = np.where(joint, 0.82, 1.0).astype(np.float32)
    if name == "modern_white":                           # vertical fins between the bays
        xx = np.mgrid[0:N, 0:N][1]
        fin = (xx % cw) < 6
        color = np.where(fin[..., None], color * 0.86, color)
        height = np.where(fin, 1.12, height)
    for fy in range(TILE_FLOORS):
        for bx in range(TILE_BAYS):
            x0, y0 = bx * cw, fy * ch
            wpx, hpx = int(cw * ww), int(ch * wh)
            if kind == "curtain":
                wx0, wy0 = x0 + (cw - wpx) // 2, y0 + int(ch * 0.04)
            elif kind == "ribbon":
                wx0, wy0 = x0 + (cw - wpx) // 2, y0 + int(ch * 0.26)
            else:
                wx0, wy0 = x0 + (cw - wpx) // 2, y0 + int(ch * 0.22)
            ys, xs = slice(wy0, wy0 + hpx), slice(wx0, wx0 + wpx)
            fr = max(3, int(cw * 0.04))
            # frame, then the recessed glass inside it
            color[ys, xs] = frame
            height[ys, xs] = 0.55
            rough[ys, xs] = 0.4
            gy, gx = slice(wy0 + fr, wy0 + hpx - fr), slice(wx0 + fr, wx0 + wpx - fr)
            lit = rng.random() < (0.16 if kind != "curtain" else 0.12)
            shade = rng.uniform(0.7, 1.5)
            tint = np.array(glass, np.float32) * shade
            if rng.random() < 0.25:                       # blinds drawn: a pale, rough window
                tint = np.array((0.55, 0.52, 0.46), np.float32) * rng.uniform(0.6, 1.0)
                rough[gy, gx] = 0.8
            else:
                rough[gy, gx] = 0.06
            color[gy, gx] = tint
            height[gy, gx] = 0.15
            if kind != "curtain":                         # a mullion down the middle of wide windows
                mid = wx0 + wpx // 2
                color[wy0:wy0 + hpx, mid - fr // 2:mid + fr // 2] = frame
                height[wy0:wy0 + hpx, mid - fr // 2:mid + fr // 2] = 0.55
            ao[wy0 - 3:wy0 + hpx + 3, wx0 - 3:wx0 + wpx + 3] = 0.78      # shadow around the recess
            ao[ys, xs] = 0.6
            if lit:
                warm = np.array((1.0, 0.72, 0.38), np.float32) if rng.random() < 0.8 else np.array((0.55, 0.7, 1.0), np.float32)
                emis[gy, gx] = warm * rng.uniform(0.5, 1.0)
                color[gy, gx] = color[gy, gx] * 0.5 + warm * 0.18
            if sill:
                sy = slice(wy0 + hpx, wy0 + hpx + max(4, ch // 14))
                sx = slice(wx0 - 4, wx0 + wpx + 4)
                color[sy, sx] = np.array(frame, np.float32) * 0.95
                height[sy, sx] = 1.2
            if kind == "curtain":                         # spandrel: the dark band at the floor line
                sp = slice(y0 + int(ch * 0.84), y0 + ch)
                color[sp, x0:x0 + cw] = np.array(wall, np.float32) * (1.0 + 0.2 * (fine[sp, x0:x0 + cw] - 0.5))[..., None]
                height[sp, x0:x0 + cw] = 1.0
                metal[sp, x0:x0 + cw] = 0.3
    if kind == "curtain":
        metal = np.maximum(metal, 0.0)
    orm = np.stack([ao, np.clip(rough, 0, 1), metal], -1)
    _save(name, color, normal_map(height, 6.0), orm, np.clip(emis, 0, 1))


def _shop(name, fascia, seed):
    rng = np.random.default_rng(seed)
    h = int(N * 0.5)                                    # 512 x 256: four bays x 6 m
    cw = N // 4
    color = np.zeros((h, N, 3), np.float32)
    height = np.ones((h, N), np.float32)
    ao = np.ones((h, N), np.float32)
    rough = np.full((h, N), 0.85, np.float32)
    metal = np.zeros((h, N), np.float32)
    emis = np.zeros((h, N, 3), np.float32)
    grain = fractal(rng, 1.2)[:h]
    color[:] = np.array((0.50, 0.49, 0.47), np.float32) * (1.0 + 0.2 * (grain - 0.5))[..., None]
    px = h / SHOP_TILE_H                               # pixels per metre vertically
    top = int(h - GROUND_H * px)                       # the band's top edge in image rows (v grows upward in the glb UVs; rows count downward)
    # rows: image row 0 is the top of the texture = v 1. The band occupies rows [h - GROUND_H*px, h).
    plinth = slice(h - int(0.5 * px), h)
    color[plinth] = np.array((0.30, 0.30, 0.30), np.float32)
    fas = slice(top, top + int(0.9 * px))              # fascia / sign band under the cornice
    color[fas] = np.array(fascia, np.float32) * (0.9 + 0.2 * grain[fas])[..., None]
    height[fas] = 1.1
    win_top, win_bot = top + int(1.1 * px), h - int(0.7 * px)
    door_bay = int(rng.integers(0, 4))
    for bx in range(4):
        x0 = bx * cw + int(cw * 0.08)
        x1 = (bx + 1) * cw - int(cw * 0.08)
        ys = slice(win_top, win_bot)
        if bx == door_bay:
            x0 += int(cw * 0.08)
            x1 -= int(cw * 0.08)
            color[ys, x0:x1] = (0.14, 0.10, 0.07)
            rough[ys, x0:x1] = 0.5
            gy = slice(win_top + int(0.15 * px), win_bot - int(1.0 * px))
            color[gy, x0 + 8:x1 - 8] = (0.07, 0.10, 0.12)
            rough[gy, x0 + 8:x1 - 8] = 0.06
            height[ys, x0:x1] = 0.6
        else:
            color[ys, x0:x1] = (0.06, 0.10, 0.13)
            rough[ys, x0:x1] = 0.05
            height[ys, x0:x1] = 0.1
            fr = 4
            for part in (slice(win_top, win_top + fr), slice(win_bot - fr, win_bot)):
                color[part, x0:x1] = (0.08, 0.08, 0.09)
                height[part, x0:x1] = 0.55
            for cx_ in (x0, x1 - fr):
                color[ys, cx_:cx_ + fr] = (0.08, 0.08, 0.09)
                height[ys, cx_:cx_ + fr] = 0.55
            if rng.random() < 0.5:                      # a shop with the lights on
                inner = (slice(win_top + fr, win_bot - fr), slice(x0 + fr, x1 - fr))
                emis[inner] = np.array((1.0, 0.82, 0.55), np.float32) * rng.uniform(0.45, 0.8)
                color[inner] = color[inner] * 0.4 + np.array((0.5, 0.45, 0.35), np.float32) * 0.2
        ao[win_top - 4:win_bot + 4, x0 - 4:x1 + 4] = 0.8
    orm = np.stack([ao, rough, metal], -1)
    _save(name, color, normal_map(height, 6.0), orm, np.clip(emis, 0, 1))


def _plain(name, base, rough_v, seed, scale):
    rng = np.random.default_rng(seed)
    mid, fine = fractal(rng, 1.4, N), fractal(rng, 0.3, N)
    v = 1.0 + scale * (mid - 0.5) + 0.5 * scale * (fine - 0.5)
    if name == "roof":
        stones = fractal(rng, 0.15, N) > 0.9
        v = np.where(stones, v * 1.5, v)
    color = np.array(base, np.float32)[None, None, :] * v[..., None]
    orm = np.stack([np.ones((N, N), np.float32), np.full((N, N), rough_v, np.float32), np.zeros((N, N), np.float32)], -1)
    _save(name, color, normal_map(0.6 * mid + 0.4 * fine, 2.0), orm)


def write_textures():
    for i, name in enumerate(STYLE_NAMES):
        _facade(name, 900 + i)
        print("wrote", name)
    for i, (name, fascia) in enumerate(SHOPS.items()):
        _shop(name, fascia, 950 + i)
        print("wrote", name)
    _plain("trim", (0.58, 0.58, 0.56), 0.85, 960, 0.2)
    _plain("roof", (0.20, 0.20, 0.21), 0.95, 961, 0.35)
    print("wrote trim, roof")


# --- the mesh ---------------------------------------------------------------------------------------------------

class Prim:
    def __init__(self, material):
        self.material = material
        self.pos, self.nrm, self.uv, self.idx = [], [], [], []

    def quad(self, p, uv, n):
        """p: four corners counter-clockwise seen from outside, uv: their UVs, n: the face normal."""
        base = len(self.pos)
        self.pos += list(p)
        self.nrm += [n] * 4
        self.uv += list(uv)
        self.idx += [base, base + 1, base + 2, base, base + 2, base + 3]


def _walls(prim, x0, x1, z0, z1, y0, y1, bays_fn, floors, u_tile_m=None, v_tile=None, ground=False):
    """The four walls of a box [x0,x1] x [y0,y1] x [z0,z1]. UV u runs in tiles of TILE_BAYS bays per wall, v in tiles of TILE_FLOORS."""
    walls = (  # corners left to right seen from outside, normal
        (((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)), (0, 0, 1), x1 - x0),
        (((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)), (1, 0, 0), z1 - z0),
        (((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)), (0, 0, -1), x1 - x0),
        (((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)), (-1, 0, 0), z1 - z0),
    )
    for corners, n, length in walls:
        bays = max(1, round(length / BAY))
        u1 = bays / TILE_BAYS
        # glTF's v runs down the image: the wall's bottom is v = 1, the top v = 1 - (its height in tiles); the shop band fills the
        # lower 0.75 of its tile whatever the wall's height (a short building squashes it a little)
        v_top = 0.25 if ground else 1.0 - floors / TILE_FLOORS
        prim.quad(corners, [(0, 1), (u1, 1), (u1, v_top), (0, v_top)], n)


def _box(prim, x0, x1, y0, y1, z0, z1, tile=4.0):
    """A closed box with world-metre planar UVs (trim, roof gear)."""
    faces = (
        (((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)), (0, 0, 1), lambda p: (p[0], p[1])),
        (((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)), (1, 0, 0), lambda p: (p[2], p[1])),
        (((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)), (0, 0, -1), lambda p: (p[0], p[1])),
        (((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)), (-1, 0, 0), lambda p: (p[2], p[1])),
        (((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)), (0, 1, 0), lambda p: (p[0], p[2])),
        (((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)), (0, -1, 0), lambda p: (p[0], p[2])),
    )
    for corners, n, proj in faces:
        prim.quad(corners, [(proj(c)[0] / tile, proj(c)[1] / tile) for c in corners], n)


def build_geometry(w, d, h, style, shop, seed):
    """Prims for a building w x d (m) and h high, origin at the middle of its base, +Y up."""
    rng = random.Random(seed)
    ground, upper, trim, roof = Prim("shop_" + shop), Prim(style), Prim("trim"), Prim("roof")
    hw, hd = w / 2, d / 2
    gh = GROUND_H if h > 9 else min(GROUND_H, h * 0.45)
    # Upper floors: a whole number of them between the shop band and the parapet.
    floors = max(1, round((h - gh - 0.6) / FLOOR))
    top = gh + floors * ((h - gh - 0.6) / floors)
    _walls(ground, -hw, hw, -hd, hd, 0.0, gh, None, 1, ground=True)
    _walls(upper, -hw, hw, -hd, hd, gh, top, None, floors)
    # The cornice over the shops and the parapet round the roof.
    _box(trim, -hw - 0.25, hw + 0.25, gh - 0.1, gh + 0.3, -hd - 0.25, hd + 0.25)
    _box(trim, -hw - 0.1, hw + 0.1, top, h, -hd - 0.1, -hd + 0.25)
    _box(trim, -hw - 0.1, hw + 0.1, top, h, hd - 0.25, hd + 0.1)
    _box(trim, -hw - 0.1, -hw + 0.25, top, h, -hd + 0.25, hd - 0.25)
    _box(trim, hw - 0.25, hw + 0.1, top, h, -hd + 0.25, hd - 0.25)
    # The roof, a rooftop box or two (stair house, plant) and, on the tall ones, a setback tier and a mast.
    roof.quad([(-hw + 0.25, top + 0.02, hd - 0.25), (hw - 0.25, top + 0.02, hd - 0.25), (hw - 0.25, top + 0.02, -hd + 0.25),
               (-hw + 0.25, top + 0.02, -hd + 0.25)], [(-hw / 4, hd / 4), (hw / 4, hd / 4), (hw / 4, -hd / 4), (-hw / 4, -hd / 4)], (0, 1, 0))
    tall = h > 40
    roof_y = top
    if tall:
        tier_floors = max(2, round(floors * 0.18))
        tw, td = w * 0.62, d * 0.62
        tier_h = tier_floors * FLOOR
        _walls(upper, -tw / 2, tw / 2, -td / 2, td / 2, top, top + tier_h, None, tier_floors)
        _box(trim, -tw / 2 - 0.1, tw / 2 + 0.1, top + tier_h, top + tier_h + 0.5, -td / 2 - 0.1, td / 2 + 0.1)
        roof_y = top + tier_h + 0.5
        mast = 10 + 12 * rng.random()
        _box(trim, -0.15, 0.15, roof_y, roof_y + mast, -0.15, 0.15)
        rw, rd = tw, td
    else:
        rw, rd = w, d
    for _ in range(rng.randint(1, 3)):
        bw, bd = rng.uniform(1.5, min(5.0, rw * 0.35)), rng.uniform(1.5, min(5.0, rd * 0.35))
        bx = rng.uniform(-rw / 2 + bw / 2 + 0.8, rw / 2 - bw / 2 - 0.8)
        bz = rng.uniform(-rd / 2 + bd / 2 + 0.8, rd / 2 - bd / 2 - 0.8)
        bh = rng.uniform(1.0, 2.6)
        _box(trim, bx - bw / 2, bx + bw / 2, roof_y, roof_y + bh, bz - bd / 2, bz + bd / 2)
    return [p for p in (ground, upper, trim, roof) if p.idx]


def _material(name, tex_dir):
    """glTF material for one prim: PBR textures by file name, emissive for the lit windows."""
    m = {"name": name, "doubleSided": False, "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 1.0,
                                                                         "roughnessFactor": 1.0}}
    uris = []

    def tex(kind, ext="jpg"):
        uri = "Textures/%s_%s.%s" % (name, kind, ext)
        if uri not in uris:
            uris.append(uri)
        return {"index": uris.index(uri)}

    m["pbrMetallicRoughness"]["baseColorTexture"] = tex("color")
    m["pbrMetallicRoughness"]["metallicRoughnessTexture"] = tex("orm")
    m["occlusionTexture"] = tex("orm")
    m["normalTexture"] = tex("normal")
    if name in STYLES or name.startswith("shop_"):
        m["emissiveTexture"] = tex("emissive")
        m["emissiveFactor"] = [1.0, 1.0, 1.0]
        m["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": 0.5}}
    return m, uris


def building_glb(w, d, h, style, shop, seed):
    prims = build_geometry(w, d, h, style, shop, seed)
    blob = bytearray()
    views, accessors, primitives, materials, images, textures = [], [], [], [], [], []
    mat_index = {}
    for p in prims:
        pos = np.array(p.pos, np.float32)
        nrm = np.array(p.nrm, np.float32)
        uv = np.array(p.uv, np.float32)
        idx = np.array(p.idx, np.uint32)
        attrs = {}
        for key, arr, typ in (("POSITION", pos, "VEC3"), ("NORMAL", nrm, "VEC3"), ("TEXCOORD_0", uv, "VEC2")):
            raw = arr.tobytes()
            views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(raw), "target": 34962})
            blob += raw
            acc = {"bufferView": len(views) - 1, "componentType": 5126, "count": len(arr), "type": typ}
            if key == "POSITION":
                acc["min"], acc["max"] = pos.min(0).tolist(), pos.max(0).tolist()
            accessors.append(acc)
            attrs[key] = len(accessors) - 1
        raw = idx.tobytes()
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(raw), "target": 34963})
        blob += raw
        accessors.append({"bufferView": len(views) - 1, "componentType": 5125, "count": len(idx), "type": "SCALAR"})
        if p.material not in mat_index:
            m, uris = _material(p.material, OUT)
            base = len(images)
            for u in uris:
                images.append({"uri": u})
                textures.append({"sampler": 0, "source": len(images) - 1})
            for key in ("baseColorTexture", "metallicRoughnessTexture"):
                if key in m["pbrMetallicRoughness"]:
                    m["pbrMetallicRoughness"][key]["index"] += base
            for key in ("occlusionTexture", "normalTexture", "emissiveTexture"):
                if key in m:
                    m[key]["index"] += base
            mat_index[p.material] = len(materials)
            materials.append(m)
        primitives.append({"attributes": attrs, "indices": len(accessors) - 1, "material": mat_index[p.material]})
    doc = {"asset": {"version": "2.0", "generator": "mce 09_openworld gen_buildings.py"},
           "extensionsUsed": ["KHR_materials_emissive_strength"],
           "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "building"}], "meshes": [{"primitives": primitives}],
           "materials": materials, "images": images, "textures": textures,
           "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
           "buffers": [{"byteLength": len(blob)}], "bufferViews": views, "accessors": accessors}
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    blob += b"\0" * (-len(blob) % 4)
    total = 12 + 8 + len(js) + 8 + len(blob)
    return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(js), b"JSON") + js
            + struct.pack("<I4s", len(blob), b"BIN\0") + bytes(blob))


def texture_files():
    """(file name, srgb) of every texture the buildings use."""
    out = []
    for name in STYLE_NAMES + list(SHOPS) + ["trim", "roof"]:
        for kind in ("color", "normal", "orm", "emissive"):
            if kind == "emissive" and name in ("trim", "roof"):
                continue
            out.append(("%s_%s.jpg" % (name, kind), kind in ("color", "emissive")))
    return out


if __name__ == "__main__":
    write_textures()
