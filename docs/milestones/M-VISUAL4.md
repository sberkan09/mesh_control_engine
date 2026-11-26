# M-VISUAL4: open-world ground detail and life (milestone record)

Added 2026-11-26, owner: the M-VISUAL routine 2 thread, from the audit `assessments/acik-dunya-gorsel-denetimi-2026-11-26.md`. (M-VISUAL2 and M-VISUAL3 are taken.)

| # | Step | State |
|---|---|---|
| 1 | Runtime ground cover: instanced grass tufts, flowers and pebbles over lawns and verges, wind sway, density by quality tier. Render core and kit, design note first; coordinator review | planned |
| 2 | Life density at the first frame (spawner density, start view) | done 2026-11-26, no change: measured, the 12-frame capture predates the spawners; after 150 frames (`notes/life-150.png`) the avenue has cars and walkers (24 cars / 70 walkers allowed). Spawners fill over the first seconds, which the headless capture length hid |
| 3 | Street decal variety (manhole covers, asphalt patches) with the existing `Decal3D` | done 2026-11-26 (data only; posters left out) |
| 4 | Headlight beam demo: fog on and a side camera in the open-world sample (`NIGHT=beam` capture) | done 2026-11-26, capture-only (`tools/night_shot.sh`, `volumetric_fog` on, side camera); the beam shows as a lit cone from the parked car (`notes/headbeam-demo.png`). The committed scene is unchanged |
| 5 | Window interior mapping (shader, render core) | planned |
| 6 | Per-instance tree tone variation | planned |

## Step 1 design: `GroundCover3D` (2026-11-26)

**Component** (kit, level data, `Save`): `mesh`, `material` (a tuft model and its material, the sample's own), `density` (instances per m2, 0..40), `radius` (metres around the camera that carry cover, 5..80, default 30), `min_scale` / `max_scale`, `sway` (0..1, uses `WindSway` / `WindGust` so cover and trees move together), `seed`, `surface_y` (the lawn's top, default 0.05), and `area` (a box in world xz; cover grows only inside it, so lawns and verges are chosen by drawing boxes, roads and plazas stay clean).

**Placement.** A world-aligned jittered grid of cell size `1/sqrt(density)`; each cell's hash (the same pcg2d used by the wet noise, seeded) picks its offset, yaw, scale and whether it is present. The cells within `radius` of the camera and inside `area` become instances. The set depends only on (camera cell, seed, area), so it is deterministic and does not pop when the camera moves inside a cell. Distance fade: scale to 0 over the last 20% of the radius.

**Draw.** Instances are submitted as `MeshInstance` of one mesh and material per component, so the existing instancing and merge path draws them; no new render pass. Shadows off (`cast_shadows 0`), so the cost is the colour pass only. Quality tiers scale `density` (Low 0.25, Medium 0.6, High 1) and `radius`.

**Open questions for the coordinator** (this is render core and kit): (a) is the camera-follow grid generation per frame acceptable on the CPU for about 20k instances, or should placement live in a compute pass (the RHI has compute); (b) is the instancing path happy with 20k small instances of one mesh (the instancing GL test covers hundreds); (c) wind: a per-instance phase from the cell hash in the vertex shader, as `WindSway` does for trees.

**Sample.** Two low-poly tuft meshes and a pebble from `gen_models.py`, three `GroundCover3D` entities over the city's park lawns and verges. A before/after capture goes to `notes/`.

**Order.** Design review (this note) first; implementation is the next PR.

## Step 3: street decals (2026-11-26) — done, data only

`gen_assets.py` writes two 64 x 64 RGBA decal textures (`manhole.png`, `asphalt_patch.png`) and `street_decals()` puts three covers and two patches on every road (a private `random.Random` per road, so no other sequence moves; added after everything else so the older entities keep their order). The 3 distinct textures of the frame budget are used by these two plus the hidden bullet-hole ring. Captures: `notes/streetdecals-after.png` (the start view: one patch beside the avenue) and `notes/streetdecals-manhole.png` / `-zoom.png` (a cover and a patch from the avenue's middle). Honest read: the covers are small and dark, so they read as a dark disc with a faint ring at street distance; the patches read well. Posters (a vertical decal on building walls) were not added.
