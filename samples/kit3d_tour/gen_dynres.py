#!/usr/bin/env python3
"""Writes the kit3d_tour dynamic resolution scene (M-DYNRES 46e): assets/scenes/tour_dynres.mcscene.

    python3 samples/kit3d_tour/gen_dynres.py

Stdlib only and deterministic (no clock, no random numbers; every position is a rounded trigonometric value of a fixed index), so
every run writes the same bytes. No new textures or models: it reuses the tour's terrain (guid ...21), sphere mesh (...41) and plain
material (...47).

The scene looks expensive on purpose: a ring of 24 shadow-casting spheres around the camera (two rings of 12 at two radii), 8 coloured
point lights among them, a shadowed sun, volumetric fog and bloom. The world entity carries a DynamicResolution3D with target_ms 12.
On a weak GPU the controller then moves the render scale to keep the frame inside 12 ms; with `-r.dynres.inject "24@0,8@240"` the
cost is a fixed 24 ms at scale 1 for frames 0..239 and 8 ms from frame 240, so the scale steps down in 1/16 steps and returns to
exactly 1.0 on any machine (the sample's GPU test greps the `dynres: scale` log lines). `-r.dynres 0` draws the same scene at the
fixed tier scale. 35 entities: Field, World, Camera, 24 spheres and 8 lights.
"""
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets", "scenes", "tour_dynres.mcscene")

TERRAIN = "guid:b0a30000-0000-4000-8000-000000000021"
SPHERE = "guid:b0a30000-0000-4000-8000-000000000041"
PLAIN = "guid:b0a30000-0000-4000-8000-000000000047"

# (radius of the ring in metres, sphere count, sphere diameter in metres (they rest on the ground), phase in turns)
RINGS = [(6.0, 12, 1.2, 0.0), (11.0, 12, 2.0, 0.5 / 12)]
LIGHT_RING = 8.5
LIGHT_COLORS = [(1.0, 0.45, 0.2), (0.25, 0.5, 1.0), (0.3, 1.0, 0.45), (1.0, 0.85, 0.3),
                (1.0, 0.25, 0.6), (0.35, 0.9, 1.0), (0.7, 0.4, 1.0), (1.0, 0.65, 0.35)]
CAMERA = (0.0, 1.8, 0.0)


def r(v):
    return round(v, 4) + 0.0


def ent(name, comps):
    return {"id": 0, "name": name, "components": comps}


def ring_point(radius, k, count, phase):
    a = 2.0 * math.pi * (k / count + phase)
    return r(radius * math.sin(a)), r(-radius * math.cos(a))


def main():
    ents = [
        ent("Field", {"Terrain3D": {"terrain": TERRAIN}, "TerrainCollider3D": {"friction": 0.9}, "LocalTransform": {"position": [0, 0, 0]}}),
        ent("World", {
            "WorldSettings": {"gravity_3d": [0, -9.81, 0]},
            "Environment3D": {"sun_direction": [-0.4, -0.6, -0.5], "sun_intensity": 1.6, "sun_shadows": 1,
                              "sun_color": [1.0, 0.92, 0.8], "ambient_sky": [0.25, 0.28, 0.36], "ambient_ground": [0.1, 0.09, 0.08],
                              "fog_density": 0.006, "fog_color": [0.6, 0.66, 0.78], "volumetric_fog": 1, "volumetric_density": 0.04,
                              "exposure": 1.0, "bloom": 1, "bloom_intensity": 0.25},
            "DynamicResolution3D": {"enabled": 1, "target_ms": 12, "min_scale": 0.6, "max_scale": 1.0, "headroom": 0.15}}),
        # Looks along -z (the identity rotation), a little downward is not needed: the rings fill the view.
        ent("Camera", {"Camera3DComponent": {"primary": 1, "fov_deg": 70},
                       "LocalTransform": {"position": list(CAMERA), "rotation": [0, 0, 0, 1]}}),
    ]
    n = 0
    for radius, count, size, phase in RINGS:
        for k in range(count):
            x, z = ring_point(radius, k, count, phase)
            n += 1
            ents.append(ent("Sphere %d" % n, {
                "MeshRef": {"mesh": SPHERE, "material": PLAIN, "cast_shadows": 1,
                            "tint": [r(0.35 + 0.5 * ((k * 5) % 7) / 6.0), r(0.35 + 0.5 * ((k * 3) % 5) / 4.0), r(0.35 + 0.5 * ((k * 2) % 3) / 2.0), 1.0]},
                "LocalTransform": {"position": [x, size * 0.5, z], "scale": [size, size, size]}}))
    for k, (cr, cg, cb) in enumerate(LIGHT_COLORS):
        x, z = ring_point(LIGHT_RING, k, len(LIGHT_COLORS), 0.5 / len(LIGHT_COLORS))
        ents.append(ent("Light %d" % (k + 1), {"PointLight3D": {"color": [cr, cg, cb, 1], "intensity": 8.0, "radius": 9},
                                               "LocalTransform": {"position": [x, 2.2, z]}}))
    for i, e in enumerate(ents):
        e["id"] = i + 1
    doc = {"format": "mce-scene", "version": 1, "guid": "b0a30000-0000-4000-8000-000000000018", "name": "tour_dynres", "entities": ents}
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
