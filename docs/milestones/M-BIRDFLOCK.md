# M-BIRDFLOCK — Bird flocks at dawn and dusk: dark specks crossing the high sky in spring and autumn, with no shader (milestone record)

**Status:** planned (2026-11-01), steps 26a to 26d. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-DUSTWIND, M-POLLEN and M-FIREFLIES (the pattern: a pure-helper header and one render-side `render::ParticleEmitter` that follows the camera, no shader, no state), M-SEASONS (`Season`, `season_phase`, spring centre 0.125 and autumn centre 0.625) and the day-night sun (`sun_direction_at`). The sky has a sun, a moon, stars, clouds, an aurora and meteors today, but nothing alive crosses it. No Linear issues (free issue limit); this record is the tracker.

**Goal.** An opt-in kit component `Flock` turns the migration seasons (a trapezoid around each of the two season centres), the twilight (the sun near the horizon, dawn or dusk) and dry weather into a strength 0..1, with a manual floor for a scene that wants birds all year. Pure helpers turn the strength into an emitter look: a few small dark specks that cross a wide, high box around the camera in a fixed authored heading, fast and straight, with a long lifetime. `Kit3DExtract` adds one **render-side `render::ParticleEmitter`** with the reserved id `0xFFFF0005u`, the same path rain, snow, leaves, fireflies, pollen and dust use. No new shader, pass, render field or system state; no component is written. A scene without `Flock` is bit-identical.

**Not in scope:** a shader or a new `render::` field (wing flapping, a V formation needs per-particle motion), meshes or skinned birds, sound (calls), flocks that react to the camera, changing `Season`, `Weather` or the sun, and a flock system or any stored emitter state.

## Design

- **Opt-in component, no system, no state.** `Flock` (`mce/kit/flock.h`, one per scene, the first in ECS order is read, registered by `install_weather` next to `Dust`). Authoring (all `Save`, with a `Range`): `enabled` 1; `migration_full` 0.06 and `migration_end` 0.12 (half-widths of the trapezoid, in season phase, around 0.125 and 0.625); `manual` 0 (floor); `twilight_width` 0.25 (the sun height `|sun_y|` below which the twilight gate opens, 1 at the horizon); `rate` 3 per second at full strength; `box_xz_m` 60, `height_m` 45; `size_m` 0.35; `lifetime_s` 14; `speed` 9; `heading_deg` 0..360 (the direction of travel on the ground plane, default 70); `color` (0.08, 0.07, 0.07); `max_particles` 64; `seed`; `strength` 1.
- **Strength (kit, 26a).** `flock_amount(const Flock&, const Season*, f32 phase, f32 sun_y, f32 rain)` is `max(manual, migration(phase)) * twilight_gate * (1 - rain)` times `strength`; `migration` is the larger of the two trapezoids, `twilight_gate` is `1 - smoothstep(0, twilight_width, |sun_y|)`. No `Season` gives the manual floor. NaN and inverted ranges fall back to the defaults; polynomial only, fixed evaluation order, no `rand`.
- **Emitter look (kit, 26a).** `flock_look(const Flock&, f32 amount) -> FlockLook {active, rate, speed_min, speed_max, direction, spread, gravity, drag, size, lifetime_min, lifetime_max, color_start, color_end, box, height}`; `active = amount > 0`; direction from `heading_deg`, no gravity, almost no drag; alpha `0.85 * amount` fading to 0 at the end of life; not additive.
- **Extract (kit, 26b).** After the dust block in `kit3d_extract.cpp`: reads the first `Flock`, `Season`, `WorldClock`, the sun height (the `resolve_environment` helper and `sun_direction_at`) and rain, and the camera position; adds a local emitter via `scene.add_particle_emitter`. The Low quality tier halves `rate` and `max_particles`. Nothing is written or hashed.
- **Determinism.** The amount depends only on authoring, the clock and the weather state; a scene without `Flock` is byte-identical.
- **Luau (26c).** `weather.set_flock{...}`, getters `weather.flock_<key>()` (colour returns `r, g, b`) and read-only `weather.flock_flying()`, clamped, named errors, nothing written on error, a missing component raises. `samples/12_weather` carries a `Flock` with `manual` 0.4 and logs `flock:`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 26a. Component and pure helpers | `Flock`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and the `components` bump in the MCP transcript golden (the component is placed in the `12_weather` scene in the same PR), guide paragraph 9.19x, `flock_amount` / `flock_look`; tests in `tests/kit/test_flock.cpp` | kit, samples | Sonnet |
| 26b. Extract feed | the local emitter, Low tier halving; tests in `tests/kit/test_flock_extract.cpp` | kit | Sonnet |
| 26c. Luau, sample | `weather.set_flock`, getters, `weather.flock_flying()` in `definitions.cpp`; `12_weather` logs `flock:`; tests in `tests/script/test_script_flock.cpp` | script, samples | Sonnet |
| 26d. Docs and close | guide 9.19x completed, the `08-3d` paragraph, `18-samples` clause, `27-scripting` rows, changelog, status, follow-ups | docs | Sonnet |

No step needs Opus: nothing writes a shader. If 26b finds that a dark alpha-blended speck cannot read as a bird against the sky without a texture, the milestone stops and records it.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 26a | done | 978 | `Flock` and the pure `flock_amount`, `flock_look` in `mce/kit/flock.h`, registered in `install_weather`; the `12_weather` scene carries one (manual 0.4). Guide 9.19x. |
| 26b | planned | | |
| 26c | planned | | |
| 26d | planned | | |
