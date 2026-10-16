#!/usr/bin/env python3
"""Writes Solstice Reach's island as a terrain (M-TERRAIN 6): assets/terrain/island.mcterrain and the
four layer materials it is painted with (assets/materials/terrain_*.mcmat).

The heights are tools/island.py's height field (the one module/terrain.h builds the collider from),
sampled every 0.78 m over the 200 m square the old 16 mesh chunks covered. The splat map paints each
sample with the rule the chunks used for their triangles: sand below 1.3 m, rock where the ground
leans past 37 degrees, dry grass above 34 m and in the noise's dry patches, grass elsewhere.

    python3 projects/SolsticeReach/tools/gen_terrain.py
"""

import base64
import json
import math
import pathlib
import struct
import sys
import uuid

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import island  # noqa: E402

ROOT = HERE.parent / "assets"
NS = uuid.UUID("6d63652d-736f-6c73-7469-636572656163")  # gen_assets.py's namespace: stable GUIDs per path
RESOLUTION = 257
HEIGHT_RANGE = (-8.0, 52.0)
LAYERS = ("sand", "grass", "dry_grass", "rock")


def guid(rel):
    return str(uuid.uuid5(NS, rel))


def write_material(name):
    rel = "materials/terrain_%s.mcmat" % name
    tex = lambda kind: "guid:" + guid("textures/%s_%s.png" % (name, kind))  # noqa: E731
    doc = {"format": "mce-material", "version": 1, "guid": guid(rel), "name": "terrain " + name,
           "base_color": [1, 1, 1, 1], "metallic": 0, "roughness": 1,
           "textures": {"base_color": tex("albedo"), "normal": tex("normal"), "metallic_roughness": tex("orm")}}
    path = ROOT / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=2) + "\n")
    return "guid:" + doc["guid"]


def main():
    for name in LAYERS:
        if not (ROOT / "textures" / ("%s_albedo.png" % name)).exists():
            sys.exit("gen_terrain: run gen_assets.py first (textures/%s_albedo.png is missing)" % name)
    materials = [write_material(name) for name in LAYERS]

    half, n = island.HALF, RESOLUTION
    step = 2.0 * half / (n - 1)
    xs = [-half + step * i for i in range(n)]
    hs = [[island.height(x, z) for x in xs] for z in xs]
    lo, hi = HEIGHT_RANGE
    heights = bytearray()
    splat = bytearray()
    counts = [0] * len(LAYERS)
    for j in range(n):
        for i in range(n):
            h = hs[j][i]
            heights += struct.pack("<H", round((min(max(h, lo), hi) - lo) / (hi - lo) * 65535.0))
            hl, hr = hs[j][max(i - 1, 0)], hs[j][min(i + 1, n - 1)]
            hd, hu = hs[max(j - 1, 0)][i], hs[min(j + 1, n - 1)][i]
            nx, nz = -(hr - hl) / (2.0 * step), -(hu - hd) / (2.0 * step)
            ny = 1.0 / math.sqrt(nx * nx + 1.0 + nz * nz)
            if h < 1.3:
                layer = 0
            elif ny < 0.80:
                layer = 3
            elif h > 34.0 or island.vnoise(xs[i] * 0.03 + 50, xs[j] * 0.03 + 50, 77) > 0.68:
                layer = 2
            else:
                layer = 1
            counts[layer] += 1
            w = [0] * len(LAYERS)
            w[layer] = 255
            splat += bytes(w)

    rel = "terrain/island.mcterrain"
    doc = {"format": "mce-terrain", "version": 1, "guid": guid(rel), "name": "island",
           "resolution": n, "chunk": 32, "size": [2 * half, 2 * half], "height_range": list(HEIGHT_RANGE),
           "layers": [{"material": materials[0], "tile": 4},
                      {"material": materials[1], "tile": 4},
                      {"material": materials[2], "tile": 4},
                      {"material": materials[3], "tile": 4, "triplanar": True}],
           "splat_resolution": n,
           "heights": base64.b64encode(bytes(heights)).decode("ascii"),
           "splat": base64.b64encode(bytes(splat)).decode("ascii")}
    path = ROOT / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=2) + "\n")
    total = float(n * n)
    print("gen_terrain: %s, %d samples; %s" % (rel, n * n, ", ".join(
        "%s %.0f %%" % (name, 100.0 * c / total) for name, c in zip(LAYERS, counts))))


if __name__ == "__main__":
    main()
