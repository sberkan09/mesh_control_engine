#!/usr/bin/env python3
"""Writes the kit3d_tour lens rain scene (M-LENSRAIN 42e): assets/scenes/tour_lensrain.mcscene.

    python3 samples/kit3d_tour/gen_lensrain.py

Stdlib only and deterministic (no clock, no random numbers; the positions are a fixed table), so every run writes the same bytes.
No new textures or models: it reuses the tour's terrain (guid ...21), sphere mesh (...41) and plain material (...47).

The scene: a dusk street of lit spheres in a row with coloured point lights behind them (warm, red, blue, green: small bright
blobs and high contrast, the picture a rain drop on the lens turns upside down), a Weather entity that is told to rain
(transition 2 s) and a LensRain3D (wet_rate 2, so the lens is wet in about 2 s of rain). Weather.current turns Rain after
the first tick; the kit's lens rain system raises LensRain3D.wet_q, Kit3DExtract feeds it to the renderer. 13 entities.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets", "scenes", "tour_lensrain.mcscene")

TERRAIN = "guid:b0a30000-0000-4000-8000-000000000021"
SPHERE = "guid:b0a30000-0000-4000-8000-000000000041"
PLAIN = "guid:b0a30000-0000-4000-8000-000000000047"

# (name, x, y, z) of the lit spheres, and (name, x, y, z, r, g, b, intensity) of the lights behind them.
SPHERES = [("Sphere A", -2.4, 0.5, 0.0), ("Sphere B", -0.8, 0.5, -1.0), ("Sphere C", 0.9, 0.5, 0.2), ("Sphere D", 2.5, 0.5, -0.8)]
LIGHTS = [
    ("Lamp warm", -3.0, 1.6, -4.0, 1.0, 0.7, 0.35, 6.0),
    ("Lamp red", -1.0, 1.0, -5.0, 1.0, 0.15, 0.1, 6.0),
    ("Lamp blue", 1.2, 1.4, -4.5, 0.2, 0.4, 1.0, 6.0),
    ("Lamp green", 3.2, 1.0, -5.0, 0.2, 1.0, 0.4, 6.0),
]


def ent(name, comps):
    return {"id": 0, "name": name, "components": comps}


def main():
    ents = [
        ent("Field", {"Terrain3D": {"terrain": TERRAIN}, "TerrainCollider3D": {"friction": 0.9}, "LocalTransform": {"position": [0, 0, 0]}}),
        ent("World", {
            "WorldSettings": {"gravity_3d": [0, -9.81, 0]},
            "Environment3D": {"sun_direction": [-0.3, -0.25, 0.9], "sun_intensity": 0.5, "sun_shadows": 1, "sun_color": [0.7, 0.75, 1.0],
                              "ambient_sky": [0.1, 0.12, 0.2], "ambient_ground": [0.05, 0.05, 0.06], "exposure": 1.0,
                              "bloom": 1, "bloom_intensity": 0.2}}),
        ent("Camera", {"Camera3DComponent": {"primary": 1, "fov_deg": 55},
                       "LocalTransform": {"position": [0, 1.4, 6.0], "rotation": [-0.0262, 0, 0, 0.9997]}}),
        ent("Weather", {"Weather": {"target": "Rain", "transition_seconds": 2}}),
        ent("Lens", {"LensRain3D": {"density": 1.3, "size": 1.2, "wet_rate": 2, "seed": 7}}),
    ]
    for name, x, y, z in SPHERES:
        ents.append(ent(name, {"MeshRef": {"mesh": SPHERE, "material": PLAIN, "cast_shadows": 1}, "LocalTransform": {"position": [x, y, z]}}))
    for name, x, y, z, r, g, b, inten in LIGHTS:
        ents.append(ent(name, {"PointLight3D": {"color": [r, g, b, 1], "intensity": inten, "radius": 9}, "LocalTransform": {"position": [x, y, z]}}))
    for i, e in enumerate(ents):
        e["id"] = i + 1
    doc = {"format": "mce-scene", "version": 1, "guid": "b0a30000-0000-4000-8000-000000000014", "name": "tour_lensrain", "entities": ents}
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
