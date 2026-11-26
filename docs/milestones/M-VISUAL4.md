# M-VISUAL4: open-world ground detail and life (milestone record)

Added 2026-11-26, owner: the M-VISUAL routine 2 thread, from the audit `assessments/acik-dunya-gorsel-denetimi-2026-11-26.md`. (M-VISUAL2 and M-VISUAL3 are taken.)

| # | Step | State |
|---|---|---|
| 1 | Grass tufts over the park lawns with the existing `Foliage3D` (no new component, no engine change; see the section below) | done 2026-11-26 |
| 2 | Life density at the first frame (spawner density, start view) | planned |
| 3 | Street decal variety (manholes, patches, posters) with the existing decal system | planned |
| 4 | Headlight beam demo: fog on and a side camera in the open-world sample (`NIGHT=beam` capture) | planned |
| 5 | Window interior mapping (shader, render core) | planned |
| 6 | Per-instance tree tone variation | planned |

## Step 1: grass tufts (2026-11-26) — done, data only

**Correction to the audit and to the design note this replaces.** The engine already has the whole thing: `Foliage3D` (M-VISUAL 9) is an instanced scatter, GPU-culled per instance with a `cull_distance`, wind-swayed (`wind_amplitude`, `wind_gust`, `wind_flutter`, with `WindSway`/`WindGust` scaling it per weather) and tinted per copy (`tint_variation`). The audit called the missing ground cover an engine gap; it is a content gap, because `samples/09_openworld` never used `Foliage3D`. A new `GroundCover3D` component, as the earlier design proposed, would have duplicated it, so none is added and no engine code changes.

**What changed.** `gen_assets.py` `grass_cover()` writes a five-blade tuft mesh and puts one `Foliage3D` over every park lawn (32 parks, about 1 tuft per m2, 4.9k tufts a park, 157k in the city, `cull_distance` 45, shadows off, scale 1..2, tint variation, gust 0.3). Capture: `notes/groundcover-after.png` (the lawn near the 50, -30 park; the committed start view is unchanged, so the first frame does not show a park).

**Cost.** llvmpipe (software GL, so a ceiling, not a GPU figure): 30 frames of the park view take 36.4 s with the tufts against 28.1 s without (+29%); the player's triangle and draw counters read the same 138428 / 1603 in both, because the instanced batches are drawn indirectly after the GPU cull. The coordinator asked for the quality tier to scale density if the path cannot take about 20k instances; 157k scattered instances ran with 0 GL errors and the per-instance cull, so no tier scaling was added. `Foliage3D` has no quality scale today; a tier-wide `foliage_density` would be a separate engine step if a real GPU shows a need.
