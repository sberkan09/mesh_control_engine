# M-VISUAL4: open-world ground detail and life (milestone record)

Added 2026-11-26, owner: the M-VISUAL routine 2 thread, from the audit `assessments/acik-dunya-gorsel-denetimi-2026-11-26.md`. (M-VISUAL2 and M-VISUAL3 are taken.)

| # | Step | State |
|---|---|---|
| 1 | Runtime ground cover: instanced grass tufts, flowers and pebbles over lawns and verges, wind sway, density by quality tier. Render core and kit, design note first; coordinator review | planned |
| 2 | Life density at the first frame (spawner density, start view) | planned |
| 3 | Street decal variety (manholes, patches, posters) with the existing decal system | planned |
| 4 | Headlight beam demo: fog on and a side camera in the open-world sample (`NIGHT=beam` capture) | planned |
| 5 | Window interior mapping (shader, render core) | planned |
| 6 | Per-instance tree tone variation | planned |

## Step 1 design: `GroundCover3D` (2026-11-26)

**Component** (kit, level data, `Save`): `mesh`, `material` (a tuft model and its material, the sample's own), `density` (instances per m2, 0..40), `radius` (metres around the camera that carry cover, 5..80, default 30), `min_scale` / `max_scale`, `sway` (0..1, uses `WindSway` / `WindGust` so cover and trees move together), `seed`, `surface_y` (the lawn's top, default 0.05), and `area` (a box in world xz; cover grows only inside it, so lawns and verges are chosen by drawing boxes, roads and plazas stay clean).

**Placement.** A world-aligned jittered grid of cell size `1/sqrt(density)`; each cell's hash (the same pcg2d used by the wet noise, seeded) picks its offset, yaw, scale and whether it is present. The cells within `radius` of the camera and inside `area` become instances. The set depends only on (camera cell, seed, area), so it is deterministic and does not pop when the camera moves inside a cell. Distance fade: scale to 0 over the last 20% of the radius.

**Draw.** Instances are submitted as `MeshInstance` of one mesh and material per component, so the existing instancing and merge path draws them; no new render pass. Shadows off (`cast_shadows 0`), so the cost is the colour pass only. Quality tiers scale `density` (Low 0.25, Medium 0.6, High 1) and `radius`.

**Open questions for the coordinator** (this is render core and kit): (a) is the camera-follow grid generation per frame acceptable on the CPU for about 20k instances, or should placement live in a compute pass (the RHI has compute); (b) is the instancing path happy with 20k small instances of one mesh (the instancing GL test covers hundreds); (c) wind: a per-instance phase from the cell hash in the vertex shader, as `WindSway` does for trees.

**Sample.** Two low-poly tuft meshes and a pebble from `gen_models.py`, three `GroundCover3D` entities over the city's park lawns and verges. A before/after capture goes to `notes/`.

**Order.** Design review (this note) first; implementation is the next PR.
