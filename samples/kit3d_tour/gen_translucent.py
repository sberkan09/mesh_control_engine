#!/usr/bin/env python3
"""Writes the assets of the kit3d_tour translucency scene (M-TRANSLUCENT 41g) into assets/models, assets/textures and assets/materials.

    python3 samples/kit3d_tour/gen_translucent.py

Stdlib only and deterministic (a fixed seed, no clock, no platform maths beyond sin/cos/sqrt), so every run writes the same bytes and
no binary is hand-made. The files and their sidecars are original to this repo (CC0-1.0); there is nothing to credit.

  models/leaf.glb        a vertical, double-sided leaf card (1 x 1.2 m, origin at its foot), alpha-masked by textures/leaf.png, with
                         KHR_materials_diffuse_transmission (diffuseTransmissionFactor 0.85): the glTF authoring path of translucency.
  models/sphere.glb      a UV sphere of radius 0.5 m whose material carries extras.mce_wrap = 0.6 (the glTF has no wrap extension, so
                         Blender's custom property is the path): a soft-terminator "skin" sphere. The paper lantern shade uses it too.
  materials/paper.mcmat  warm paper, double sided, translucency 0.6: the .mcmat authoring path (a screen and the lantern shade; a .mcmat carries its guid inside, no sidecar).
  materials/wax.mcmat    pale wax, translucency 0.45 and wrap 0.6.
  materials/plain.mcmat  the same pale colour as the wax with neither, to put beside it.
  textures/leaf.png      128 x 128 RGBA, a pointed leaf with a midrib and side veins; outside the leaf alpha is 0 (the mask).
  textures/paper.png     128 x 128 RGB, warm fibre noise.

Guids are fixed (b0a30000-0000-4000-8000-0000000000{40..47}); the scene scenes/tour_translucent.mcscene refers to them.
"""
import json
import math
import os
import random
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
N = 128
AA = 4


def guid(n):
    return "b0a30000-0000-4000-8000-0000000000%02x" % n


def write_png(path, width, height, rows, alpha):
    """rows: height lists of width byte tuples ((r, g, b) or (r, g, b, a)). 8-bit, one IDAT, filter 0."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for px in row:
            raw.extend(px)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def write_sidecar(path, g, importer, version):
    settings = "{}"
    with open(path + ".mceasset", "w") as f:
        f.write('{\n "guid": "%s",\n "importer": "%s",\n "importer_version": %d,\n "settings": %s\n}\n' % (g, importer, version, settings))


def clamp01(v):
    return 0.0 if v < 0.0 else 1.0 if v > 1.0 else v


def leaf_sample(u, v):
    """(coverage 0/1, r, g, b in 0..1) of the leaf texture at (u, v), v down the image: tip at the top, stalk at the bottom."""
    t = (v - 0.03) / 0.94           # 0 at the tip, 1 at the stalk end
    if t < 0.0 or t > 1.0:
        return 0.0, 0.0, 0.0, 0.0
    # the widest part sits at 40 percent from the tip; a pointed tip and a rounded base
    half = 0.44 * (math.sin(math.pi * t ** 0.8)) ** 0.9
    du = abs(u - 0.5)
    inside = du <= half
    rib = du < 0.012                # midrib
    side = 0.0
    if inside and half > 0.0:
        # side veins: diagonal lines leaving the rib toward the tip
        k = (du / 0.44 + t * 3.2) % 0.34
        side = 1.0 if k < 0.03 else 0.0
    edge = clamp01((half - du) / 0.12) if inside else 0.0
    shade = 0.78 + 0.22 * edge
    r, g, b = 0.16 * shade, 0.42 * shade, 0.08 * shade
    if rib or side:
        r, g, b = r * 1.35 + 0.05, g * 1.25 + 0.05, b * 1.3
    if inside:
        return 1.0, r, g, b
    return 0.0, 0.16, 0.42, 0.08


def leaf_png():
    rows = []
    for y in range(N):
        row = []
        for x in range(N):
            cov = 0.0
            col = [0.0, 0.0, 0.0]
            for j in range(AA):
                for i in range(AA):
                    c, r, g, b = leaf_sample((x + (i + 0.5) / AA) / N, (y + (j + 0.5) / AA) / N)
                    cov += c
                    col[0] += r * c
                    col[1] += g * c
                    col[2] += b * c
            n = AA * AA
            if cov > 0.0:
                col = [v / cov for v in col]
            else:
                col = [0.16, 0.42, 0.08]    # outside: the leaf's green, so the edge pixels do not bleed black
            row.append((int(col[0] * 255 + 0.5), int(col[1] * 255 + 0.5), int(col[2] * 255 + 0.5), int(cov / n * 255 + 0.5)))
        rows.append(row)
    return rows


def paper_png():
    rng = random.Random(20261105)
    fibres = []
    for _ in range(220):
        fibres.append((rng.random(), rng.random(), rng.uniform(0.04, 0.16), rng.uniform(0.0, math.pi), rng.uniform(-0.05, 0.04)))
    rows = []
    for y in range(N):
        row = []
        for x in range(N):
            u, v = (x + 0.5) / N, (y + 0.5) / N
            d = 0.0
            for fx, fy, ln, ang, amp in fibres:
                du = (u - fx + 0.5) % 1.0 - 0.5
                dv = (v - fy + 0.5) % 1.0 - 0.5
                a = du * math.cos(ang) + dv * math.sin(ang)
                b = -du * math.sin(ang) + dv * math.cos(ang)
                if abs(a) < ln and abs(b) < 0.004:
                    d += amp
            row.append(tuple(int(clamp01(c + d) * 255 + 0.5) for c in (0.93, 0.87, 0.74)))
        rows.append(row)
    return rows


def pad4(b, fill=b"\x00"):
    return b + fill * (-len(b) % 4)


def write_glb(path, gltf, binary):
    js = pad4(json.dumps(gltf, separators=(",", ":"), sort_keys=True).encode(), b" ")
    binary = pad4(binary)
    total = 12 + 8 + len(js) + 8 + len(binary)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<I4s", len(js), b"JSON") + js)
        f.write(struct.pack("<I4s", len(binary), b"BIN\x00") + binary)


def mesh_gltf(name, pos, nrm, uv, idx, material, extensions_used, images=None):
    """One mesh, one material. pos/nrm/uv: lists of tuples; idx: list of ints (uint16)."""
    pb = b"".join(struct.pack("<3f", *p) for p in pos)
    nb = b"".join(struct.pack("<3f", *p) for p in nrm)
    ub = b"".join(struct.pack("<2f", *p) for p in uv)
    ib = pad4(b"".join(struct.pack("<H", i) for i in idx))
    views, off = [], 0
    for blob, target in ((pb, 34962), (nb, 34962), (ub, 34962), (ib, 34963)):
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(blob), "target": target})
        off += len(blob)
    binary = pb + nb + ub + ib
    lo = [min(p[k] for p in pos) for k in range(3)]
    hi = [max(p[k] for p in pos) for k in range(3)]
    g = {
        "asset": {"version": "2.0", "generator": "mce gen_translucent.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [material],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": views,
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": lo, "max": hi},
            {"bufferView": 1, "componentType": 5126, "count": len(pos), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(pos), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
        ],
    }
    if extensions_used:
        g["extensionsUsed"] = extensions_used
    if images:
        g["images"] = images
        g["textures"] = [{"source": 0}]
    return g, binary


def leaf_glb():
    w, h = 0.5, 1.2
    pos = [(-w, 0, 0), (w, 0, 0), (w, h, 0), (-w, h, 0)]
    nrm = [(0, 0, 1)] * 4
    uv = [(0, 1), (1, 1), (1, 0), (0, 0)]
    idx = [0, 1, 2, 0, 2, 3]
    mat = {
        "name": "leaf",
        "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "baseColorTexture": {"index": 0}, "metallicFactor": 0, "roughnessFactor": 0.65},
        "alphaMode": "MASK",
        "alphaCutoff": 0.5,
        "doubleSided": True,
        "extensions": {"KHR_materials_diffuse_transmission": {"diffuseTransmissionFactor": 0.85}},
    }
    return mesh_gltf("leaf", pos, nrm, uv, idx, mat, ["KHR_materials_diffuse_transmission"], [{"uri": "../textures/leaf.png"}])


def sphere_glb(seg=24, rings=16, r=0.5):
    pos, nrm, uv = [], [], []
    for j in range(rings + 1):
        phi = math.pi * j / rings
        for i in range(seg + 1):
            th = 2.0 * math.pi * i / seg
            n = (math.sin(phi) * math.cos(th), math.cos(phi), math.sin(phi) * math.sin(th))
            nrm.append(n)
            pos.append((n[0] * r, n[1] * r, n[2] * r))
            uv.append((i / seg, j / rings))
    idx = []
    for j in range(rings):
        for i in range(seg):
            a = j * (seg + 1) + i
            b = a + seg + 1
            idx += [a, a + 1, b, a + 1, b + 1, b]
    mat = {
        "name": "skin",
        "pbrMetallicRoughness": {"baseColorFactor": [0.86, 0.62, 0.5, 1], "metallicFactor": 0, "roughnessFactor": 0.55},
        "extras": {"mce_wrap": 0.6},
    }
    return mesh_gltf("sphere", pos, nrm, uv, idx, mat, None)


def mcmat(path, g, name, base, rough, double_sided, translucency, wrap, texture=None):
    m = {"format": "mce-material", "version": 1, "guid": g, "name": name, "base_color": base, "metallic": 0.0, "roughness": rough,
         "double_sided": double_sided}
    if translucency:
        m["translucency"] = translucency
    if wrap:
        m["wrap"] = wrap
    if texture:
        m["textures"] = {"base_color": "guid:" + texture}
    with open(path, "w") as f:
        json.dump(m, f, indent=2)
        f.write("\n")


def main():
    for d in ("models", "textures", "materials"):
        os.makedirs(os.path.join(ASSETS, d), exist_ok=True)
    tex = os.path.join(ASSETS, "textures")
    write_png(os.path.join(tex, "leaf.png"), N, N, leaf_png(), True)
    write_sidecar(os.path.join(tex, "leaf.png"), guid(0x43), "texture", 1)
    write_png(os.path.join(tex, "paper.png"), N, N, paper_png(), False)
    write_sidecar(os.path.join(tex, "paper.png"), guid(0x44), "texture", 1)

    models = os.path.join(ASSETS, "models")
    g, b = leaf_glb()
    write_glb(os.path.join(models, "leaf.glb"), g, b)
    write_sidecar(os.path.join(models, "leaf.glb"), guid(0x40), "gltf", 3)
    g, b = sphere_glb()
    write_glb(os.path.join(models, "sphere.glb"), g, b)
    write_sidecar(os.path.join(models, "sphere.glb"), guid(0x41), "gltf", 3)

    mats = os.path.join(ASSETS, "materials")
    mcmat(os.path.join(mats, "paper.mcmat"), guid(0x45), "paper", [0.8, 0.76, 0.66, 1], 0.9, True, 0.6, 0.0, guid(0x44))
    mcmat(os.path.join(mats, "wax.mcmat"), guid(0x46), "wax", [0.93, 0.84, 0.6, 1], 0.45, False, 0.45, 0.6)
    mcmat(os.path.join(mats, "plain.mcmat"), guid(0x47), "plain", [0.93, 0.84, 0.6, 1], 0.45, False, 0.0, 0.0)
    for n in ("textures/leaf.png", "textures/paper.png", "models/leaf.glb", "models/sphere.glb", "materials/paper.mcmat",
              "materials/wax.mcmat", "materials/plain.mcmat"):
        print("wrote", n)


if __name__ == "__main__":
    main()
