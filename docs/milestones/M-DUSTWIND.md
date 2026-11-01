# M-DUSTWIND — Wind-blown dust: tan specks streaming sideways past the camera in a strong dry wind, with no shader (milestone record)

**Status:** planned (2026-11-01), steps 25a to 25d. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-POLLEN and M-LEAFFALL (the pattern: a pure-helper header and one render-side `render::ParticleEmitter` that follows the camera, no shader, no state), M-WINDDIR and M-WINDGUST (`effective_wind`, `wind_direction_vector`) and M-WEATHER (`Weather` wind and rain). A strong wind bends the trees and the rain today, but on a dry day nothing in the air shows it. No Linear issues (free issue limit); this record is the tracker.

**Goal.** An opt-in kit component `Dust` turns the scene wind strength (0..1) and dry weather into a strength 0..1: none below `wind_start`, full at `wind_full`, none in rain, with a manual floor for a desert scene. Pure helpers turn the strength into an emitter look: a fast sideways stream along the wind direction, a short lifetime and a small alpha-blended tan speck. `Kit3DExtract` adds one **render-side `render::ParticleEmitter`** with the reserved id `0xFFFF0004u`, in a wide low box around the camera, the same path rain, snow, leaves, fireflies and pollen use. No new shader, pass, render field or system state; no component is written. A scene without `Dust` is bit-identical.

**Not in scope:** a shader or a new `render::` field, dust that lifts from specific ground patches, a sand storm visibility or fog change, sound, changing `Weather`, `WindGust` or `GroundMist`, and a dust system or any stored emitter state.

## Design

- **Opt-in component, no system, no state.** `Dust` (`mce/kit/dust.h`, one per scene, the first in ECS order is read, registered by `install_weather` next to `Pollen`). Authoring (all `Save`, with a `Range`): `enabled` 1; `wind_start` 0.3 and `wind_full` 0.8 (both Range 0..1, on the scale of `effective_wind(..., kGustSway)`, the gusted `Weather.wind` 0..1; not m/s; `speed` stays m/s); `manual` 0 (floor); `rate` 40 per second at full strength; `box_xz_m` 14, `height_m` 3; `size_m` 0.05; `lifetime_s` 2.5; `speed` 6; `wind_follow` 0.8; `color` (0.78, 0.66, 0.46); `max_particles` 1024; `seed`; `strength` 1.
- **Strength (kit, 25a).** `dust_amount(const Dust&, f32 wind, f32 rain)` (`wind` is the gusted `Weather.wind`, 0..1) is `max(manual, smoothstep(wind_start, wind_full, wind)) * (1 - rain)` times `strength`. NaN and inverted ranges fall back to the defaults; polynomial only, fixed evaluation order, no `rand`.
- **Emitter look (kit, 25a).** `dust_look(const Dust&, f32 amount, f32 wind, const math::vec2& wind_dir) -> DustLook` like `pollen_look`: `active = amount > 0`, direction from the wind, speed scaled by `wind_follow`, alpha `0.6 * amount` fading to 0 at the end of life; not additive.
- **Extract (kit, 25b).** After the pollen block in `kit3d_extract.cpp`: reads the first `Dust`, the first `Weather` (rain only for Rain and Storm), the gusted wind and direction as the leaf block does, and the camera position; adds a local emitter via `scene.add_particle_emitter`. The Low quality tier halves `rate` and `max_particles`. Nothing is written or hashed.
- **Determinism.** The amount depends only on authoring and the weather state; a scene without `Dust` is byte-identical.
- **Luau (25c).** `weather.set_dust{...}`, getters `weather.dust_<key>()` (colour returns `r, g, b`) and read-only `weather.dust_blowing()`, clamped, named errors, nothing written on error, a missing component raises. `samples/12_weather` carries a `Dust` and logs `dust:`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 25a. Component and pure helpers | `Dust`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and the `components` bump in the MCP transcript golden (the component is placed in the `12_weather` scene in the same PR), guide paragraph 9.19w, `dust_amount` / `dust_look`; tests in `tests/kit/test_dust.cpp` | kit, samples | Sonnet |
| 25b. Extract feed | the local emitter, Low tier halving; tests in `tests/kit/test_dust_extract.cpp` | kit | Sonnet |
| 25c. Luau, sample | `weather.set_dust`, getters, `weather.dust_blowing()` in `definitions.cpp`; `12_weather` logs `dust:`; tests in `tests/script/test_script_dust.cpp` | script, samples | Sonnet |
| 25d. Docs and close | guide 9.19w completed, the `08-3d` paragraph, `18-samples` clause, `27-scripting` rows, changelog, status, follow-ups | docs | Sonnet |

No step needs Opus: nothing writes a shader.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 25a | done | 971 | `Dust` and the pure `dust_amount`, `dust_look` in `mce/kit/dust.h`, registered in `install_weather`; the `12_weather` scene carries one (manual 0.4). Guide 9.19w. |
| 25b | done | 972 | Extract feed: one local alpha-blended emitter (id `0xFFFF0004`) from `dust_look`, Low tier halving; `tests/kit/test_dust_extract.cpp`. Guide 9.19w. |
| 25c | planned | | |
| 25d | planned | | |
