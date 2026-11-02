# M-BAT — Dusk bats: dark flitting specks over the ground at twilight in warm, calm, dry weather, with no shader (milestone record)

**Naming:** the component is `DuskBat` (header `dusk_bat.h`, helpers `dusk_bat_amount` / `dusk_bat_look`), not `Bat`: the Glowmoth game already registers its own `Bat` type, and the reflection registry asserts on two types with one name and different layouts (found by the 30a CI).

**Status:** planned (2026-11-02), steps 30a to 30d. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-BUTTERFLY, M-FIREFLIES and M-BIRDFLOCK (the pattern: a pure-helper header and one render-side `render::ParticleEmitter` that follows the camera, no shader, no state), M-PARTICLEQ and M-PARTICLEUSER (the emitter reads `QualitySettings::particle_scale`, so `r.particles` and the Low tier already apply) and M-SEASONS (`Season`). Butterflies fly by day and fireflies glow at night, but the twilight between them has nothing alive in the air. No Linear issues (free issue limit); this record is the tracker.

**Goal.** An opt-in kit component `DuskBat` turns twilight (the night fade inside a dusk band, dark enough to read and light enough to see), the warm part of the year (a circular trapezoid around the summer centre 0.375 that is wider than the butterfly's), calm wind and dry weather into a strength 0..1, with a manual floor. Pure helpers turn the strength into an emitter look: a few small dark specks that dart quickly (high speed, wide spread, almost no gravity) in a box around the camera. `Kit3DExtract` adds one **render-side `render::ParticleEmitter`** with the reserved id `0xFFFF0007u`, the same path as the other six. No shader, pass, render field or system state; no component is written. A scene with no `DuskBat` (or `enabled` 0, `strength` 0 or an amount of 0) is bit-identical.

**Not in scope:** a shader or a new `render::` field (wing beats), meshes or skinned animals, sound, bats that roost or react to the camera, changing `Season`, `Weather` or the sun, and any stored emitter state.

## Design

- **Opt-in component, no system, no state.** `DuskBat` (`mce/kit/dusk_bat.h`, one per scene, the first in ECS order is read, registered by `install_weather` next to `Butterfly`). Authoring (all `Save`, with a `Range`): `enabled` 1; `window_full` 0.16 and `window_end` 0.26 (half-widths of the seasonal trapezoid around 0.375); `manual` 0 (floor); `dusk_low` 0.25, `dusk_high` 0.85 (the night-fade band in which they fly, smoothed at both ends); `wind_calm` 0.3 and `wind_still` 0.75; `rate` 4 per second at full strength; `box_xz_m` 24, `height_m` 6; `size_m` 0.1; `lifetime_s` 5; `speed` 3; `color` (0.08, 0.07, 0.1); `max_particles` 32; `seed`; `strength` 1.
- **Strength (kit, 30a).** `dusk_bat_amount(const DuskBat&, const Season*, f32 phase, f32 night_fade, f32 wind, f32 rain)` is `max(manual, window(phase)) * dusk_gate(night_fade) * calm_gate(wind) * (1 - rain)` times `strength`. No `Season` gives the manual floor. NaN and inverted ranges fall back to the defaults; polynomial only, fixed evaluation order, no `rand`.
- **Emitter look (kit, 30a).** `dusk_bat_look(const DuskBat&, f32 amount) -> DuskBatLook`; `active = amount > 0`; fast, wide-spread, alpha `0.85 * amount`, not additive.
- **Extract (kit, 30b).** After the butterfly block in `kit3d_extract.cpp`: reads the first `DuskBat`, `Season`, `WorldClock`, the night fade, the wind and the rain; adds a local emitter via `scene.add_particle_emitter`, rate and budget scaled by `particle_scale_of(quality_)` / `scaled_budget` like the other six. Nothing is written or hashed.
- **Determinism.** The amount depends only on authoring, the clock and the weather state; a scene without `DuskBat` is byte-identical.
- **Luau (30c).** `weather.set_dusk_bat{...}`, getters `weather.dusk_bat_<key>()` (colour returns `r, g, b`) and read-only `weather.dusk_bat_flying()`, clamped, named errors, nothing written on error, raises when the component is missing.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 30a. Component and pure helpers | `DuskBat`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and the `components` bump in the MCP transcript golden (the component is placed in the `12_weather` scene in the same PR), guide paragraph 9.19z, `dusk_bat_amount` / `dusk_bat_look`; tests in `tests/kit/test_dusk_bat.cpp` | kit, samples | Sonnet |
| 30b. Extract feed | the local emitter (id `0xFFFF0007`), Low tier and `particle_scale`; tests in `tests/kit/test_dusk_bat_extract.cpp` | kit | Sonnet |
| 30c. Luau, sample | `weather.set_dusk_bat`, getters, `weather.dusk_bat_flying()` in `definitions.cpp`; `12_weather` logs `dusk_bat:`; tests in `tests/script/test_script_bat.cpp` | script, samples | Sonnet |
| 30d. Docs and close | the `08-3d` paragraph, milestone status, follow-ups, changelog | docs | Sonnet |

No step needs Opus: nothing writes a shader.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 30a | done | 1003 | `DuskBat` and the pure `dusk_bat_amount`, `dusk_bat_look` in `mce/kit/dusk_bat.h`, registered in `install_weather`; the `12_weather` scene carries one (manual 0.4). Guide 9.19z. |
| 30b | done | 1005 | Extract feed: one local alpha-blended emitter (id `0xFFFF0007`) from `dusk_bat_look`, Low tier and `particle_scale`; `tests/kit/test_dusk_bat_extract.cpp`. |
| 30c | planned | | |
| 30d | planned | | |

## Follow-ups

- Bats that roost or react to the camera; wing beats (needs per-particle animation or a render field).
- A box strictly above the ground (the same open item as the butterflies).
