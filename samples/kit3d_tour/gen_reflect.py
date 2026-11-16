#!/usr/bin/env python3
"""Writes the kit3d_tour screen-space reflections scene (M-REFLECT2 48f): assets/scenes/tour_reflect.mcscene and its assets.

    python3 samples/kit3d_tour/gen_reflect.py

Stdlib only and deterministic (no clock, no random numbers; the normal map is a sum of sines of the texel index), so every run writes
the same bytes. It reuses the tour's sphere mesh (guid ...41) and makes what it needs:

  models/box.glb                      a unit cube (flat normals, uv 0..1 on every face), guid ...50: the floor strips and the pillars
  textures/reflect_bumps.png          64 x 64 RGB tangent-space normal map (a tileable bump field), guid ...51, imported with srgb false
  materials/reflect_r00..r70.mcmat    the roughness ladder 0.0, 0.15, 0.3, 0.5, 0.7 (a grey metal), guids ...52..56
  materials/reflect_bumpy.mcmat       the same metal at roughness 0.3 with the normal map, guid ...57
  materials/reflect_pillar.mcmat      a rough, slightly emissive white: the things the floor reflects, guid ...58
  materials/reflect_ground.mcmat      the dark rough ground around the strips, guid ...59
  timelines/reflect_slide.mctimeline  moves the sphere to and fro over the mirror strip (loops), guid ...5a
  materials/reflect_ball.mcmat        the moving sphere's plain light material, guid ...5b

The scene: the far row is the ladder, five 1.6 m wide floor strips of roughness 0.0, 0.15, 0.3, 0.5 and 0.7 side by side, each with a
tall coloured pillar behind it, so the reflected pillar is a sharp line on the left and a soft smear on the right (r.ssr.blur). The
near row is a normal-mapped strip (the bump widening) and a mirror strip with a bright sphere sliding over it (r.ssr.motion: with
TAA on, which the High tier has, the reflection of the sphere stays under it instead of trailing). Environment3D has ssr on with
ssr_max_roughness 0.8 (the blur makes the rougher strips usable), ssao and bloom. 17 entities. TAA is the quality tier's: the capture
of the GPU test runs `-r.quality high`. `-r.ssr 0`, `-r.ssr.blur 0` and `-r.ssr.motion 0` draw the same scene without each part.
"""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_translucent as gt  # noqa: E402  (write_png, write_glb, mesh_gltf, write_sidecar, guid; no side effects on import)

ASSETS = os.path.join(HERE, "assets")
OUT = os.path.join(ASSETS, "scenes", "tour_reflect.mcscene")

SPHERE = "guid:" + gt.guid(0x41)
BOX = gt.guid(0x50)
BUMP_TEX = gt.guid(0x51)
LADDER = [(0.0, 0x52), (0.15, 0x53), (0.3, 0x54), (0.5, 0x55), (0.7, 0x56)]
BUMPY, PILLAR, GROUND, SLIDE, BALL = 0x57, 0x58, 0x59, 0x5A, 0x5B
TILES = 64
BUMPS = 3  # bumps across the map (it tiles)


def r(v):
    return round(v, 4) + 0.0


def box_glb():
    pos, nrm, uv, idx = [], [], [], []
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    for n, u, v in faces:
        base = len(pos)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            pos.append(tuple(0.5 * n[k] + 0.5 * su * u[k] + 0.5 * sv * v[k] for k in range(3)))
            nrm.append(n)
            uv.append(((su + 1) / 2, 1 - (sv + 1) / 2))
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    mat = {"name": "box", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0.8}}
    return gt.mesh_gltf("box", pos, nrm, uv, idx, mat, None)


def height(x, y):
    """A tileable bump field in [0, 1] at texel (x, y)."""
    a = 2.0 * math.pi * BUMPS / TILES
    return 0.5 + 0.25 * math.sin(a * x) * math.cos(a * y) + 0.15 * math.sin(2 * a * x + 1.0) + 0.1 * math.cos(2 * a * y + 2.0)


def bump_rows():
    rows = []
    strength = 6.0
    for y in range(TILES):
        row = []
        for x in range(TILES):
            dx = height((x + 1) % TILES, y) - height((x - 1) % TILES, y)
            dy = height(x, (y + 1) % TILES) - height(x, (y - 1) % TILES)
            n = (-dx * strength, dy * strength, 1.0)  # +y up in the image: tangent space with v running down
            ln = math.sqrt(sum(c * c for c in n))
            row.append(tuple(max(0, min(255, int(round((c / ln * 0.5 + 0.5) * 255)))) for c in n))
        rows.append(row)
    return rows


def mcmat(path, g, name, base, metallic, rough, normal=None, emissive=None):
    m = {"format": "mce-material", "version": 1, "guid": g, "name": name, "base_color": base, "metallic": metallic, "roughness": rough,
         "double_sided": False}
    if emissive:
        m["emissive"] = emissive
    if normal:
        m["textures"] = {"normal": "guid:" + normal}
    with open(path, "w") as f:
        json.dump(m, f, indent=2)
        f.write("\n")


def write_assets():
    for d in ("models", "textures", "materials", "timelines", "scenes"):
        os.makedirs(os.path.join(ASSETS, d), exist_ok=True)
    models, tex, mats = (os.path.join(ASSETS, d) for d in ("models", "textures", "materials"))
    g, b = box_glb()
    gt.write_glb(os.path.join(models, "box.glb"), g, b)
    gt.write_sidecar(os.path.join(models, "box.glb"), BOX, "gltf", 3)
    png = os.path.join(tex, "reflect_bumps.png")
    gt.write_png(png, TILES, TILES, bump_rows(), False)
    with open(png + ".mceasset", "w") as f:  # a normal map is data, not colour: no sRGB decode
        f.write('{\n "guid": "%s",\n "importer": "texture",\n "importer_version": 1,\n "settings": {"srgb": false}\n}\n' % BUMP_TEX)
    metal = [0.55, 0.57, 0.6, 1]
    for rough, n in LADDER:
        mcmat(os.path.join(mats, "reflect_r%02d.mcmat" % int(round(rough * 100))), gt.guid(n), "reflect_r%02d" % int(round(rough * 100)),
              metal, 0.9, rough)
    mcmat(os.path.join(mats, "reflect_bumpy.mcmat"), gt.guid(BUMPY), "reflect_bumpy", metal, 0.9, 0.3, normal=BUMP_TEX)
    mcmat(os.path.join(mats, "reflect_pillar.mcmat"), gt.guid(PILLAR), "reflect_pillar", [0.9, 0.9, 0.9, 1], 0.0, 0.8, emissive=[0.3, 0.3, 0.3])
    mcmat(os.path.join(mats, "reflect_ground.mcmat"), gt.guid(GROUND), "reflect_ground", [0.12, 0.12, 0.13, 1], 0.0, 1.0)
    mcmat(os.path.join(mats, "reflect_ball.mcmat"), gt.guid(BALL), "reflect_ball", [0.9, 0.9, 0.9, 1], 0.0, 0.6)
    tl = {"format": "mce-timeline", "version": 1, "guid": gt.guid(SLIDE), "name": "reflect_slide", "duration": 6,
          "tracks": [{"type": "transform", "target": 0, "keys": [
              {"t": 0, "position": [-1.6, 0.7, 2.0]}, {"t": 3, "position": [3.6, 0.7, 2.0]}, {"t": 6, "position": [-1.6, 0.7, 2.0]}]}]}
    with open(os.path.join(ASSETS, "timelines", "reflect_slide.mctimeline"), "w") as f:
        json.dump(tl, f, indent=2)
        f.write("\n")


def ent(name, comps):
    return {"id": 0, "name": name, "components": comps}


def slab(name, mat, x, y, z, sx, sy, sz, tint=None):
    mr = {"mesh": "guid:" + BOX, "material": "guid:" + gt.guid(mat), "cast_shadows": 0}
    if tint:
        mr["tint"] = tint
    return ent(name, {"MeshRef": mr, "LocalTransform": {"position": [x, y, z], "scale": [sx, sy, sz]}})


def main():
    write_assets()
    pitch = math.radians(-17.0)
    ents = [
        ent("World", {
            "WorldSettings": {"gravity_3d": [0, -9.81, 0]},
            "Environment3D": {"sun_direction": [-0.35, -0.55, -0.6], "sun_intensity": 2.2, "sun_shadows": 0,
                              "sun_color": [1.0, 0.94, 0.84], "ambient_sky": [0.45, 0.55, 0.75], "ambient_ground": [0.2, 0.19, 0.18],
                              "exposure": 1.0, "bloom": 1, "bloom_intensity": 0.2, "ssao": 1,
                              "ssr": 1, "ssr_max_distance": 14, "ssr_thickness": 0.4, "ssr_max_roughness": 0.8,
                              "ssr_roughness_fade": 0.2, "ssr_steps": 64, "ssr_intensity": 1.0}}),
        ent("Camera", {"Camera3DComponent": {"primary": 1, "fov_deg": 60},
                       "LocalTransform": {"position": [0.0, 2.7, 7.2],
                                          "rotation": [r(math.sin(pitch / 2)), 0, 0, r(math.cos(pitch / 2))]}}),
        slab("Ground", GROUND, 0, -0.2, -2, 40, 0.2, 30),
    ]
    tints = [[1.0, 0.95, 0.9, 1], [1.0, 0.35, 0.25, 1], [1.0, 0.8, 0.2, 1], [0.3, 0.9, 0.45, 1], [0.35, 0.55, 1.0, 1]]
    for k, (rough, n) in enumerate(LADDER):
        x = -3.2 + 1.6 * k
        ents.append(slab("Strip %.2f" % rough, n, x, -0.05, -2.0, 1.6, 0.1, 4.0))
    for k, t in enumerate(tints):
        ents.append(slab("Pillar %d" % (k + 1), PILLAR, -3.2 + 1.6 * k, 1.4, -4.4, 0.5, 2.8, 0.5, t))
    ents.append(slab("Bumpy strip", BUMPY, -3.2, -0.05, 2.0, 2.4, 0.1, 3.0))
    ents.append(slab("Mirror strip", LADDER[0][1], 1.0, -0.05, 2.0, 5.6, 0.1, 3.0))
    ents.append(ent("Sphere", {"MeshRef": {"mesh": SPHERE, "material": "guid:" + gt.guid(BALL), "cast_shadows": 0,
                                           "tint": [1.0, 0.55, 0.15, 1]},
                               "LocalTransform": {"position": [-1.6, 0.7, 2.0], "scale": [1.2, 1.2, 1.2]}}))
    sphere_id = len(ents)
    ents.append(ent("Mover", {"TimelinePlayer": {"timeline": "guid:" + gt.guid(SLIDE), "loop": True,
                                                "bindings": ["$%d" % sphere_id] + [None] * 7}}))
    for i, e in enumerate(ents):
        e["id"] = i + 1
    assert len(ents) == 17 and sphere_id == ents[-2]["id"]
    doc = {"format": "mce-scene", "version": 1, "guid": gt.guid(0x19), "name": "tour_reflect", "entities": ents}
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
