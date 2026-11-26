# M-VISUAL4: open-world ground detail and life (milestone record)

Added 2026-11-26, owner: the M-VISUAL routine 2 thread, from the audit `assessments/acik-dunya-gorsel-denetimi-2026-11-26.md`. (M-VISUAL2 and M-VISUAL3 are taken.)

| # | Step | State |
|---|---|---|
| 1 | Grass tufts over the park lawns with the existing `Foliage3D` (no new component, no engine change; see the section below) | done 2026-11-26 |
| 2 | Life density at the first frame (spawner density, start view) | done 2026-11-26, no change: measured, the 12-frame capture predates the spawners; after 150 frames (`notes/life-150.png`) the avenue has cars and walkers (24 cars / 70 walkers allowed). Spawners fill over the first seconds, which the headless capture length hid |
| 3 | Street decal variety (manhole covers, asphalt patches) with the existing `Decal3D` | done 2026-11-26 (data only; posters left out) |
| 4 | Headlight beam demo: fog on and a side camera in the open-world sample (`NIGHT=beam` capture) | planned |
| 5 | Window interior mapping (shader, render core) | planned |
| 6 | Per-instance tree tone variation | planned |

## Step 1: grass tufts (2026-11-26) — done, data only

**Correction to the audit and to the design note this replaces.** The engine already has the whole thing: `Foliage3D` (M-VISUAL 9) is an instanced scatter, GPU-culled per instance with a `cull_distance`, wind-swayed (`wind_amplitude`, `wind_gust`, `wind_flutter`, with `WindSway`/`WindGust` scaling it per weather) and tinted per copy (`tint_variation`). The audit called the missing ground cover an engine gap; it is a content gap, because `samples/09_openworld` never used `Foliage3D`. A new `GroundCover3D` component, as the earlier design proposed, would have duplicated it, so none is added and no engine code changes.

**What changed.** `gen_assets.py` `grass_cover()` writes a five-blade tuft mesh and puts one `Foliage3D` over every park lawn (32 parks, about 1 tuft per m2, 4.9k tufts a park, 157k in the city, `cull_distance` 45, shadows off, scale 1..2, tint variation, gust 0.3). Capture: `notes/groundcover-after.png` (the lawn near the 50, -30 park; the committed start view is unchanged, so the first frame does not show a park).

**Cost.** llvmpipe (software GL, so a ceiling, not a GPU figure): 30 frames of the park view take 36.4 s with the tufts against 28.1 s without (+29%); the player's triangle and draw counters read the same 138428 / 1603 in both, because the instanced batches are drawn indirectly after the GPU cull. The coordinator asked for the quality tier to scale density if the path cannot take about 20k instances; 157k scattered instances ran with 0 GL errors and the per-instance cull, so no tier scaling was added. `Foliage3D` has no quality scale today; a tier-wide `foliage_density` would be a separate engine step if a real GPU shows a need.

**Draw.** Instances are submitted as `MeshInstance` of one mesh and material per component, so the existing instancing and merge path draws them; no new render pass. Shadows off (`cast_shadows 0`), so the cost is the colour pass only. Quality tiers scale `density` (Low 0.25, Medium 0.6, High 1) and `radius`.

**Open questions for the coordinator** (this is render core and kit): (a) is the camera-follow grid generation per frame acceptable on the CPU for about 20k instances, or should placement live in a compute pass (the RHI has compute); (b) is the instancing path happy with 20k small instances of one mesh (the instancing GL test covers hundreds); (c) wind: a per-instance phase from the cell hash in the vertex shader, as `WindSway` does for trees.

**Sample.** Two low-poly tuft meshes and a pebble from `gen_models.py`, three `GroundCover3D` entities over the city's park lawns and verges. A before/after capture goes to `notes/`.

**Order.** Design review (this note) first; implementation is the next PR.

## Step 3: street decals (2026-11-26) — done, data only

`gen_assets.py` writes two 64 x 64 RGBA decal textures (`manhole.png`, `asphalt_patch.png`) and `street_decals()` puts three covers and two patches on every road (a private `random.Random` per road, so no other sequence moves; added after everything else so the older entities keep their order). The 3 distinct textures of the frame budget are used by these two plus the hidden bullet-hole ring. Captures: `notes/streetdecals-after.png` (the start view: one patch beside the avenue) and `notes/streetdecals-manhole.png` / `-zoom.png` (a cover and a patch from the avenue's middle). Honest read: the covers are small and dark, so they read as a dark disc with a faint ring at street distance; the patches read well. Posters (a vertical decal on building walls) were not added.
