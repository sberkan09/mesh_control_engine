#!/usr/bin/env python3
"""Writes the kit3d_tour river scene (M-WATER2 49h): assets/scenes/tour_river.mcscene.

    python3 samples/kit3d_tour/gen_river.py

Stdlib only and deterministic (no clock, no random numbers; every position is a fixed number or a fixed formula of the stripe
index), so every run writes the same bytes. No new models, textures or materials: it reuses the unit cube of gen_reflect.py
(models/box.glb, guid ...50) with its plain light grey material reflect_ball (guid ...5b), coloured by the MeshRef tint.

The scene: a straight channel along x, 8 m wide between two grassy banks that stand 0.45 m out of the water, with a Water at
level 0 (amplitude 0.08 m, wavelength 3 m, waves travelling down the channel, depth_scale 3 m, shore foam along the banks) and a
WaterFlow of 1.2 m/s down the channel (dir_deg 180, toward -x): the short waves and the ripples drift away from the camera. Three
coloured pillars stand in the water on each side and a stone arch spans the channel farther down: the water mirrors them, broken
by the waves (r.water.reflect). Under the water the seabed is 24 stripes, 1 m each, alternately light sand and dark silt, stepping
down from 0.5 m deep near the camera to 2.5 m deep 24 m farther on, then a dark floor at 2.5 m; four stones lie on it. The stripes show
through the water, bent by the waves, and lose their red first as the water deepens (r.water.refract). A low sun from behind the
camera's right. The camera stands at the near end, 2 m above the water, looking down the channel 13 degrees below the horizon.
43 entities. The look needs the depth read (waves or foam, and the prepass the Medium and High tiers create): the GPU test runs
`-r.quality high`; `-r.water.reflect 0` and `-r.water.refract 0` draw the same frame without each effect.
"""
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets", "scenes", "tour_river.mcscene")


def guid(n):
    return "b0a30000-0000-4000-8000-%012x" % n


BOX = "guid:" + guid(0x50)
PLAIN = "guid:" + guid(0x5B)

NEAR_X, FAR_X = 17.0, -7.0  # the striped stretch of the seabed
STRIPES = 24
SHALLOW, DEEP = 0.5, 2.5  # seabed depth at NEAR_X and FAR_X, metres
HALF_WIDTH = 4.0  # the channel spans z -4..4
BANK_TOP = 0.45


def r(v):
    return round(v, 4) + 0.0


def depth_at(x):
    """The seabed depth under x: SHALLOW at NEAR_X, DEEP at FAR_X and beyond, in steps of one stripe."""
    width = (NEAR_X - FAR_X) / STRIPES
    i = min(STRIPES - 1, max(0, int((NEAR_X - x) // width)))
    return SHALLOW + (DEEP - SHALLOW) * i / (STRIPES - 1)


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def ent(name, comps):
    return {"id": 0, "name": name, "components": comps}


def box(name, mat, x, y, z, sx, sy, sz, tint):
    return ent(name, {"MeshRef": {"mesh": BOX, "material": mat, "cast_shadows": 0, "tint": tint},
                      "LocalTransform": {"position": [r(x), r(y), r(z)], "scale": [r(sx), r(sy), r(sz)]}})


def main():
    yaw, pitch = math.radians(90.0), math.radians(-13.0)  # forward -z turned to -x, then tilted down
    q = quat_mul((0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2)), (math.sin(pitch / 2), 0.0, 0.0, math.cos(pitch / 2)))
    ents = [
        ent("World", {
            "WorldSettings": {"gravity_3d": [0, -9.81, 0]},
            "Environment3D": {"sun_direction": [-0.7, -0.28, 0.65], "sun_intensity": 2.4, "sun_shadows": 0,
                              "sun_color": [1.0, 0.86, 0.68], "ambient_sky": [0.42, 0.52, 0.72], "ambient_ground": [0.2, 0.2, 0.17],
                              "exposure": 1.0, "bloom": 1, "bloom_intensity": 0.08, "ssao": 1}}),
        ent("Camera", {"Camera3DComponent": {"primary": 1, "fov_deg": 60},
                       "LocalTransform": {"position": [17.0, 2.0, 0.0], "rotation": [r(c) for c in q]}}),
        ent("River", {"Water": {"level_y": 0, "half_extent": 60, "amplitude": 0.08, "wavelength": 3, "wind_dir_deg": 180,
                                "wind_follow": 0, "depth_scale_m": 3, "foam_amount": 0.6, "foam_width_m": 0.5},
                      "WaterFlow": {"enabled": 1, "dir_deg": 180, "speed": 1.2},
                      "LocalTransform": {"position": [0, 0, 0]}}),
    ]
    grass = [0.42, 0.55, 0.3, 1]
    bank_len, bank_x = 62.0, -9.0  # x -40..22
    for name, side in (("North bank", -1.0), ("South bank", 1.0)):
        ents.append(box(name, PLAIN, bank_x, (BANK_TOP - 3.0) / 2, side * (HALF_WIDTH + 6.0), bank_len, BANK_TOP + 3.0, 12.0, grass))
    width = (NEAR_X - FAR_X) / STRIPES
    sand, silt = [1.0, 0.9, 0.7, 1], [0.22, 0.2, 0.19, 1]
    for i in range(STRIPES):
        x = NEAR_X - width * (i + 0.5)
        d = depth_at(x)
        ents.append(box("Seabed %02d" % (i + 1), PLAIN, x, -d - 0.15, 0.0, width, 0.3, 2 * HALF_WIDTH + 0.4, sand if i % 2 == 0 else silt))
    ents.append(box("Deep floor", PLAIN, (FAR_X - 40.0) / 2, -DEEP - 0.15, 0.0, FAR_X + 40.0, 0.3, 2 * HALF_WIDTH + 0.4, silt))
    for k, (x, z, s) in enumerate(((13.0, 1.2, 0.6), (7.0, -1.0, 0.8), (1.0, 0.8, 0.7), (-5.0, -1.6, 0.9))):
        ents.append(box("Stone %d" % (k + 1), PLAIN, x, -depth_at(x) + 0.4 * s, z, s, 0.8 * s, s * 0.9, [0.55, 0.52, 0.5, 1]))
    colours = [[1.0, 0.3, 0.22, 1], [1.0, 0.8, 0.2, 1], [0.3, 0.9, 0.45, 1], [0.35, 0.55, 1.0, 1], [1.0, 0.55, 0.15, 1],
               [0.95, 0.95, 0.95, 1]]
    for k, x in enumerate((10.0, 4.0, -2.0)):
        for j, side in enumerate((-1.0, 1.0)):
            ents.append(box("Pillar %d" % (2 * k + j + 1), PLAIN, x, -0.3, side * 2.6, 0.5, 5.4, 0.5, colours[2 * k + j]))
    stone = [0.85, 0.75, 0.62, 1]
    for name, side in (("Arch post north", -1.0), ("Arch post south", 1.0)):
        ents.append(box(name, PLAIN, -9.0, 0.1, side * 3.0, 0.6, 6.2, 0.6, stone))
    ents.append(box("Arch lintel", PLAIN, -9.0, 3.5, 0.0, 0.6, 0.6, 6.6, stone))
    for i, e in enumerate(ents):
        e["id"] = i + 1
    assert len(ents) == 43
    doc = {"format": "mce-scene", "version": 1, "guid": guid(0x1A), "name": "tour_river", "entities": ents}
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
