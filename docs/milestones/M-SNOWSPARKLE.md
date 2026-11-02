# M-SNOWSPARKLE — Snow sparkle: tiny bright glints over settled snow in low sun on clear days, with no shader (milestone record)

**Status:** planned (2026-11-02), steps 31a to 31d. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-SNOWCOVER (the settled `SnowCover.cover`), M-BAT / M-BUTTERFLY / M-POLLEN (the pattern: a pure-helper header and one render-side `render::ParticleEmitter` that follows the camera, no shader, no state), M-PARTICLEQ and M-PARTICLEUSER (`particle_scale`, `r.particles`). Snow covers the ground but stays flat and dull in sunshine; real snow glitters. No Linear issues (free issue limit); this record is the tracker.

**Naming:** the component is `SnowSparkle` (header `snow_sparkle.h`); `projects/` has no type of that name (checked 2026-11-02; the M-BAT 30a CI showed a clash is only found late).

**Goal.** An opt-in kit component `SnowSparkle` turns the settled snow cover (`SnowCover.cover`), the sun (daylight and a fairly low elevation, where glints are strongest), clear and calm weather into a strength 0..1, with a manual floor. Pure helpers turn it into an emitter look: many very small, short-lived, additive white specks in a flat box around the camera that barely move. `Kit3DExtract` adds one **render-side `render::ParticleEmitter`** with the reserved id `0xFFFF0008u`, the same path as the other seven. No shader, pass, render field or system state; no component is written. A scene with no `SnowSparkle` (or `enabled` 0, `strength` 0, no `SnowCover`, or an amount of 0) is bit-identical.

**Not in scope:** per-pixel glitter in the snow shader, normal-map sparkle, changing `SnowCover`, `Weather` or the sun, sound, and any stored emitter state.

## Design

- **Opt-in component, no system, no state.** `SnowSparkle` (`mce/kit/snow_sparkle.h`, one per scene, the first in ECS order is read, registered by `install_weather`). Authoring (all `Save`, with a `Range`): `enabled` 1; `cover_low` 0.2 and `cover_full` 0.8 (smoothed cover gate); `sun_low` 0.05 and `sun_high` 0.6 (sun elevation band, trapezoid, strongest in the lower part); `manual` 0 (floor, ignores the cover and sun gates but not rain); `rate` 24 per second at full strength; `box_xz_m` 14, `height_m` 1.2; `size_m` 0.03; `lifetime_s` 0.6; `color` (1, 1, 1); `max_particles` 96; `seed`; `strength` 1.
- **Strength (kit, 31a).** `snow_sparkle_amount(const SnowSparkle&, f32 cover, f32 sun_elev, f32 cloud, f32 rain)` is `max(manual, cover_gate * sun_gate) * (1 - cloud) * (1 - rain)` times `strength`. NaN and inverted ranges fall back to the defaults; polynomial only, fixed evaluation order, no `rand`.
- **Emitter look (kit, 31a).** `snow_sparkle_look(const SnowSparkle&, f32 amount) -> SnowSparkleLook`; `active = amount > 0`; additive, almost no speed or gravity.
- **Extract (kit, 31b).** After the dusk bat block in `kit3d_extract.cpp`: reads the first `SnowSparkle`, the first `SnowCover` (`cover * strength`), the sun elevation, cloud and rain from `Weather`; `Weather` has no cloud field, so the cloud term is the cloud-layer kind factor (Clear 0, Snow 0.9, others 1) times the intensity, and no `Weather` is a clear sky; local emitter via `scene.add_particle_emitter`, rate and budget scaled by `particle_scale_of(quality_)` / `scaled_budget`. Nothing is written or hashed.
- **Determinism.** The amount depends only on authoring, the cover and the weather state; a scene without `SnowSparkle` is byte-identical.
- **Luau (31c).** `weather.set_snow_sparkle{...}`, getters `weather.snow_sparkle_<key>()` (colour returns `r, g, b`), read-only `weather.snow_sparkle_active()`; clamped, named errors, nothing written on error, raises when the component is missing.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 31a. Component and pure helpers | `SnowSparkle`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and the `components` bump in the MCP transcript golden, scene placement in `12_weather`, guide paragraph; `tests/kit/test_snow_sparkle.cpp` | kit, samples | Sonnet |
| 31b. Extract feed | the local emitter (id `0xFFFF0008`), Low tier and `particle_scale`; `tests/kit/test_snow_sparkle_extract.cpp` | kit | Sonnet |
| 31c. Luau, sample | `weather.set_snow_sparkle`, getters, `weather.snow_sparkle_active()`; `12_weather` logs `snow_sparkle:`; `tests/script/test_script_snow_sparkle.cpp` | script, samples | Sonnet |
| 31d. Docs and close | the `08-3d` paragraph, milestone status, follow-ups, ROADMAP row, changelog | docs | Sonnet |

No step needs Opus: nothing writes a shader.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 31a | done | 1010 | `SnowSparkle` and the pure `snow_sparkle_amount`, `snow_sparkle_look` in `mce/kit/snow_sparkle.h`, registered in `install_weather`; the `12_weather` scene carries one (manual 0.3). Guide 9.19za. |
| 31b | done | 1012 | Extract feed: one local additive emitter (id `0xFFFF0008`) from `snow_sparkle_look`, Low tier and `particle_scale`; cloud is the cloud-layer kind factor times intensity (Weather has no cloud field); `tests/kit/test_snow_sparkle_extract.cpp`. |
| 31c | done | 1013 | `weather.set_snow_sparkle{...}`, the getters `weather.snow_sparkle_<key>()` and the read-only `weather.snow_sparkle_active()` (the extract's amount: cover, sun elevation, cloud kind factor, rain); typed in `definitions.cpp`; `12_weather` logs `snow_sparkle:`; `tests/script/test_script_snow_sparkle.cpp`. |
| 31d | planned | | |

## Follow-ups

- Per-pixel glitter in the snow shader (Opus-written, independent review).
