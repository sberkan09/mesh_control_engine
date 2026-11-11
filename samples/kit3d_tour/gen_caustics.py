#!/usr/bin/env python3
"""Writes the kit3d_tour caustics scene (M-CAUSTICS 52d): assets/scenes/tour_caustics.mcscene.

    python3 samples/kit3d_tour/gen_caustics.py

Stdlib only and deterministic (no clock, no random numbers; every position is a fixed number), so every run writes the same
bytes. No new models, textures or materials: it reuses the unit cube of gen_reflect.py (models/box.glb, guid ...50) with its plain
light grey material reflect_ball (guid ...5b), and the tour's sphere mesh (...41) with its plain material (...47), coloured by the
MeshRef tint.

The scene: a shallow pool, 16 m along x and 12 m along z, cut into a pale stone deck that stands 0.3 m out of the water. A Water
at level 0 fills it (small short waves, clear turquoise water, depth_scale 4 m, no foam) and a Caustics3D on the world entity lays
the sun's moving web of light on what lies under the surface: the light sand floor (1 m deep at the near end, stepping down to
2 m at the far end), the lower part of the pool walls and three rounded rocks that stand half out of the water (above the surface
they stay as they are). A high strong sun from behind the camera, a little to its left (about 58 degrees up). The camera stands
5.2 m above the deck just behind the near edge and looks down into the pool, 45 degrees below the horizon. The look needs the
water to show the floor from above, which it does only through the M-WATER2 refraction (waves, and the prepass the Medium and
High tiers create with SSAO on): the GPU test runs `-r.quality high`; `-r.caustics 0` draws the same frame without the web.
17 entities: World, Camera, Pool, the deck (4 slabs), the floor (2 steps), the step's riser, the walls (4) and 3 rocks.
"""
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets", "scenes", "tour_caustics.mcscene")


def guid(n):
    return "b0a30000-0000-4000-8000-%012x" % n


BOX = "guid:" + guid(0x50)
BOX_MAT = "guid:" + guid(0x5B)
SPHERE = "guid:" + guid(0x41)
PLAIN = "guid:" + guid(0x47)

HALF_X, HALF_Z = 8.0, 6.0  # the pool spans x -8..8 and z -6..6
SHALLOW, DEEP = 1.0, 2.0  # floor depth at the near (+z) and far (-z) halves, metres
DECK_TOP = 0.3
WALL = 0.4  # wall thickness
SAND = [0.86, 0.7, 0.46, 1]
DECK = [0.58, 0.55, 0.5, 1]
TILE = [0.42, 0.55, 0.6, 1]  # the pool walls: pale blue-grey tiles
ROCK = [0.5, 0.47, 0.43, 1]


def r(v):
    return round(v, 4) + 0.0


def ent(name, comps):
    return {"id": 0, "name": name, "components": comps}


def box(name, x, y, z, sx, sy, sz, tint):
    return ent(name, {"MeshRef": {"mesh": BOX, "material": BOX_MAT, "cast_shadows": 0, "tint": tint},
                      "LocalTransform": {"position": [r(x), r(y), r(z)], "scale": [r(sx), r(sy), r(sz)]}})


def rock(name, x, y, z, sx, sy, sz):
    return ent(name, {"MeshRef": {"mesh": SPHERE, "material": PLAIN, "cast_shadows": 0, "tint": ROCK},
                      "LocalTransform": {"position": [r(x), r(y), r(z)], "scale": [r(sx), r(sy), r(sz)]}})


def main():
    pitch = math.radians(-45.0)  # forward -z, tilted down
    q = (math.sin(pitch / 2), 0.0, 0.0, math.cos(pitch / 2))
    ents = [
        ent("World", {
            "WorldSettings": {"gravity_3d": [0, -9.81, 0]},
            # The light travels down, toward -z and a little toward +x: the sun stands about 58 degrees up behind the camera.
            "Environment3D": {"sun_direction": [0.12, -0.85, -0.51], "sun_intensity": 1.5, "sun_shadows": 0,
                              "sun_color": [1.0, 0.95, 0.86], "ambient_sky": [0.26, 0.32, 0.42], "ambient_ground": [0.15, 0.14, 0.12],
                              "exposure": 1.0, "bloom": 1, "bloom_intensity": 0.06, "ssao": 1},
            "Caustics3D": {"enabled": 1, "strength": 1.5, "scale": 1.6, "speed": 1.0, "absorption": 0.15, "max_depth": 6}}),
        ent("Camera", {"Camera3DComponent": {"primary": 1, "fov_deg": 60},
                       "LocalTransform": {"position": [0.0, 5.5, 7.6], "rotation": [r(c) for c in q]}}),
        ent("Pool", {"Water": {"level_y": 0, "half_extent": 40, "amplitude": 0.04, "wavelength": 2.5, "wave_count": 4,
                               "wind_dir_deg": 30, "wind_follow": 0, "steepness": 0.5, "speed_scale": 0.8,
                               "shallow_color": [0.32, 0.72, 0.74], "deep_color": [0.03, 0.22, 0.3], "depth_scale_m": 4,
                               "reflectivity": 0.8, "roughness": 0.06, "foam_amount": 0},
                     "LocalTransform": {"position": [0, 0, 0]}}),
    ]
    # The deck: four slabs around the pool's opening, 80 m wide so the water plane (40 m half extent) never shows past them.
    thick = 3.0
    y = DECK_TOP - thick / 2
    ox, oz = HALF_X + WALL, HALF_Z + WALL  # the opening's half size, walls included
    ents.append(box("Deck north", 0.0, y, -(oz + 40.0) / 2, 80.0, thick, 40.0 - oz, DECK))
    ents.append(box("Deck south", 0.0, y, (oz + 40.0) / 2, 80.0, thick, 40.0 - oz, DECK))
    ents.append(box("Deck west", -(ox + 40.0) / 2, y, 0.0, 40.0 - ox, thick, 2 * oz, DECK))
    ents.append(box("Deck east", (ox + 40.0) / 2, y, 0.0, 40.0 - ox, thick, 2 * oz, DECK))
    # The floor: the near (+z) half at SHALLOW, the far half at DEEP, 0.3 m thick slabs.
    ents.append(box("Floor shallow", 0.0, -SHALLOW - 0.15, HALF_Z / 2, 2 * HALF_X, 0.3, HALF_Z, SAND))
    ents.append(box("Floor deep", 0.0, -DEEP - 0.15, -HALF_Z / 2, 2 * HALF_X, 0.3, HALF_Z, SAND))
    ents.append(box("Step riser", 0.0, -(SHALLOW + DEEP) / 2, -0.1, 2 * HALF_X, DEEP - SHALLOW, 0.2, SAND))
    # The walls, from below the deep floor up to the deck's top.
    wall_h = DECK_TOP + DEEP + 0.3
    wall_y = DECK_TOP - wall_h / 2
    ents.append(box("Wall north", 0.0, wall_y, -(HALF_Z + WALL / 2), 2 * ox, wall_h, WALL, TILE))
    ents.append(box("Wall south", 0.0, wall_y, HALF_Z + WALL / 2, 2 * ox, wall_h, WALL, TILE))
    ents.append(box("Wall west", -(HALF_X + WALL / 2), wall_y, 0.0, WALL, wall_h, 2 * HALF_Z, TILE))
    ents.append(box("Wall east", HALF_X + WALL / 2, wall_y, 0.0, WALL, wall_h, 2 * HALF_Z, TILE))
    # Three rounded rocks standing on the floor and half out of the water.
    ents.append(rock("Rock 1", -3.6, -0.35, 2.4, 2.2, 1.9, 1.8))
    ents.append(rock("Rock 2", 2.8, -0.8, -2.6, 2.6, 2.8, 2.2))
    ents.append(rock("Rock 3", 5.4, -0.45, 3.2, 1.4, 1.5, 1.3))
    for i, e in enumerate(ents):
        e["id"] = i + 1
    assert len(ents) == 17, len(ents)
    doc = {"format": "mce-scene", "version": 1, "guid": guid(0x1C), "name": "tour_caustics", "entities": ents}
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
