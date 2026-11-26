# M-VISUAL5: open-world street life and lawn read (milestone record)

Added 2026-11-26, owner: the M-VISUAL routine 2 thread, from a second look at the day frame after M-VISUAL4 (`notes/audit-day-2.png`, 150 frames of `scenes/city.mcscene` on llvmpipe). The judgement rests on that frame only; no side-by-side with another game was made.

What the frame shows now: road decals, wet-free asphalt patches, tone-varied trees and interior-mapped upper windows read well. What still reads weak:

| # | Step | State |
|---|---|---|
| 1 | Land M-VISUAL4 step 1 (grass tufts, PR #1364): the lawn on the right of the frame is still one flat dark green slab | blocked on the runner's `capabilities.md` regen |
| 2 | Lawn and park macro variation: slab albedo is uniform, so a large lawn has no patches or mowing bands. Data only: a tiled macro-noise detail map or tinted `Decal3D` patches on the `grass` surface | planned |
| 3 | Walkers near the player at the first seconds: at 150 frames only distant cars show and no walker is on the near sidewalk. Measure at 300 and 600 frames first; change the start view or the spawner's near ring only if the street is still empty | planned (measure first) |
| 4 | Day window read: upper-floor glass looks pale and flat in daylight (interior mapping shows mostly at night). Check the daytime room tint against the sky reflection; a small constant in the glass material, no shader change | planned (hypothesis, not yet measured) |
| 5 | Sidewalk edge: the curb is a clean bevel with no wear, gum or chips. Decal variety like step 3 of M-VISUAL4 | planned |
