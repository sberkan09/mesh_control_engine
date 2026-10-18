# M-WETSURFACE — Wet surfaces from the weather (milestone record)

**Status:** planned 2026-10-18, step 5a next. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-WEATHER (`Weather.wetness` rises in rain, storm and snow and dries over `dry_seconds`, but nothing renders it). No Linear issues (free issue limit); this record is the tracker. Plan written by an Opus planning pass; the shader and draw path steps need an Opus review before merge and are **not self-merged** (coordinator rule for render).

**Goal.** Wet ground and walls: darker albedo on porous (rough, non-metal) surfaces, sharper reflections, and puddles on up-facing surfaces, driven by `Weather.wetness`, opt-in, off on the mobile Low tier except darkening.

**Not in scope:** rain occlusion under roofs (interiors get wet too; a top-down depth mask would be its own step), rain ripples (animated normals; optional step 5e on High only), writing wetness into materials or `Environment3D`, specialization constants or new pipeline variants (a uniform branch is enough), planar puddle reflections, darkening metals, `sin()`-hash noise (differs across GPUs and would break goldens).

## Design

- **Authoring.** New kit component `WetSurfaces` (`mce/kit/wet_surfaces.h`, one per scene): `strength` 1, `darken` 0.5, `wet_roughness` 0.15, `puddles` 0.5, `puddle_scale_m` 4, explicit pad fields. Not added to `Weather` (its bytes would change `samples/12_weather` hashes). Registered by `install_weather`.
- **Render carrier.** A POD `render::WetSurfaces {wetness, darken, roughness, puddles, puddle_scale, puddles_enabled}` in `scene3d.h` next to `ContactShadows` / `ParallaxSettings`, with `set_wet_surfaces()` / `wet_surfaces()`. `kit3d.h` is not touched.
- **Extract.** `Kit3DExtract::apply_environment` reads the first `Weather` and `WetSurfaces`; `wetness = Weather.wetness * strength` only when both exist; then `render::apply_quality(quality_, wet)`.
- **Quality.** `QualitySettings` gains `wet_surfaces` and `wet_puddles`; Low keeps darkening and roughness, drops puddles; `r.wet_surfaces 0` switches it off.
- **Uniforms.** Two `vec4` after `p_sky_rot` in both `Forward3DPassU` (`forward3d.cpp`) and `MCE_GLSL_PASS_U` (`shaders3d.h`): `p_wet` (wetness, puddles, 1/scale, darken) and `p_wet2` (origin shift mod 256 m, wet roughness). `p_wet.x = 0` skips the whole branch (same idiom as `o_pom.x > 0.0` in M-VISUAL3 step 2), so existing frames are bit-identical.
- **Shader.** In `MCE_GLSL_MESH_FS` after ORM and `apply_decals`, before the roughness clamp (terrain through `MCE_TERRAIN`, never unlit): albedo `*= mix(1, 1 - darken, w * porosity)` with porosity `(1 - metallic) * clamp(rough * 1.25, 0, 1)`; `rough = mix(rough, min(rough, wet_roughness), w)`; puddle mask from integer-hash value noise on `v_world.xz` (octaves 1 and 2.03, crevices from `ao`, up-facing from the geometric normal), puddle roughness 0.03 and the shading normal blended toward the geometric one. SSR and probes follow from the lower roughness.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 5a. Render data | `render::WetSurfaces` POD, `set_wet_surfaces()`, `QualitySettings` fields, `apply_quality` overload, cvar, test of the quality mapping | render, tests/render | Sonnet (Opus glance at the interface) |
| 5b. Shader and pass uniform | the GLSL above, `p_wet` / `p_wet2`, goldens | render, tests/render_gl | Sonnet writes, **Opus reviews, no self-merge** |
| 5c. Kit | `WetSurfaces` component, extract, registration, kit tests, add to `test_padding.cpp` | kit, tests/kit | Sonnet |
| 5d. Luau, sample, docs | `weather.wet_*` in script, `samples/12_weather` turned on, close | script, samples | Sonnet |

## Gates
- Every existing golden under `tests/render_gl/golden/*` stays byte-identical (compare the raw readback, not only the 0.5% block threshold). `p_wet = 0` skips the branch.
- New golden `wet_plaza` (rough 0.8 ground next to a metal sphere under sun and sky) with pixel tests: wetness 1 without puddles drops the mean luminance of the dielectric ground by 25 to 40% (pin the measured value ±2%) and the metal sphere by less than 1%; the ground's specular peak at least 2x; puddles 0.5 at wetness 1 cover 15 to 35% of the up-facing pixels; Low tier equals the no-puddle frame; a +x wall gets darker but never puddles; shifting `origin_shift` by 1000 m moves the frame by less than 0.1%.
- Kit: a scene with `Weather` but no `WetSurfaces`, and a scene with neither, give the same `Scene3D::wet_surfaces()` (wetness 0) and an unchanged state hash; `WetSurfaces` is saved and hash-stable; `test_padding.cpp` covers it.
- Risks: register pressure in `kMeshFS` on mobile (measure with `perf_run`; Low skips the noise); precision of the noise at large coordinates (hence mod 256 m); GPU agreement (uint hashing, no `sin()`).

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 5a | starting | | |
| 5b | planned | | needs 5a; Opus review |
| 5c | planned | | needs 5a |
| 5d | planned | | needs 5b, 5c |
