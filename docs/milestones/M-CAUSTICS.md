# M-CAUSTICS — Animated caustics under water (milestone record)

**Status:** in progress (2026-11-10; 52a data seam). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-WATER, M-UNDERWATER ("caustics on the seabed" was not in scope), M-WATER2 (its follow-up 7) and M-CLOUDSHADOW, whose post pass shape it copies. No Linear issues (free issue limit); this record is the tracker.

**Naming:** no type, file or cvar called `Caustics`, `caustics` or `M-CAUSTICS` existed before 52a (`src/`, `docs/`, `tests/`, `samples/` checked 2026-11-10; the only hits were the "not in scope" and follow-up lines of M-UNDERWATER, M-WATER2, M-SUNSHAFTS and the M-OUTLINE / M-TRANSLUCENT surveys).

**Goal.** The bright, moving web of light the sun throws through a wavy water surface onto what lies below it: a sea floor, a pool bottom, the legs of a pier, a rock half in the water. The web drifts and changes shape with time, fades with the depth below the surface, follows the sun's colour and elevation, and a scene without the component renders exactly as today.

**Not in scope:** refraction of the geometry seen through the water (M-WATER2 owns that), caustics from local lights (point and spot lights under or over water), caustics cast by the real wave geometry (the pattern is procedural, not traced from `WaterSurface`'s waves), caustics on the water's own surface, on transparent surfaces or on particles, light shafts in the water volume, dispersion (a coloured fringe), caustics in the IBL bake, GI captures and reflection probes, and Metal / WebGPU pixels in this milestone (the shader text goes through the existing translation tests only).

## Design

- **Why a post pass.** As with M-CLOUDSHADOW: `kMeshFS` and `kTerrainFS` sit just under the 65536-byte string literal limit and all 16 sampler banks of the lit pass are taken (M-LIGHTCOOKIE). A per-fragment caustic term there would grow both. A post pass on the HDR frame has neither limit. The pass has its own shader string (`kCausticsFS`, at most 65536 bytes) and its own small uniform block (`CausticsU`, a handful of vec4 at bank kPass); `PassU`, `kMeshFS` and `kTerrainFS` are not touched.
- **The pass.** Runs on the HDR frame after the cloud shadows and before the sun shafts, the flare, the underwater composite and bloom. For each non-sky pixel the world position is rebuilt from the half-resolution prepass depth (`ao_gbuf_`, as the cloud shadow pass does). A pixel is lit only when the point lies below the water plane (`WaterSurface::level_y`, inside `half_extent` of the plane's centre, the wave-free plane: the swell is not modelled) and less than `max_depth` below it. The point is moved along the sun direction up to the plane; the procedural pattern is read there in the plane's camera-relative xz divided by `scale`. The pass adds `sun_colour * strength * pattern * sun_share * exp(-absorption * (d_sun + d_view)) * smoothstep(0.02, 0.2, sun_y)` to the pixel, where `d_sun` is the sun ray's length in water (depth below the plane / sun_y), `d_view` the view ray's length in water when the camera is above the plane (from the point to where the view ray crosses the plane; 0 when the camera is below it, where the underwater pass applies its own absorption afterwards), and `sun_share` the sun's share of the frame's light (`cloud_shadow_sun_share`, reused from M-CLOUDSHADOW). Additive, so a frame where the term is 0 is the frame without the pass to the bit.
- **The pattern.** 2 (Medium) or 3 (High) octaves of a cellular "ridged" noise: per octave, the distance to the nearest and second-nearest jittered feature point of an integer lattice, `1 - smoothstep` on their difference gives the bright thin lines of the web. The feature points come from an integer hash (PCG-style on `ivec2` / `uvec2`, no `sin`-hash), and each moves on a closed loop whose phase is `(tick mod P) / P` with an integer period `P`: the clock is wrapped as an integer before it becomes a float, so the pattern repeats exactly after `P` and never drifts with a long session. The pattern has a CPU twin in `mce/render/caustics.h`, and the shader is tested against it.
- **Camera under water.** Decided: the pass runs whether the camera is above or under the water. Seen from under the water the floor's caustics are the most visible case, and the underwater composite (M-UNDERWATER) runs after this pass, so its absorption and fog apply to the added light like to everything else. From above, `d_view` stands in for the water's view-path absorption, which the water shader applies only to its own colour, not to the floor behind it.
- **Render carrier (seam, 52a).** `render::Caustics {enabled, pad_[3], strength, scale, speed, absorption, max_depth}` in `PostFx` (`scene3d.h`, wide header: its own seam PR), `Forward3DStats::caustics` / `caustics_strength`, `caustics_pipe_` and a `record_caustics` stub that records nothing (`forward3d.h`, `forward3d.cpp`), `QualitySettings::caustics` / `caustics_octaves`, cvar `r.caustics`; Low off, Medium 2 octaves, High 3. `apply_quality` clamps (NaN to the default): strength 0..4 (1), scale 0.25..16 m (2), speed 0..4 (1), absorption 0..4 per m (0.15), max_depth 0.5..200 m (30).
- **Kit.** Opt-in `Caustics3D` (`mce/kit/caustics.h`: `enabled`, `strength`, `scale`, `speed`, `absorption`, `max_depth`; explicit pad, no state, a copy of the `SunShafts3D` / `CloudShadow3D` pattern; `kit3d.h` untouched). The water comes from the scene's `Water` and the clock from the render time. A scene without water gets nothing. `Kit3DExtract` feeds `PostFx::caustics` through `apply_quality`, and nothing is hashed.
- **Luau.** `weather.set_caustics{strength=, scale=, speed=, absorption=, max_depth=}` and getters in `api_weather.cpp`, typed in `definitions.cpp`.
- **Sample.** A `tour_caustics` scene in `kit3d_tour`: a shallow pool with a light sand floor and a few rocks crossing the surface, a high strong sun.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 52a. Seam | `render::Caustics` in `PostFx`, stats, `caustics_pipe_`, the `record_caustics` stub, `QualitySettings::caustics` / `caustics_octaves`, `r.caustics` and the clamps, defaults test | render, tests/render | Sonnet; seam PR for `scene3d.h` and `forward3d.h` |
| 52b. Pass | `caustics.h` CPU twin (plane point, pattern, falloff), `kCausticsFS` and `CausticsU`, the pipeline, `record_caustics` after the cloud shadows, GL / SDL GPU pixel tests, golden `caustics`, SPIR-V and MSL translation cases | render, tests/render, tests/render_gl | **Opus writes**, independent Opus review |
| 52c. Kit | `Caustics3D`, registration, padding audit, extract feed through `apply_quality`, quality tier and cvar cases | kit, tests/kit | Sonnet |
| 52d. Luau, sample, docs | `weather.set_caustics`, `tour_caustics`, user guide (8.6, the `PostFx` row, 27.3, the `kit3d_tour` row) | script, samples, docs | Sonnet |
| 52e. Close | GPU cost per tier, the record, the ROADMAP row | docs | Sonnet |

## Gates

- A scene without `Caustics3D` (or with `r.caustics 0`, or on Low, or without a drawn water plane, or with the sun under the horizon) is bit-identical to today; every existing golden stays byte-identical.
- New golden `caustics` with pixel tests: only pixels below the plane change and only brighter; pixels above the plane, the sky and the water surface's own pixels without floor behind them stay as they are; more depth gives less light; the pattern moves with time and is bit-identical one period later; a rebase of the floating origin is bit-identical; the sun near the horizon fades the effect to nothing; the camera under water still draws it.
- The shader agrees with the CPU twin on a grid of sample points.
- `kCausticsFS` at most 65536 bytes; `kMeshFS`, `kTerrainFS` and `PassU` unchanged.
- GL validation clean; GPU cost of the pass recorded per tier.

## Limits (planned, to be confirmed in 52b)

- The pattern is procedural: it does not follow the actual waves, their direction or height, and a calm `WaterSurface` (amplitude 0) still shows a web unless the scene lowers `strength`.
- The plane test uses `level_y` without the swell, so a point within a wave's amplitude of the plane gets the light of the flat plane.
- Not darkened by the cloud shadows: the caustic term uses the sun share, not the cloud density at the point (a later step may multiply by the cloud shadow factor).
- The half-resolution prepass depth is point sampled, as in M-CLOUDSHADOW: a silhouette pixel takes its block's sample.
- Transparent surfaces take the prepass point behind them, or none.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 52a. Seam | done | TBD | `render::Caustics` and `PostFx::caustics` (`scene3d.h`), `Forward3DStats::caustics` / `caustics_strength`, `caustics_pipe_` (freed in shutdown), `record_caustics` (a stub that records nothing and is not called yet), `QualitySettings::caustics` / `caustics_octaves` (Low off / 0, Medium 2, High 3), `r.caustics` (Saved), the `apply_quality` gate and clamps; `test_caustics_defaults.cpp` (defaults, no implicit padding, tiers, cvar, clamps and NaN, pass-through) |
| 52b. Pass | planned | | |
| 52c. Kit | planned | | |
| 52d. Luau, sample, docs | planned | | |
| 52e. Close | planned | | |
