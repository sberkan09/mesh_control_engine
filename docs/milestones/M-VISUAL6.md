# M-VISUAL6: open-world glass, lawn and kerb read (milestone record)

Added 2026-11-28, owner: the M-VISUAL routine 2 thread, from the candidates listed at the end of [M-VISUAL5.md](M-VISUAL5.md). The judgement rests on the day frame `notes/audit-day-2.png` (150 frames of `scenes/city.mcscene` on llvmpipe); no side-by-side with another game was made. Each step is measured on a fresh frame before and after, and a step that leaves the frame unchanged is recorded as tried, not shipped.

| # | Step | State |
|---|---|---|
| 1 | Ground-floor shopfront glass is a flat dark navy with no room behind it. Try interior mapping (`mce_interior_cells`) on the ground-floor panes in `gen_buildings.py`, keeping the dark read | planned |
| 2 | Lawn macro variation: patches or mowing bands. `Decal3D` patches did not show (M-VISUAL5 step 2), so this needs a shader-side term on the grass material (render core, review before merge) | planned |
| 3 | Kerb and asphalt wear: lighter marks (chips, salt, gum) or a shader edge term; dark grime had no contrast on the dark asphalt (M-VISUAL5 step 5) | planned |
