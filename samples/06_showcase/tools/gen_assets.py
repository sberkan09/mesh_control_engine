#!/usr/bin/env python3
"""Generates samples/06_showcase/assets: the M-VISUAL demo's models and textures, procedurally.

    python3 samples/06_showcase/tools/gen_assets.py

Deterministic (no randomness, no clocks): re-running reproduces the committed bytes. Standard library
only; the glTF / PNG writers and mesh helpers are 04_cubes' (samples/04_cubes/tools/gen_assets.py).

  models/courtyard.glb  40 m tiled courtyard (base color + normal + ORM maps)
  models/pavilion.glb   a 7 x 3.2 x 6 m room: polished floor, ceiling, a red back wall, white side
                        walls, a door in the front wall and a window in the east wall (the GI interior)
  models/statue.glb     a chrome sphere on a stone plinth (reflections)
  models/lantern.glb    a glowing lantern box (the local lights' fixtures)
  models/brazier.glb    a dark iron bowl on legs (the particle source)
  models/crate.glb      a 0.8 m wooden crate
  models/grass.glb      a tuft of five tapered blades (the instanced foliage)
  textures/*.png        the courtyard tiles' maps
"""

import importlib.util
import math
import pathlib
import uuid

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[0] / "assets"
_spec = importlib.util.spec_from_file_location("cubes_gen", HERE.parents[1] / "04_cubes" / "tools" / "gen_assets.py")
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)
NS = uuid.UUID("6d63652d-3036-7368-6f77-636173653900")  # fixed namespace: stable GUIDs per path

Gltf, box, plane, uv_sphere, tapered_tube, r6 = base.Gltf, base.box, base.plane, base.uv_sphere, base.tapered_tube, base.r6


def pbr(g, name, color, metallic=0.0, rough=0.8, emissive=None, strength=None):
    m = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": color, "metallicFactor": metallic,
                                                 "roughnessFactor": rough}}
    if emissive:
        m["emissiveFactor"] = emissive
        if strength:
            m["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": strength}}
            g.doc["extensionsUsed"] = ["KHR_materials_emissive_strength"]
    return g.material(m)


def part(g, name, geo, mat, t, parent=None, root=False):
    return g.node({"name": name, "mesh": g.mesh(name, geo, mat), "translation": [r6(c) for c in t]}, root=root,
                  parent=parent)


def slab(g, name, mat, lo, hi, root):
    """An axis box between corners lo and hi."""
    h = [(hi[k] - lo[k]) * 0.5 for k in range(3)]
    c = [(hi[k] + lo[k]) * 0.5 for k in range(3)]
    part(g, name, box(h[0], h[1], h[2], uv_scale=max(h) * 2.0), mat, c, parent=root)


def build_models():
    models = {}

    g = Gltf()
    mat = g.material({"name": "tiles", "pbrMetallicRoughness": {
        "baseColorTexture": {"index": g.texture("../textures/tiles_albedo.png")},
        "metallicRoughnessTexture": {"index": g.texture("../textures/tiles_orm.png")},
        "metallicFactor": 1.0, "roughnessFactor": 1.0},
        "normalTexture": {"index": g.texture("../textures/tiles_normal.png")},
        "occlusionTexture": {"index": g.texture("../textures/tiles_orm.png")}})
    g.node({"name": "courtyard", "mesh": g.mesh("courtyard", plane(20.0, 10.0), mat)}, root=True)
    models["courtyard.glb"] = g

    # The pavilion: x -3.5..3.5, z -3..3, floor at 0, ceiling at 3.2; walls 0.2 thick outside the box.
    g = Gltf()
    floor = pbr(g, "polished_stone", [0.42, 0.40, 0.38, 1], 0.0, 0.12)
    ceiling = pbr(g, "ceiling", [0.85, 0.83, 0.8, 1], 0.0, 0.9)
    red = pbr(g, "red_plaster", [0.78, 0.12, 0.08, 1], 0.0, 0.85)
    white = pbr(g, "white_plaster", [0.88, 0.86, 0.82, 1], 0.0, 0.9)
    trim = pbr(g, "trim", [0.18, 0.16, 0.14, 1], 0.0, 0.6)
    root = g.node({"name": "pavilion"}, root=True)
    x0, x1, z0, z1, top, t = -3.5, 3.5, -3.0, 3.0, 3.2, 0.2
    slab(g, "floor", floor, (x0 - t, -0.1, z0 - t), (x1 + t, 0.0, z1 + t), root)
    slab(g, "ceiling", ceiling, (x0 - t, top, z0 - t), (x1 + t, top + 0.25, z1 + t), root)
    slab(g, "back_wall", red, (x0 - t, 0.0, z0 - t), (x1 + t, top, z0), root)
    slab(g, "west_wall", white, (x0 - t, 0.0, z0), (x0, top, z1 + t), root)
    # East wall with a 2 m x 1.4 m window, sill at 1 m, centred on z = -0.5.
    wz0, wz1, wy0, wy1 = -1.5, 0.5, 1.0, 2.4
    slab(g, "east_wall_a", white, (x1, 0.0, z0), (x1 + t, top, wz0), root)
    slab(g, "east_wall_b", white, (x1, 0.0, wz1), (x1 + t, top, z1 + t), root)
    slab(g, "east_wall_sill", white, (x1, 0.0, wz0), (x1 + t, wy0, wz1), root)
    slab(g, "east_wall_head", white, (x1, wy1, wz0), (x1 + t, top, wz1), root)
    # Front wall with a 2.4 m x 2.5 m door, centred.
    dx, dy = 1.2, 2.5
    slab(g, "front_wall_a", white, (x0, 0.0, z1), (-dx, top, z1 + t), root)
    slab(g, "front_wall_b", white, (dx, 0.0, z1), (x1, top, z1 + t), root)
    slab(g, "front_wall_head", white, (-dx, dy, z1), (dx, top, z1 + t), root)
    slab(g, "door_trim_l", trim, (-dx - 0.08, 0.0, z1 + t), (-dx, dy + 0.08, z1 + t + 0.04), root)
    slab(g, "door_trim_r", trim, (dx, 0.0, z1 + t), (dx + 0.08, dy + 0.08, z1 + t + 0.04), root)
    slab(g, "door_trim_t", trim, (-dx - 0.08, dy, z1 + t), (dx + 0.08, dy + 0.08, z1 + t + 0.04), root)
    models["pavilion.glb"] = g

    g = Gltf()
    chrome = pbr(g, "chrome", [0.95, 0.95, 0.97, 1], 1.0, 0.08)
    stone = pbr(g, "plinth", [0.55, 0.53, 0.5, 1], 0.0, 0.7)
    root = g.node({"name": "statue"}, root=True)
    part(g, "plinth", box(0.35, 0.45, 0.35), stone, (0, 0.45, 0), parent=root)
    part(g, "sphere", uv_sphere(0.38, 40, 20), chrome, (0, 1.28, 0), parent=root)
    models["statue.glb"] = g

    g = Gltf()
    iron = pbr(g, "iron", [0.12, 0.12, 0.13, 1], 1.0, 0.45)
    glow = pbr(g, "glow", [1, 0.85, 0.6, 1], 0.0, 0.5, [1.0, 0.72, 0.4], 8.0)
    root = g.node({"name": "lantern"}, root=True)
    part(g, "cap", box(0.14, 0.03, 0.14), iron, (0, 0.17, 0), parent=root)
    part(g, "glass", box(0.1, 0.14, 0.1), glow, (0, 0.0, 0), parent=root)
    part(g, "base", box(0.12, 0.02, 0.12), iron, (0, -0.16, 0), parent=root)
    models["lantern.glb"] = g

    g = Gltf()
    iron = pbr(g, "iron", [0.1, 0.09, 0.09, 1], 1.0, 0.55)
    coal = pbr(g, "embers", [0.2, 0.05, 0.02, 1], 0.0, 0.9, [1.0, 0.35, 0.08], 6.0)
    root = g.node({"name": "brazier"}, root=True)
    for i, (x, z) in enumerate(((-0.25, -0.25), (0.25, -0.25), (-0.25, 0.25), (0.25, 0.25))):
        part(g, "leg%d" % i, box(0.03, 0.35, 0.03), iron, (x, 0.35, z), parent=root)
    part(g, "bowl", box(0.4, 0.08, 0.4), iron, (0, 0.74, 0), parent=root)
    part(g, "embers", box(0.33, 0.03, 0.33), coal, (0, 0.83, 0), parent=root)
    models["brazier.glb"] = g

    g = Gltf()
    wood = pbr(g, "wood", [0.52, 0.34, 0.18, 1], 0.0, 0.75)
    plank = pbr(g, "plank", [0.36, 0.22, 0.11, 1], 0.0, 0.8)
    root = g.node({"name": "crate"}, root=True)
    part(g, "body", box(0.4, 0.4, 0.4), wood, (0, 0.4, 0), parent=root)
    part(g, "band_a", box(0.42, 0.05, 0.42), plank, (0, 0.15, 0), parent=root)
    part(g, "band_b", box(0.42, 0.05, 0.42), plank, (0, 0.65, 0), parent=root)
    models["crate.glb"] = g

    # Grass: five tapered three-sided blades leaning out from the origin (one mesh, so one batch).
    g = Gltf()
    green = pbr(g, "grass", [0.22, 0.42, 0.12, 1], 0.0, 0.85)
    pos, nrm, uv, idx = [], [], [], []
    for k in range(5):
        a = 2 * math.pi * k / 5 + 0.4
        bp, bn, bu, bi = tapered_tube(0.025, 0.002, 0.45 + 0.08 * (k % 3), 3, 3)
        lean = 0.35
        off = len(pos) // 3
        for v in range(len(bp) // 3):
            x, y, z = bp[3 * v], bp[3 * v + 1], bp[3 * v + 2]
            f = y / 0.6
            x += math.cos(a) * (0.06 + lean * f * f * 0.3)
            z += math.sin(a) * (0.06 + lean * f * f * 0.3)
            pos += [r6(x), r6(y), r6(z)]
        nrm += bn
        uv += bu
        idx += [i + off for i in bi]
    g.node({"name": "grass", "mesh": g.mesh("grass", (pos, nrm, uv, idx), green)}, root=True)
    models["grass.glb"] = g
    return models


def sidecar(path, importer, version, settings):
    rel = path.relative_to(ROOT).as_posix()
    doc = {"guid": str(uuid.uuid5(NS, rel)), "importer": importer, "importer_version": version, "settings": settings}
    pathlib.Path(str(path) + ".mceasset").write_text(base.json.dumps(doc, indent=2) + "\n")


def main():
    textures = ROOT / "textures"
    a, n, o = base.tiles_textures()
    for name, px, srgb in (("tiles_albedo", a, True), ("tiles_normal", n, False), ("tiles_orm", o, False)):
        base.write_png(textures / (name + ".png"), 256, 256, px)
        sidecar(textures / (name + ".png"), "texture", 1, {} if srgb else {"srgb": False})
    for name, g in build_models().items():
        g.doc["asset"]["generator"] = "mce 06_showcase gen_assets.py"
        g.write(ROOT / "models" / name)
        sidecar(ROOT / "models" / name, "gltf", 2, {})
    print("06_showcase assets written to", ROOT)


if __name__ == "__main__":
    main()
