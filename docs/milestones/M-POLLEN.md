# M-POLLEN — Spring pollen and dandelion seeds: pale specks drifting on the wind around the camera by day, from the season, with no shader (milestone record)

**Status:** planned (2026-11-01), steps 24a to 24d. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-FIREFLIES and M-LEAFFALL (the pattern: a pure-helper header and one render-side `render::ParticleEmitter` that follows the camera, no shader, no state), M-SEASONS (`Season`, `season_phase`, spring centre 0.125, `kSeasonSpringCentre`), M-WINDDIR and M-WINDGUST (`effective_wind`, `wind_direction_vector`) and M-WEATHER (`Weather` rain). Spring is a foliage tint today and nothing floats in the air. No Linear issues (free issue limit); this record is the tracker.

**Goal.** An opt-in kit component `Pollen` turns the `Season` spring weight (a trapezoid around the phase 0.125 that wraps the year start), daylight and dry weather into a strength 0..1, with a manual floor for a scene that wants pollen all year. Pure helpers turn the strength into an emitter look: a very slow fall plus a sideways drift from the scene wind, a long lifetime and a tiny alpha-blended pale yellow-white sprite. `Kit3DExtract` adds one **render-side `render::ParticleEmitter`** with the reserved id `0xFFFF0003u`, in a wide low box around the camera, the same path rain, snow, leaves and fireflies use. No new shader, pass, render field or system state; no component is written. A scene without `Pollen` is bit-identical.

**Not in scope:** a shader or a new `render::` field (per-speck spin or flicker), specks that spawn from specific trees or flowers, allergy gameplay, sound, changing `Season`, `Weather`, `Foliage3D` or `LeafFall`, and a pollen system or any stored emitter state.

## Design

- **Opt-in component, no system, no state.** `Pollen` (`mce/kit/pollen.h`, one per scene, the first in ECS order is read, registered by `install_weather` next to `Firefly`; 24a confirms the spot). Authoring (all `Save`, with a `Range`): `enabled` 1; `spring_start` 0.95 and `spring_full` 0.02, `spring_end_full` 0.22 and `spring_end` 0.32 (the phase window centred on 0.125, wrapping the year start, so the helper unwraps it as a signed distance from the centre); `manual` 0 (floor); `day_only` 1 and `min_daylight` 0.5 (the daylight below which none show: one minus the night fade); `rate` 14 per second at full strength; `box_xz_m` 18, `height_m` 5; `size_m` 0.06; `lifetime_s` 12; `fall_speed` 0.08; `wind_follow` 0.6; `color` (1.0, 0.95, 0.7); `max_particles` 512; `seed`; `strength` 1.
- **Strength (kit, 24a).** `pollen_amount(const Pollen&, const Season*, f32 phase, f32 night_fade, f32 rain)` is `max(manual, window(phase)) * day_gate * (1 - rain)` times `strength`; `day_gate` is a smoothstep over `1 - night_fade` from `min_daylight` to 1 (1 when `day_only` is 0). No `Season` gives the manual floor. NaN and inverted ranges fall back to the defaults; polynomial only, fixed evaluation order, no `rand`.
- **Emitter look (kit, 24a).** `pollen_look(const Pollen&, f32 amount, f32 wind, const math::vec2& wind_dir) -> PollenLook {active, rate, speed_min, speed_max, direction, spread, gravity, drag, size, lifetime_min, lifetime_max, color_start, color_end, box, height}` like `leaf_fall_look`; `active = amount > 0`; alpha `0.8 * amount` fading to 0 at the end of life; not additive.
- **Extract (kit, 24b).** After the firefly block in `kit3d_extract.cpp`: reads the first `Pollen`, `Season`, `WorldClock`, the night fade (the `resolve_environment` helper) and rain, the scene wind as the leaf block does, and the camera position; adds a local emitter via `scene.add_particle_emitter`. The Low quality tier halves `rate` and `max_particles`. Nothing is written or hashed.
- **Determinism.** The amount depends only on authoring, the clock and the weather state; a scene without `Pollen` is byte-identical.
- **Luau (24c).** `weather.set_pollen{...}`, getters `weather.pollen_<key>()` (colour returns `r, g, b`) and read-only `weather.pollen_drifting()`, clamped, named errors, nothing written on error, a missing component raises. `samples/12_weather` carries a `Pollen` with `manual` 0.4 and logs `pollen:`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 24a. Component and pure helpers | `Pollen`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and the `components` bump in the MCP transcript golden (the component is placed in the `12_weather` scene in the same PR), guide paragraph 9.19v, `pollen_amount` / `pollen_look`; tests in `tests/kit/test_pollen.cpp` | kit, samples | Sonnet |
| 24b. Extract feed | the local emitter, Low tier halving; tests in `tests/kit/test_pollen_extract.cpp` | kit | Sonnet |
| 24c. Luau, sample | `weather.set_pollen`, getters, `weather.pollen_drifting()` in `definitions.cpp`; `12_weather` logs `pollen:`; tests in `tests/script/test_script_pollen.cpp` | script, samples | Sonnet |
| 24d. Docs and close | guide 9.19v completed, the `08-3d` paragraph, `18-samples` clause, `27-scripting` rows, changelog, status, follow-ups | docs | Sonnet |

No step needs Opus: nothing writes a shader. If 24b finds that an alpha-blended sprite cannot read as a speck without a texture, the milestone stops and records it.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 24a | planned | | |
| 24b | planned | | |
| 24c | planned | | |
| 24d | planned | | |
