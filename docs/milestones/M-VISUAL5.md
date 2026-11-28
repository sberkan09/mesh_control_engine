# M-VISUAL5: open-world street life and lawn read (milestone record)

Added 2026-11-26, owner: the M-VISUAL routine 2 thread, from a second look at the day frame after M-VISUAL4 (`notes/audit-day-2.png`, 150 frames of `scenes/city.mcscene` on llvmpipe). The judgement rests on that frame only; no side-by-side with another game was made.

What the frame shows now: road decals, wet-free asphalt patches, tone-varied trees and interior-mapped upper windows read well. What still reads weak:

| # | Step | State |
|---|---|---|
| 1 | Land M-VISUAL4 step 1 (grass tufts, PR #1364): the lawn on the right of the frame is still one flat dark green slab | done 2026-11-28 (PR #1364, with the regenerated `capabilities.md` row and the mesh golden entry for `tuft_grass.glb`; two CI script bugs found on the way, #1376 and #1377) |
| 2 | Lawn and park macro variation: slab albedo is uniform, so a large lawn has no patches or mowing bands | tried 2026-11-26, not shipped: soft light/dark `Decal3D` lawn patches (one new texture, over park lots and the road verges) left the start frame unchanged (`notes/lawn-after.png`); the visible lawn is the `Ground mesh` strip in front of the buildings and the decals did not show there or were too faint at alpha 0.55. Next try: a macro-noise term on the grass material (shader, render core) or large `Foliage3D`-free tinted ground tiles; needs a diagnosis of where the decals landed first |
| 3 | Walkers near the player in the first seconds | done 2026-11-26, no change: at 450 frames (`notes/life-450.png`) five walkers walk the near sidewalk and a car passes in front; the empty street at 150 frames was the spawners still filling. No start view or spawner change needed |
| 4 | Day window read: upper-floor glass looks pale and flat in daylight (interior mapping shows mostly at night). Check the daytime room tint against the sky reflection; a small constant in the glass material, no shader change | measured 2026-11-28, no change: in `notes/audit-day-2.png` the upper windows are mid-tone room tints (about 170/150/125 for the beige ones; dark olive and grey-blue ones too) between white frames, darker than the cream wall and with per-window variety; ground-floor glass is dark navy. They do not read pale or flat, so the hypothesis does not hold and the glass constant stays |
| 5 | Sidewalk edge: the curb is a clean bevel with no wear, gum or chips | tried 2026-11-27, not shipped: a dark soft `Decal3D` grime streak (8 per kerb side per road, one new texture) left the start frame unchanged (`notes/curbgrime-after.png`). The asphalt is already very dark, so a dark streak has no contrast; a lighter wear (chips, salt, gum) or a shader-side edge term would be needed |

## Closed 2026-11-28, next candidates

All five steps are done or measured. What the same day frame (`notes/audit-day-2.png`) still shows, as candidates for a later milestone (not measured against another game, none started):

- Ground-floor shopfront glass is a flat dark navy with no room behind it; interior mapping (`mce_interior_cells`) is only on the upper-floor facade styles. Giving the ground-floor panes a shallow lit interior is a `gen_buildings.py` change plus a check that the dark read is not lost.
- Lawn macro variation needs a shader-side term (render core); `Decal3D` patches did not show (step 2).
- Kerb and asphalt wear needs lighter marks or a shader edge term (step 5).
