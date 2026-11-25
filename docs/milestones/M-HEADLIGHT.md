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

**Step 2 (shader).** `ObjectU` gains a last `vec4 o_retro` (24 vec4, still one 512-byte ring slot; older members keep their offsets). In the Forward+ local-light loop of `kMeshFS`, behind a uniform branch `o_retro.x > 0`, a spot or point light adds `retro * 3 * pow(max(dot(v, l), 0), 24) * max(dot(n, l), 0) * diffuse * light` : bright when the eye sits on the line to the light (a headlight), nothing when it does not. Material factor 0 skips the branch, so every existing frame is unchanged (`test_render_gl` passes in full). The sun is not a headlight and is not touched.

**Step 3 (sample).** The cars of `samples/09_openworld` already carry `SpotLight3D` headlights on a `TimedLight3D` (on from 18:30), so the only change is data: `marking_white` gets `retro_reflect` 0.8 and `marking_yellow` 0.6 in `gen_assets.py`. A 22:00 llvmpipe frame without a car in view shows no change (the term needs a light near the eye); the effect is covered by the `test_render_gl` headlight test of step 2. A frame from the driver's seat is the remaining visual check.
