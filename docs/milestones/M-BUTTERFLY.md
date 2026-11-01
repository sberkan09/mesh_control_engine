# M-BUTTERFLY — Summer butterflies: bright specks fluttering low over the ground by day in warm, calm, dry weather, with no shader (milestone record)

**Status:** done (2026-11-01), steps 27a to 27d (PRs 984, 985, 986, 987, 988). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-POLLEN, M-FIREFLIES and M-BIRDFLOCK (the pattern: a pure-helper header and one render-side `render::ParticleEmitter` that follows the camera, no shader, no state) and M-SEASONS (`Season`, `season_phase`, summer centre 0.375). Spring has pollen, autumn has leaves and dusk has birds, but the middle of summer has nothing alive near the ground. No Linear issues (free issue limit); this record is the tracker.

**Goal.** An opt-in kit component `Butterfly` turns the summer window (a circular trapezoid around the summer centre 0.375), daylight, calm wind and dry weather into a strength 0..1, with a manual floor for a scene that wants butterflies all year. Pure helpers turn the strength into an emitter look: a few small bright specks that drift slowly and erratically (wide spread, low speed, a little lift) in a low box around the camera. `Kit3DExtract` adds one **render-side `render::ParticleEmitter`** with the reserved id `0xFFFF0006u`, the same path rain, snow, leaves, fireflies, pollen, dust and the flock use. No new shader, pass, render field or system state; no component is written. A scene without `Butterfly` is bit-identical.

**Not in scope:** a shader or a new `render::` field (wing flapping needs per-particle animation), meshes or skinned insects, sound, butterflies that land or react to the camera, changing `Season`, `Weather` or the sun, and any stored emitter state.

## Design

- **Opt-in component, no system, no state.** `Butterfly` (`mce/kit/butterfly.h`, one per scene, the first in ECS order is read, registered by `install_weather` next to `Flock`). Authoring (all `Save`, with a `Range`): `enabled` 1; `window_full` 0.07 and `window_end` 0.14 (half-widths of the trapezoid, in season phase, around 0.375); `manual` 0 (floor); `min_daylight` 0.6; `wind_calm` 0.25 and `wind_still` 0.7 (the wind, 0..1, below which they fly fully and above which none fly); `rate` 5 per second at full strength; `box_xz_m` 18, `height_m` 3; `size_m` 0.12; `lifetime_s` 8; `speed` 0.8; `lift` 0.2; `color` (1.0, 0.75, 0.2); `max_particles` 48; `seed`; `strength` 1.
- **Strength (kit, 27a).** `butterfly_amount(const Butterfly&, const Season*, f32 phase, f32 night_fade, f32 wind, f32 rain)` is `max(manual, window(phase)) * day_gate * calm_gate * (1 - rain)` times `strength`. No `Season` gives the manual floor. NaN and inverted ranges fall back to the defaults; polynomial only, fixed evaluation order, no `rand`.
- **Emitter look (kit, 27a).** `butterfly_look(const Butterfly&, f32 amount) -> ButterflyLook`; `active = amount > 0`; slow, wide-spread, almost no gravity, alpha `0.9 * amount` fading to 0 at the end of life; not additive.
- **Extract (kit, 27b).** After the flock block in `kit3d_extract.cpp`: reads the first `Butterfly`, `Season`, `WorldClock`, the night fade, the wind and the rain; adds a local emitter via `scene.add_particle_emitter`. The Low quality tier halves `rate` and `max_particles`. Nothing is written or hashed.
- **Determinism.** The amount depends only on authoring, the clock and the weather state; a scene without `Butterfly` is byte-identical.
- **Luau (27c).** `weather.set_butterfly{...}`, getters `weather.butterfly_<key>()` (colour returns `r, g, b`) and read-only `weather.butterfly_flying()`, clamped, named errors, nothing written on error, a missing component raises. `samples/12_weather` carries a `Butterfly` with `manual` 0.4 and logs `butterfly:`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 27a. Component and pure helpers | `Butterfly`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and the `components` bump in the MCP transcript golden (the component is placed in the `12_weather` scene in the same PR), guide paragraph 9.19y, `butterfly_amount` / `butterfly_look`; tests in `tests/kit/test_butterfly.cpp` | kit, samples | Sonnet |
| 27b. Extract feed | the local emitter, Low tier halving; tests in `tests/kit/test_butterfly_extract.cpp` | kit | Sonnet |
| 27c. Luau, sample | `weather.set_butterfly`, getters, `weather.butterfly_flying()` in `definitions.cpp`; `12_weather` logs `butterfly:`; tests in `tests/script/test_script_butterfly.cpp` | script, samples | Sonnet |
| 27d. Docs and close | the `08-3d` paragraph, milestone status, follow-ups, changelog | docs | Sonnet |

No step needs Opus: nothing writes a shader.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 27a | done | 985 | `Butterfly` and the pure `butterfly_amount`, `butterfly_look` in `mce/kit/butterfly.h`, registered in `install_weather`; the `12_weather` scene carries one (manual 0.4). Guide 9.19y. |
| 27b | done | 986 | Extract feed: one local alpha-blended emitter (id `0xFFFF0006`) from `butterfly_look`, Low tier halving; `tests/kit/test_butterfly_extract.cpp`. Guide 9.19y. |
| 27c | done | 987 | `weather.set_butterfly`, the `weather.butterfly_<key>()` getters and read-only `weather.butterfly_flying()` in `api_weather.cpp`, typed in `definitions.cpp`; `test_script_butterfly.cpp`. `12_weather` logs `butterfly:` every 150 ticks. Guide 9.19y. |
| 27d | done | 988 | The `08-3d` "Butterflies flutter." paragraph; the milestone record closed with follow-ups. |

## Follow-ups

None blocks the milestone.
- The emitter box is centred on the camera (a half extent), so part of it lies below the camera; a box strictly above the ground (a ground-height query) is open.
- A real GPU/windowed look at the bright alpha-blended specks against grass and sky (size, alpha; whether they read as butterflies) and wing flapping (needs per-particle animation or a render field).
- Butterflies that land on flowers or react to the camera, and per-species colours.
- A real particle quality switch in `quality.h` (the Low tier is recognised by its settings, as for the leaves, fireflies, pollen, dust and the flock). Done by M-PARTICLEQ (`QualitySettings::particle_scale`).
