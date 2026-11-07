#!/usr/bin/env python3
"""Writes the kit3d_tour sun shafts scene (M-SUNSHAFTS 45e): assets/scenes/tour_shafts.mcscene.

    python3 samples/kit3d_tour/gen_shafts.py

Stdlib only and deterministic (no clock, no random numbers; every position is a fixed number), so every run writes the same bytes.
No new textures or models: it reuses the tour's terrain (guid ...21), sphere mesh (...41) and plain material (...47).

The scene: a low sun about 4 degrees above the horizon straight ahead of the camera (an Environment3D sun, the light travelling
+z and a little down, a warm colour, a little exp2 fog), with a row of tall trunks (stretched spheres) and a wall in two halves
with a gap between the camera and the sun. The sun disk shows through the gaps between the trunks and through the wall's gap; the
screen-space pass (SunShafts3D) turns every opening into a ray fanning out from the sun and veils the dark trunks and the wall.
`-r.sun_shafts 0` draws the same scene without the rays. 14 entities: Field, World, Camera, the wall's two halves and 9 trunks.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets", "scenes", "tour_shafts.mcscene")

TERRAIN = "guid:b0a30000-0000-4000-8000-000000000021"
SPHERE = "guid:b0a30000-0000-4000-8000-000000000041"
PLAIN = "guid:b0a30000-0000-4000-8000-000000000047"

CAMERA_Y = 1.6
CAMERA_Z = 10.0
# Trunks: x positions of a row 14 m ahead of the camera (z = -4), spaced unevenly so the openings differ in width.
TRUNKS = [-9.2, -6.9, -4.6, -2.3, 0.0, 2.3, 4.6, 6.9, 9.2]
TRUNK_Z = -4.0
BARK = (0.12, 0.09, 0.07, 1.0)
# The wall: two ellipsoid slabs 6 m ahead of the camera with a 3 m gap that the sun shows through.
WALL_Z = 4.0
WALL = (0.2, 0.19, 0.18, 1.0)


def ent(name, comps):
    return {"id": 0, "name": name, "components": comps}


def mesh(tint):
    return {"mesh": SPHERE, "material": PLAIN, "cast_shadows": 0, "tint": list(tint)}


def main():
    ents = [
        ent("Field", {"Terrain3D": {"terrain": TERRAIN}, "TerrainCollider3D": {"friction": 0.9}, "LocalTransform": {"position": [0, 0, 0]}}),
        ent("World", {
            "WorldSettings": {"gravity_3d": [0, -9.81, 0]},
            # The light travels +z and down: the sun stands at -z (in front of the camera), 4 degrees up.
            "Environment3D": {"sun_direction": [0.04, -0.07, 0.997], "sun_intensity": 2.0, "sun_shadows": 0,
                              "sun_color": [1.0, 0.85, 0.65], "ambient_sky": [0.3, 0.3, 0.36], "ambient_ground": [0.14, 0.12, 0.1],
                              "fog_density": 0.02, "fog_color": [0.7, 0.62, 0.55], "exposure": 1.0, "bloom": 1, "bloom_intensity": 0.1},
            "SunShafts3D": {"enabled": 1, "intensity": 3.0, "length": 1.0, "decay": 0.98}}),
        ent("Camera", {"Camera3DComponent": {"primary": 1, "fov_deg": 60},
                       "LocalTransform": {"position": [0, CAMERA_Y, CAMERA_Z], "rotation": [0.0349, 0, 0, 0.9994]}}),
        ent("Wall left", {"MeshRef": mesh(WALL), "LocalTransform": {"position": [-5.6, 0.9, WALL_Z], "scale": [8.0, 2.4, 0.3]}}),
        ent("Wall right", {"MeshRef": mesh(WALL), "LocalTransform": {"position": [5.6, 0.9, WALL_Z], "scale": [8.0, 2.4, 0.3]}}),
    ]
    for i, x in enumerate(TRUNKS):
        ents.append(ent("Trunk %d" % (i + 1), {"MeshRef": mesh(BARK), "LocalTransform": {"position": [x, 4.5, TRUNK_Z], "scale": [1.5, 9.0, 1.5]}}))
    for i, e in enumerate(ents):
        e["id"] = i + 1
    doc = {"format": "mce-scene", "version": 1, "guid": "b0a30000-0000-4000-8000-000000000017", "name": "tour_shafts", "entities": ents}
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
