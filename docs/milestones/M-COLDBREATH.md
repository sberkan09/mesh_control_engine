# M-COLDBREATH — Cold breath: small pale vapour puffs in front of the camera in winter cold and over settled snow, with no shader (milestone record)

**Status:** planned (2026-11-03), steps 34a to 34d. PRs: this plan. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-MIDGES, M-SNOWSPARKLE and M-BUTTERFLY (the pattern: a pure-helper header and one render-side `render::ParticleEmitter` that follows the camera, no shader, no state), M-SEASONS (`Season`, winter centre 0.875), M-SNOWCOVER (`SnowCover.cover`) and M-FREEZE, which listed "steam or breath particles" as not in scope. Winter now has snow, frozen water and glints but nothing that says the air is cold; real cold air shows the breath. No Linear issues (free issue limit); this record is the tracker.

**Naming:** the component is `ColdBreath` (header `cold_breath.h`); `projects/`, `src/` and `docs/` have no type of that name (checked 2026-11-03).

**Goal.** An opt-in kit component `ColdBreath` turns a winter window (a circular trapezoid around the winter centre 0.875), the settled snow cover and calm wind into a strength 0..1, with a manual floor for a scene that wants visible breath all year. Pure helpers turn it into an emitter look: a few soft, pale, slowly growing puffs that leave a point just in front of and below the camera (the mouth of a first-person view), drift forward and up and fade out in about a second. `Kit3DExtract` adds one **render-side `render::ParticleEmitter`** with the reserved id `0xFFFF000Au`, the same path as the other nine. No shader, pass, render field or system state; no component is written. A scene with no `ColdBreath` (or `enabled` 0, `strength` 0, or an amount of 0) is bit-identical.

**Not in scope:** a shader or a new `render::` field, breath tied to a character's head or to breathing animation (the point follows the camera, so a third-person camera shows the puffs in front of the lens; an entity anchor is a follow-up), breath rate that follows exertion, steam from vents or animals, sound, a temperature model (the drivers are the season, the snow cover and the wind), and changing `Season`, `SnowCover`, `Weather` or the sun.

## Design

- **Opt-in component, no system, no state.** `ColdBreath` (`mce/kit/cold_breath.h`, one per scene, the first in ECS order is read, registered by `install_weather` next to `Midge`). Authoring (all `Save`, with a `Range`): `enabled` 1; `window_full` 0.08 and `window_end` 0.16 (half-widths in season phase around 0.875); `manual` 0 (floor, ignores the window and cover but not the wind gate); `cover_low` 0.15 and `cover_full` 0.6 (settled snow cover that also brings breath, smoothed); `wind_calm` 0.5 and `wind_still` 0.95 (above which the wind scatters the vapour); `rate` 0.8 puffs per second at full strength; `mouth_forward_m` 0.35, `mouth_down_m` 0.25; `size_m` 0.12 growing to `grow` 2.5 times; `lifetime_s` 1.2; `speed` 0.35; `lift` 0.15; `color` (0.9, 0.93, 0.97); `max_particles` 24; `seed`; `strength` 1.
- **Strength (kit, 34a).** `cold_breath_amount(const ColdBreath&, const Season*, f32 phase, f32 cover, f32 wind)` is `max(manual, window(phase), cover_gate(cover)) * calm_gate(wind)` times `strength`. No `Season` and no cover give the manual floor. NaN and inverted ranges fall back to the defaults; polynomial only, fixed evaluation order, no `rand`.
- **Emitter look (kit, 34a).** `cold_breath_look(const ColdBreath&, f32 amount) -> ColdBreathLook`; `active = amount > 0`; low speed, a little lift, drag, alpha `0.35 * amount` fading to 0 at the end of life, size growing from `size_m` to `size_m * grow`; alpha-blended, not additive.
- **Extract (kit, 34b).** After the midge block in `kit3d_extract.cpp`: reads the first `ColdBreath`, `Season`, `WorldClock`, the first `SnowCover`, the wind (as the midge block does); the emitter is a point (a very small box) at `view.position + forward * mouth_forward_m - up * mouth_down_m`, direction the view's forward with a small spread; rate and budget scaled by `particle_scale_of(quality_)` / `scaled_budget`. Nothing is written or hashed.
- **Determinism.** The amount depends only on authoring, the clock, the snow cover and the weather state; a scene without `ColdBreath` is byte-identical.
- **Luau (34c).** `weather.set_cold_breath{...}`, getters `weather.cold_breath_<key>()` (colour returns `r, g, b`) and read-only `weather.cold_breath_active()`, clamped, named errors, nothing written on error, a missing component raises. `samples/12_weather` carries a `ColdBreath` with `manual` 0.3 and logs `cold_breath:`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 34a. Component and pure helpers | `ColdBreath`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and the `components` bump in the MCP transcript golden, scene placement in `12_weather`, guide paragraph; `tests/kit/test_cold_breath.cpp` | kit, samples | Sonnet |
| 34b. Extract feed | the local emitter (id `0xFFFF000A`) at the mouth point, Low tier and `particle_scale`; `tests/kit/test_cold_breath_extract.cpp` | kit | Sonnet |
| 34c. Luau, sample | `weather.set_cold_breath`, getters, `weather.cold_breath_active()`; `12_weather` logs `cold_breath:`; `tests/script/test_script_cold_breath.cpp` | script, samples | Sonnet |
| 34d. Docs and close | the `08-3d` paragraph, milestone status, follow-ups, ROADMAP row, changelog | docs | Sonnet |

No step needs Opus: nothing writes a shader.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 34a | planned | | |
| 34b | planned | | |
| 34c | planned | | |
| 34d | planned | | |

## Follow-ups

- Anchor the puff to an entity's head (a `Head` or mouth offset) instead of the camera, so a third-person view shows breath at the character.
- A real GPU/windowed look at the soft puffs (size, alpha, whether they read as vapour).
