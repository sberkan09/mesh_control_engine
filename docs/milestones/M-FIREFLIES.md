# M-FIREFLIES — Fireflies on warm summer nights: drifting glow points around the camera, from the season and the night, with no shader (milestone record)

**Status:** planned (2026-10-31), steps 23a to 23d. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-LEAFFALL (the pattern: a pure-helper header and one render-side `render::ParticleEmitter` that follows the camera, no shader, no state), M-SEASONS (`Season`, `season_phase`, summer centre 0.375), M-NIGHTSKY and M-METEOR (the night fade the sky layers gate on) and M-WEATHER (`Weather` rain and cloud cover). Summer nights are plain dark today: the season thins nothing and lights nothing. No Linear issues (free issue limit); this record is the tracker.

**Goal.** An opt-in kit component `Firefly` turns the `Season` summer weight (a trapezoid around the phase 0.375, `kSeasonSummerCentre`), the night fade and the weather (dry, not stormy) into a strength 0..1, with a manual floor for a scene that wants fireflies all year. Pure helpers turn the strength into an emitter look: a slow hovering drift (tiny gravity, high drag, a small random-ish spread), a short pulse-free lifetime, a small additive warm yellow-green sprite. `Kit3DExtract` adds one **render-side additive `render::ParticleEmitter`** with a reserved id (`0xFFFF0002u`), in a low box around the camera, the same path rain, snow and leaves use. No new shader, pass, render field or system state; no component is written. A scene without `Firefly` is bit-identical.

**Not in scope:** a shader or a new `render::` field (a per-particle blink needs one: the glow is a steady additive point with a lifetime fade), fireflies that spawn from specific bushes or water, lights cast on the ground, sound (crickets), changing `Season`, `Weather`, `NightSky` or `Foliage3D`, and a firefly system or any stored emitter state.

## Design

- **Opt-in component, no system, no state.** `Firefly` (`mce/kit/firefly.h`, one per scene, the first in ECS order is read, registered by `install_weather` next to `LeafFall`; 23a confirms the spot). Authoring (all `Save`, with a `Range`): `enabled` 1; `summer_start` 0.2 and `summer_full` 0.3, `summer_end_full` 0.45 and `summer_end` 0.55 (the phase window, centre 0.375); `manual` 0 (floor); `night_only` 1 and `min_darkness` 0.6 (the night fade below which none show, as `Meteor`); `rate` 18 per second at full strength; `box_xz_m` 14, `height_m` 3; `size_m` 0.08; `lifetime_s` 6; `drift_speed` 0.25; `color` (0.8, 1.0, 0.35); `max_particles` 256; `seed`; `strength` 1.
- **Strength (kit, 23a).** `firefly_amount(const Firefly&, const Season*, f32 phase, f32 night_fade, f32 cloud_cover, f32 rain)` is `max(manual, window(phase)) * night_gate * (1 - rain)` times `strength`; `night_gate` is the `meteor_level` smoothstep (1 when `night_only` is 0). No `Season` gives the manual floor. NaN and inverted ranges fall back to the defaults; polynomial only, fixed evaluation order, no `rand`.
- **Emitter look (kit, 23a).** `firefly_look(const Firefly&, f32 amount) -> FireflyLook {active, rate, speed_min, speed_max, direction, spread, gravity, drag, size, lifetime_min, lifetime_max, color_start, color_end, box, height}`; `active = amount > 0`; alpha `0.9 * amount` fading to 0 at the end of life; additive.
- **Extract (kit, 23b).** After the leaf block in `kit3d_extract.cpp`, reads the first `Firefly`, `Season`, `WorldClock`, the night fade and cloud cover already computed for the meteor block, and the camera position; adds a local emitter via `scene.add_particle_emitter`. The Low quality tier halves `rate` and `max_particles`, as the leaves do. Nothing is written or hashed.
- **Determinism.** The amount depends only on authoring, the clock and the weather state; a scene without `Firefly` is byte-identical.
- **Luau (23c).** `weather.set_firefly{...}`, getters `weather.firefly_<key>()` (colour returns `r, g, b`) and read-only `weather.fireflies_glowing()`, clamped, named errors, nothing written on error, a missing component raises. `samples/12_weather` carries a `Firefly` with `manual` 0.5 and logs `fireflies:`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 23a. Component and pure helpers | `Firefly`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row (the component is placed in the `12_weather` scene in the same PR), guide paragraph 9.19u, `firefly_amount` / `firefly_look`; tests in `tests/kit/test_firefly.cpp` | kit, samples | Sonnet |
| 23b. Extract feed | the local additive emitter, Low tier halving; tests in `tests/kit/test_firefly_extract.cpp` (no component is bit-identical; day, rain and zero amount add nothing; a summer night adds exactly one emitter; id does not collide; follows the camera) | kit | Sonnet |
| 23c. Luau, sample | `weather.set_firefly`, getters, `weather.fireflies_glowing()`, typed in `definitions.cpp`; `12_weather` logs `fireflies:`; tests in `tests/script/test_script_firefly.cpp` | script, samples | Sonnet |
| 23d. Docs and close | guide 9.19u completed, the `08-3d` paragraph, `18-samples` clause, `27-scripting` rows, changelog, status, follow-ups | docs | Sonnet |

No step needs Opus: nothing writes a shader. If 23b finds that an additive sprite cannot read as a point of light without a texture, the milestone stops and records it.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 23a | done | 956 | `Firefly` and the pure `firefly_amount`, `firefly_look` in `mce/kit/firefly.h`, registered in `install_weather`; the `12_weather` scene carries one (manual 0.5). Guide 9.19u. |
| 23b | planned | | |
| 23c | planned | | |
| 23d | planned | | |
