#!/usr/bin/env python3
"""Writes the kit3d_tour cloud shadow scene (M-CLOUDSHADOW 51e): assets/scenes/tour_cloudshadow.mcscene.

    python3 samples/kit3d_tour/gen_cloudshadow.py

Stdlib only and deterministic (no clock, no random numbers; every position is a fixed number), so every run writes the same bytes.
No new textures or models: it reuses the tour's terrain (guid ...21), sphere mesh (...41) and plain material (...47).

The scene: a high sun (about 55 degrees) over the tour's flat terrain, the sky layer on (a Weather in clear weather and a CloudLayer
with half the sky covered, 100 m noise cells, a fast drift so the patches visibly slide), and a CloudShadow3D on the world entity. The
camera stands 30 m up and looks a little down so the horizon and the sky with its clouds show over the ground; a row of squat
"buildings" (stretched spheres) and a few trees stand on the ground so the patches of shade cross them too. The shade is a post pass:
only ground and objects darken, never the sky. `-r.cloud_shadows 0` draws the same scene without it. 14 entities: Field, World,
Camera and 11 stand-ins (5 buildings, 6 trees).
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets", "scenes", "tour_cloudshadow.mcscene")

TERRAIN = "guid:b0a30000-0000-4000-8000-000000000021"
SPHERE = "guid:b0a30000-0000-4000-8000-000000000041"
PLAIN = "guid:b0a30000-0000-4000-8000-000000000047"

CAMERA_Y = 30.0
CAMERA_Z = 45.0
# (x, z, width, height): squat buildings in a loose row 20..60 m ahead of the camera.
BUILDINGS = [(-24.0, -10.0, 9.0, 6.0), (-8.0, -26.0, 12.0, 9.0), (10.0, -14.0, 8.0, 5.0), (26.0, -30.0, 14.0, 11.0), (4.0, -52.0, 18.0, 8.0)]
WALL = (0.62, 0.58, 0.52, 1.0)
# (x, z): trees as small dark-green stretched spheres.
TREES = [(-14.0, 6.0), (-4.0, -2.0), (15.0, 4.0), (22.0, -6.0), (-30.0, -34.0), (36.0, -18.0)]
LEAF = (0.14, 0.3, 0.12, 1.0)


def ent(name, comps):
    return {"id": 0, "name": name, "components": comps}


def mesh(tint):
    return {"mesh": SPHERE, "material": PLAIN, "cast_shadows": 0, "tint": list(tint)}


def main():
    ents = [
        ent("Field", {"Terrain3D": {"terrain": TERRAIN}, "TerrainCollider3D": {"friction": 0.9}, "LocalTransform": {"position": [0, 0, 0]}}),
        ent("World", {
            "WorldSettings": {"gravity_3d": [0, -9.81, 0]},
            # The light travels down and a little toward -z and +x: the sun stands about 55 degrees up behind the camera's left.
            "Environment3D": {"sun_direction": [-0.3, -0.82, -0.48], "sun_intensity": 1.6, "sun_shadows": 0, "sky_enabled": 1,
                              "ambient_sky": [0.3, 0.34, 0.42], "ambient_ground": [0.16, 0.15, 0.13],
                              "exposure": 1.0, "bloom": 1, "bloom_intensity": 0.08},
            "Weather": {"transition_seconds": 10},
            "CloudLayer": {"fair_coverage": 0.5, "scale_m": 100, "speed_scale": 8, "height_m": 1500},
            "CloudShadow3D": {"enabled": 1, "strength": 0.8, "softness": 0.3}}),
        ent("Camera", {"Camera3DComponent": {"primary": 1, "fov_deg": 60},
                       "LocalTransform": {"position": [0, CAMERA_Y, CAMERA_Z], "rotation": [-0.0998, 0, 0, 0.9950]}}),
    ]
    for i, (x, z, w, h) in enumerate(BUILDINGS):
        ents.append(ent("Building %d" % (i + 1), {"MeshRef": mesh(WALL), "LocalTransform": {"position": [x, h * 0.5, z], "scale": [w, h, w]}}))
    for i, (x, z) in enumerate(TREES):
        ents.append(ent("Tree %d" % (i + 1), {"MeshRef": mesh(LEAF), "LocalTransform": {"position": [x, 2.4, z], "scale": [3.0, 4.8, 3.0]}}))
    for i, e in enumerate(ents):
        e["id"] = i + 1
    doc = {"format": "mce-scene", "version": 1, "guid": "b0a30000-0000-4000-8000-00000000001b", "name": "tour_cloudshadow", "entities": ents}
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
