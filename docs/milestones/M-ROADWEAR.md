# M-ROADWEAR — worn asphalt instead of flat black

Owner: rutin 2 (M-VISUAL), SBA-235 (data only). Chosen from llvmpipe frames of `samples/09_openworld`.

**Problem.** In a daytime junction frame the road was the weakest surface: a near-black flat plane (mean luma 45 of 255) with
uniform paint, no wear, no patches. Everything around it (brick, trees, paving) had more detail.

**Change.** `samples/09_openworld/tools/gen_textures.py` `asphalt()`: lighter weathered base (v 0.26), large wear variation,
resurfaced patches with a hard darker edge, and wider, darker sealed cracks. Other textures stay byte-identical (seeded rng);
only `asphalt_color.jpg` and `asphalt_normal.jpg` change. No code, component or shader change.

**Check.** Same junction frame before and after: `/mnt/project-files/notes/road-before.png`, `road-after.png`. Mean luma 45 to 66.
