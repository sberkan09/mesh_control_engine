# M-LASTFUZZ — fuzzing picking, the 2D camera and selection sets (milestone record)

**Status:** done 2026-11-20 (step 1 found one defect, step 2 fixed it). **Owner: the "Geliştirme rutini 3" thread.** Last round of the editor fuzz series, closed on the coordinator's instruction. No Linear issues. Editor core and tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 seeds each: picking and the marquee against a brute-force model (sprites on random layers and orders, turned and scaled), the camera (pixel/world round trip, zoom about a point keeps it fixed and in range, framing a box keeps it in view), and selection sets against a plain model | done 2026-11-20: `test_scene_editing.cpp`, `test_selection_sets.cpp`; found that a click picked a sprite on a layer below zero over one on layer zero or above |
| 2 | The pick key was a signed 64-bit number whose layer field used the top bit, so negative layers sorted above the rest | fixed 2026-11-20: `viewport2d.cpp` builds the key unsigned; named case "pick_entity picks the higher layer over a lower one, negative layers included" |
