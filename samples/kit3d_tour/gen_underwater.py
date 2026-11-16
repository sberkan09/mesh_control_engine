#!/usr/bin/env python3
"""Writes the kit3d_tour underwater scene (M-UNDERWATER 44e): assets/scenes/tour_underwater.mcscene.

    python3 samples/kit3d_tour/gen_underwater.py

Stdlib only and deterministic (no clock, no random numbers; every position is a fixed number), so every run writes the same bytes.
No new textures or models: it reuses the tour's terrain (guid ...21), sphere mesh (...41) and plain material (...47).

The scene: a calm sea (a Water at level 0, amplitude 0.2, the default colours) over a seabed that is the tour's flat terrain moved
6 m down; the camera hangs 2 m below the surface and looks along -z, tilted 8 degrees up so the lid (the surface seen from below,
with its bright Snell window) shows in the top of the frame. Three equal-coloured white spheres stand at about 3, 8 and 20 m from the
camera (growing with distance so each stays readable): the near one keeps its colour, the middle one loses its red, the far one is
almost the water's scatter colour. A stretched sphere (a pillar) rises from the seabed through the surface, where the pass's lid
and the plain water plane meet it. 8 entities: Seabed, World, Camera, Sea, Pillar and the 3 spheres.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets", "scenes", "tour_underwater.mcscene")

TERRAIN = "guid:b0a30000-0000-4000-8000-000000000021"
SPHERE = "guid:b0a30000-0000-4000-8000-000000000041"
PLAIN = "guid:b0a30000-0000-4000-8000-000000000047"

CAMERA_Z = 10.0
# (name, distance from the camera in metres, x, centre y, scale): scale grows with distance so the far sphere is still a few pixels wide.
SPHERES = [("Near sphere", 3.0, -1.4, -2.4, 1.0), ("Middle sphere", 8.0, 0.4, -3.0, 1.8), ("Far sphere", 20.0, 6.0, -3.4, 3.2)]
WHITE = (0.95, 0.95, 0.95, 1.0)


def ent(name, comps):
    return {"id": 0, "name": name, "components": comps}


def mesh(tint):
    return {"mesh": SPHERE, "material": PLAIN, "cast_shadows": 0, "tint": list(tint)}


def main():
    ents = [
        # The tour's flat terrain (fill 0) moved down: the seabed is at y = -6.
        ent("Seabed", {"Terrain3D": {"terrain": TERRAIN}, "LocalTransform": {"position": [0, -6.0, 0]}}),
        ent("World", {
            "WorldSettings": {"gravity_3d": [0, -9.81, 0]},
            "Environment3D": {"sun_direction": [-0.3, -0.8, -0.5], "sun_intensity": 1.2, "sun_shadows": 0,
                              "ambient_sky": [0.5, 0.55, 0.65], "ambient_ground": [0.25, 0.25, 0.3], "exposure": 1.0,
                              "bloom": 1, "bloom_intensity": 0.15}}),
        ent("Camera", {"Camera3DComponent": {"primary": 1, "fov_deg": 60},
                       "LocalTransform": {"position": [0, -2.0, CAMERA_Z], "rotation": [0.0698, 0, 0, 0.9976]}}),
        ent("Sea", {"Water": {"level_y": 0, "amplitude": 0.2}, "LocalTransform": {"position": [0, 0, 0]}}),
        ent("Pillar", {"MeshRef": mesh((0.55, 0.5, 0.45, 1.0)),
                       "LocalTransform": {"position": [-4.0, -1.0, CAMERA_Z - 7.0], "scale": [0.5, 10.0, 0.5]}}),
    ]
    for name, dist, x, y, s in SPHERES:
        ents.append(ent(name, {"MeshRef": mesh(WHITE), "LocalTransform": {"position": [x, y, CAMERA_Z - dist], "scale": [s, s, s]}}))
    for i, e in enumerate(ents):
        e["id"] = i + 1
    doc = {"format": "mce-scene", "version": 1, "guid": "b0a30000-0000-4000-8000-000000000016", "name": "tour_underwater", "entities": ents}
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
