# M-HEADLIGHT — reflective paint under headlights

Owner: rutin 2 (M-VISUAL). Step 3 of [M-PAINTWEAR](M-PAINTWEAR.md).

**Problem.** Road markings look the same at night as by day. Real lane paint contains glass beads and throws light back toward a car's headlights.

**Split (agreed with the coordinator):** three PRs. (1) the seam: the factor in the material (below), cooker and editor, no behaviour change; (2) the shader term and its golden (coordinator review); (3) headlights and the paint flag in the open-world sample.

**Step 1 design.** The factor is quantized to 8 bits into `MaterialChunk::flags` bits 24..31 (`MaterialFlags::RetroMask`, `mce/asset/material_retro.h`), the same way M-TRANSLUCENT stores translucency and wrap. This leaves `MaterialChunk`, `MaterialExt` and the chunk versions untouched, so every cooked pack and the mesh-chunk layout stay valid and a material cooked before reads 0 (no reflection). `.mcmat` key: `retro_reflect` 0..1.

**Plan.**
1. A retro-reflection term for materials flagged reflective: brighter when the light and the eye direction agree (headlight on a vehicle, camera behind it). Needs the coordinator: new material field and shader term.
2. The two marking materials set the flag; chipped (alpha-cut) areas already show plain asphalt, so they stay dark.
3. Vehicle headlights in the open-world sample as spot lights for the check.

**Check.** Night junction frame with and without the term.
