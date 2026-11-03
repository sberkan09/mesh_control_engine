# M-MIDGES — Midge swarms: tiny dark specks that dance low over the ground at dawn and dusk in warm, calm, dry weather, with no shader (milestone record)

**Status:** done (2026-11-03), steps 33a to 33d (PRs 1031, 1033, 1036, 1038, 1040). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-BUTTERFLY, M-FIREFLIES and M-BAT (the pattern: a pure-helper header and one render-side `render::ParticleEmitter` that follows the camera, no shader, no state), M-PARTICLEFLOOR (`particle_floor_center_y`, so the box floor sits at the ground) and M-SEASONS (`Season`, `season_phase`). Butterflies fly by day, fireflies and bats at night, but the twilight hour of a warm evening has nothing alive near the ground: the thin, dancing cloud of gnats that real summer dusks have. No Linear issues (free issue limit); this record is the tracker.

**Naming:** the component is `Midge` (header `midge.h`); `projects/`, `src/` and `docs/` have no type of that name (checked 2026-11-02; the M-BAT 30a CI showed a clash is only found late).

**Goal.** An opt-in kit component `Midge` turns a wide warm-season window (a circular trapezoid around the summer centre 0.375), the twilight band (sun elevation near the horizon, both dawn and dusk), calm wind and dry weather into a strength 0..1, with a manual floor for a scene that wants midges all year. Pure helpers turn the strength into an emitter look: many tiny dark specks with a high spread and almost no net speed, so they hover and shimmer in a low box around the camera. `Kit3DExtract` adds one **render-side `render::ParticleEmitter`** with the reserved id `0xFFFF0009u`, the same path as the other eight. No shader, pass, render field or system state; no component is written. A scene with no `Midge` (or `enabled` 0, `strength` 0, or an amount of 0) is bit-identical.

**Not in scope:** a shader or a new `render::` field, meshes or skinned insects, sound, swarms anchored to water or a position (the box follows the camera; a position anchor is a follow-up), reaction to the camera, changing `Season`, `Weather` or the sun, and any stored emitter state.

## Design

- **Opt-in component, no system, no state.** `Midge` (`mce/kit/midge.h`, one per scene, the first in ECS order is read, registered by `install_weather` next to `SnowSparkle`). Authoring (all `Save`, with a `Range`): `enabled` 1; `window_full` 0.12 and `window_end` 0.2 (half-widths of the trapezoid in season phase around 0.375); `manual` 0 (floor, ignores the window and twilight gates but not rain); `twilight_low` -0.12 and `twilight_high` 0.25 (sun elevation, the sines of the angle: the gate is 0 below `twilight_low`, rises to 1 at the horizon and falls to 0 at `twilight_high`); `wind_calm` 0.2 and `wind_still` 0.6 (the wind, 0..1, below which they fly fully and above which none fly); `rate` 30 per second at full strength; `box_xz_m` 8, `height_m` 2.5; `size_m` 0.025; `lifetime_s` 3; `speed` 0.35; `color` (0.08, 0.07, 0.06); `max_particles` 192; `seed`; `strength` 1.
- **Strength (kit, 33a).** `midge_amount(const Midge&, const Season*, f32 phase, f32 sun_elev, f32 wind, f32 rain)` is `max(manual, window(phase) * twilight(sun_elev)) * calm_gate * (1 - rain)` times `strength`. No `Season` gives the manual floor. NaN and inverted ranges fall back to the defaults; polynomial only, fixed evaluation order, no `rand`.
- **Emitter look (kit, 33a).** `midge_look(const Midge&, f32 amount) -> MidgeLook`; `active = amount > 0`; wide spread, low speed, no gravity, alpha `0.7 * amount` fading to 0 at the end of life; not additive (dark specks).
- **Extract (kit, 33b).** After the snow sparkle block in `kit3d_extract.cpp`: reads the first `Midge`, `Season`, `WorldClock`, the sun elevation, the wind and the rain; adds a local emitter via `scene.add_particle_emitter` whose centre uses `particle_floor_center_y`; rate and budget scaled by `particle_scale_of(quality_)` / `scaled_budget`. Nothing is written or hashed.
- **Determinism.** The amount depends only on authoring, the clock and the weather state; a scene without `Midge` is byte-identical.
- **Luau (33c).** `weather.set_midge{...}`, getters `weather.midge_<key>()` (colour returns `r, g, b`) and read-only `weather.midge_active()`, clamped, named errors, nothing written on error, a missing component raises. `samples/12_weather` carries a `Midge` with `manual` 0.3 and logs `midge:`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 33a. Component and pure helpers | `Midge`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and the `components` bump in the MCP transcript golden, scene placement in `12_weather`, guide paragraph; `tests/kit/test_midge.cpp` | kit, samples | Sonnet |
| 33b. Extract feed | the local emitter (id `0xFFFF0009`), floor centre, Low tier and `particle_scale`; `tests/kit/test_midge_extract.cpp` | kit | Sonnet |
| 33c. Luau, sample | `weather.set_midge`, getters, `weather.midge_active()`; `12_weather` logs `midge:`; `tests/script/test_script_midge.cpp` | script, samples | Sonnet |
| 33d. Docs and close | the `08-3d` paragraph, milestone status, follow-ups, ROADMAP row, changelog | docs | Sonnet |

No step needs Opus: nothing writes a shader.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 33a | done | 1033 | `Midge` and the pure `midge_amount`, `midge_look` in `mce/kit/midge.h`, registered in `install_weather`; the `12_weather` scene carries one (manual 0.3). Guide 9.19zb. |
| 33b | done | 1036 | Extract feed: one local alpha-blended emitter (id `0xFFFF0009`) from `midge_look`, box floor via `particle_floor_center_y`, Low tier and `particle_scale`; the sun elevation stays signed (-1..1) because the twilight gate needs the dip below the horizon; `tests/kit/test_midge_extract.cpp`. |
| 33c | done | 1038 | `weather.set_midge{...}` (18 keys), the `weather.midge_<key>()` getters and the read-only `weather.midge_active()` (the extract's amount; the script reads the clock, the extract also applies `r.env` overrides); typed in `definitions.cpp`; `12_weather` logs `midge:`; `tests/script/test_script_midge.cpp`. |
| 33d | done | 1040 | The `08-3d` "Midges" paragraph, milestone closed, ROADMAP row done. |

## Follow-ups

- A swarm anchored to a world position (a pond, a lamp) instead of the camera.
- A real GPU/windowed look at the dark specks against sky and grass (size, alpha).
