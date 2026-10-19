# M-SNOWCOVER — Snow that settles on up-facing surfaces (milestone record)

**Status:** planned 2026-10-19, step 6a next. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-WEATHER and M-WETSURFACE: snow falls (`Precipitation3D`) and wets (`Weather.wetness`) but nothing piles up, so a snowstorm leaves the ground bare. No Linear issues (free issue limit); this record is the tracker. Render steps (6b) are Opus-written and not self-merged (coordinator rule for render and `scene3d.h`).

**Goal.** While `Weather.current` is snow, up-facing surfaces whiten with a noisy, wind-biased layer that grows with time and melts away afterwards, deterministically, saved with the scene, opt-in, and a scene without it renders exactly as today.

**Not in scope:** snow depth or vertex displacement, footprints, per-object accumulation under roofs (a top-down depth mask would be its own step, same as in M-WETSURFACE), sparkle/glitter, snow on `Foliage3D` leaves, changing `Weather` (its bytes would change every weather sample's hash for no reason).

## Design

- **State.** New kit component `SnowCover` (`mce/kit/snow_cover.h`, one per scene, registered by `install_weather`; `kit3d.h` untouched): authoring `strength` 1, `build_seconds` 90 (full cover at full snow intensity), `melt_seconds` 120, `coverage` 0.8 (share of up-facing ground that whitens at full cover), `noise_scale_m` 3, `wind_bias` 0.5; engine-written and saved `snow_q` (numerator out of 2^24) and `cover` (float 0..1), both `ReadOnly`, explicit pads. A system `kit_snow_cover` (after `kit_weather`) advances `snow_q` with the same integer mul_div idiom as `Weather.wet_q`: while `Weather.current` is Snow it rises by `intensity` per tick toward 1, otherwise it falls by 1 / (`melt_seconds` / dt). No wall clock, no RNG.
- **Render carrier.** `render::SnowCover {cover, coverage, noise_scale, wind_bias, enabled}` in `scene3d.h` next to `WetSurfaces`, `set_snow_cover()` / `snow_cover()`, `QualitySettings::snow_cover` and cvar `r.snow_cover` (Low keeps the whitening, drops the noise breakup), `apply_quality` overload (by value, like the wet one, clamped to 0..1).
- **Uniforms and shader.** One `vec4` `p_snow` (cover, coverage, 1/scale, wind_bias) plus reuse of `p_wet2.xy` (origin mod 256 m) in `Forward3DPassU` and `MCE_GLSL_PASS_U`; GLSL in `MCE_GLSL_MESH_FS` after `apply_wet` behind `p_snow.x > 0.0`, so cover 0 is bit-identical: integer-hash value noise (the `wet_hash`/`wet_value` helpers, periodic over 256 m), mask = smoothstep over (noise + cover bias) times the up-facing weight from the geometric normal (the same stand-in as puddles), albedo mixed toward 0.9 white, roughness toward 0.8, metallic toward 0 under the mask.
- **Extract.** `Kit3DExtract::apply_environment` builds `render::SnowCover` from the first `SnowCover` only when a `Weather` exists as well; the cover is `SnowCover.cover * strength`.
- **Luau.** `weather.snow_cover()` getter and `weather.set_snow{strength=, build=, melt=, coverage=, scale=}`; sample: `samples/12_weather` gets a `SnowCover` and the snow phase whitens the ground.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 6a. Render data | `render::SnowCover` POD, `set_snow_cover()`, `QualitySettings::snow_cover`, `r.snow_cover`, `apply_quality` overload with clamping, test | render, tests/render | Sonnet; seam PR for `scene3d.h`, coordinator review |
| 6b. Shader | `p_snow`, GLSL, `snow_plaza` golden and pixel tests, all goldens byte-identical | render, tests/render_gl | **Opus writes**, coordinator review, no self-merge |
| 6c. Kit | `SnowCover` component, `kit_snow_cover` system, extract, determinism and save tests, padding audit | kit, tests/kit | Sonnet |
| 6d. Luau, sample, docs | `weather.snow_cover()`, `weather.set_snow{}`, `samples/12_weather`, guide, close | script, samples | Sonnet |

6c does not depend on 6b and can run in parallel with it after 6a.

## Gates
- A scene without `SnowCover` (or without `Weather`) gives `Scene3D::snow_cover()` with cover 0 and an unchanged state hash; every existing golden under `tests/render_gl/golden/*` stays byte-identical (compare the raw readback), including `wet_plaza`.
- New golden `snow_plaza` with pixel tests: cover 1 whitens the up-facing ground (mean luminance of the ground up by a pinned amount), a +x wall is not whitened, the metal sphere's top is a documented exception like puddles, Low equals the no-noise frame, an origin shift of 1000 m moves the frame by less than the dry baseline plus 0.1%.
- Kit: `snow_q` is integer-exact and saved (a save/load mid-snow resumes with the same state hash); cover rises under snow, melts after, and never leaves 0..1; a clear scene leaves it 0.
- Risks: `kMeshFS` register pressure (measure with `perf_run`; this is the item left open in M-WETSURFACE); noise precision at large coordinates (hence mod 256 m); overlap with the puddle mask (snow wins over water where `cover` is high).

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 6a | done | #622 | `render::SnowCover`, `r.snow_cover`, tier fields, `apply_quality` overload |
| 6b | planned | | needs 6a; Opus writes, coordinator review |
| 6c | done, awaiting PR | | `SnowCover` component, `kit_snow_cover`, extract, tests/kit/test_snow_cover.cpp |
| 6d | planned | | needs 6b, 6c |
