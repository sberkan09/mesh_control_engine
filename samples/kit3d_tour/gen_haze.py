#!/usr/bin/env python3
"""Writes the kit3d_tour heat haze scene (M-HEATHAZE 43e): assets/scenes/tour_haze.mcscene.

    python3 samples/kit3d_tour/gen_haze.py

Stdlib only and deterministic (no clock, no random numbers; the positions are computed from fixed loops), so every run writes the
same bytes. No new textures or models: it reuses the tour's terrain (guid ...21), sphere mesh (...41) and plain material (...47).

The scene: a campfire at the origin (a stack of three glowing orange spheres, the tour's fire particle emitter and a point light
with a LightFlicker3D, the same numbers as the tour's "Campfire light") under a HeatHaze3D (fire_follow 1, offset_y 1.2) that sits
on the light entity, so the strength follows the flicker. Behind it a checker backdrop (there is no checker texture: a 7 x 3
grid of spheres tinted in two alternating colours, so the displacement of straight rows is easy to see), and in front of the
haze a thin pole (a stretched sphere) that must stay sharp: the depth mask keeps the pole out of the displacement.
The camera looks at the fire from 6 m. 5 + 3 stack + 21 checker + 1 pole = 30 entities.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets", "scenes", "tour_haze.mcscene")

TERRAIN = "guid:b0a30000-0000-4000-8000-000000000021"
SPHERE = "guid:b0a30000-0000-4000-8000-000000000041"
PLAIN = "guid:b0a30000-0000-4000-8000-000000000047"

# (name, y, scale, r, g, b) of the glowing stack; emissive_tint > 1 feeds bloom where the material glows.
STACK = [("Ember base", 0.2, 0.55, 1.0, 0.35, 0.08), ("Ember mid", 0.5, 0.4, 1.0, 0.5, 0.1), ("Ember top", 0.75, 0.25, 1.0, 0.75, 0.25)]
COLS, ROWS = 7, 3
TINT_A = (0.9, 0.85, 0.75, 1.0)
TINT_B = (0.2, 0.3, 0.7, 1.0)


def ent(name, comps):
    return {"id": 0, "name": name, "components": comps}


def mesh(tint, emissive=None):
    m = {"mesh": SPHERE, "material": PLAIN, "cast_shadows": 0, "tint": list(tint)}
    if emissive is not None:
        m["emissive_tint"] = list(emissive)
    return m


def main():
    ents = [
        ent("Field", {"Terrain3D": {"terrain": TERRAIN}, "TerrainCollider3D": {"friction": 0.9}, "LocalTransform": {"position": [0, 0, 0]}}),
        ent("World", {
            "WorldSettings": {"gravity_3d": [0, -9.81, 0]},
            "Environment3D": {"sun_direction": [-0.3, -0.25, 0.9], "sun_intensity": 0.8, "sun_shadows": 0, "sun_color": [0.8, 0.8, 1.0],
                              "ambient_sky": [0.25, 0.28, 0.4], "ambient_ground": [0.12, 0.12, 0.14], "exposure": 1.0,
                              "bloom": 1, "bloom_intensity": 0.2}}),
        ent("Camera", {"Camera3DComponent": {"primary": 1, "fov_deg": 55},
                       "LocalTransform": {"position": [0, 1.4, 6.0], "rotation": [-0.0262, 0, 0, 0.9997]}}),
        ent("Campfire", {
            "PointLight3D": {"color": [1.0, 0.6, 0.25, 1], "offset": [0, 0.6, 0], "intensity": 4, "radius": 8},
            "LightFlicker3D": {"depth": 0.4, "rate_hz": 9, "seed": 3},
            "HeatHaze3D": {"radius": 1.2, "strength": 0.9, "rise": 0.8, "scale": 1.0, "softness": 0.5, "offset_y": 1.2, "fire_follow": 1},
            "LocalTransform": {"position": [0, 0.0, 0]}}),
        ent("Fire", {"ParticleEmitter3D": {"rate": 40, "collide": 1}, "LocalTransform": {"position": [0, 0.2, 0]}}),
    ]
    for name, y, s, r, g, b in STACK:
        ents.append(ent(name, {"MeshRef": mesh((r, g, b, 1.0), (6.0, 6.0, 6.0)),
                               "LocalTransform": {"position": [0, y, 0], "scale": [s, s, s]}}))
    for row in range(ROWS):
        for col in range(COLS):
            tint = TINT_A if (row + col) % 2 == 0 else TINT_B
            x = (col - (COLS - 1) / 2) * 1.0
            y = 0.5 + row * 1.0
            ents.append(ent("Check %d-%d" % (row, col), {"MeshRef": mesh(tint),
                                                       "LocalTransform": {"position": [x, y, -4.0], "scale": [0.95, 0.95, 0.95]}}))
    ents.append(ent("Pole", {"MeshRef": mesh((0.1, 0.1, 0.1, 1.0)),
                             "LocalTransform": {"position": [0.5, 1.4, 2.5], "scale": [0.06, 2.8, 0.06]}}))
    for i, e in enumerate(ents):
        e["id"] = i + 1
    doc = {"format": "mce-scene", "version": 1, "guid": "b0a30000-0000-4000-8000-000000000015", "name": "tour_haze", "entities": ents}
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
