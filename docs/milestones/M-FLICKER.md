# M-FLICKER — Light flicker: fire, candle, torch and failing-lamp lights that waver in brightness, render-side only, with no shader (milestone record)

**Status:** planned (2026-11-03), steps 36a to 36d. PRs: this plan. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-DAYNIGHT (`TimedLight3D`: lamps fade on at dusk, but a lit lamp is perfectly steady), the 3D visual assessment (gap: "lights are flat and clean, nothing says fire or a bad bulb") and the `kit3d_tour` fire emitter, whose light would be a steady point light today. No Linear issues (free issue limit, Linear unreachable); this record is the tracker.

**Naming:** the component is `LightFlicker3D` (header `light_flicker.h`); `projects/`, `src/`, `docs/` and `tests/` have no type of that name (checked 2026-11-03).

**Goal.** An opt-in kit component `LightFlicker3D` goes on an entity that has a `PointLight3D` and/or a `SpotLight3D` and makes the light's brightness waver. Pure helpers turn the render clock (`frame_time`, the value `time_ + frame_dt_` the aurora and meteor blocks compute) and a per-entity seed into a multiplier around 1: a fire style (two or three incommensurate noise octaves, `depth` 0..1 of the intensity), a failing-lamp style (mostly steady with short dropouts) and a pulse style (a slow sine). `Kit3DExtract` multiplies the intensity of the light it adds to the scene (local `render::PointLight`), and may nudge the radius a little (`radius_depth`) so the pool of light breathes. **Render-side only: nothing is written to a component and nothing is hashed**, so the simulation, saves and replays are unchanged. A light with no `LightFlicker3D` (or `enabled` 0, or `depth` 0) is bit-identical.

**Not in scope:** a shader or a new `render::` field, flicker of emissive materials (the lamp mesh stays steady; a follow-up can drive `MeshRef.emissive_tint` the same way), colour shifts, shadow flicker beyond what the intensity change gives, an audio crackle, and flicker that gameplay can read (it is a pure look; a gameplay-visible light level would be a system with state).

## Design

- **Opt-in component, no system, no state.** `LightFlicker3D` (`mce/kit/light_flicker.h`, any number of entities, registered by `install_kit3d` next to `PointLight3D`). Authoring (all `Save`, with a `Range`): `enabled` 1; `style` (0 fire, 1 failing lamp, 2 pulse); `depth` 0..1 (how far the intensity may drop, 0 = off); `rate_hz` 0.1..30 (the main noise rate); `radius_depth` 0..0.5; `seed`; `phase` for the pulse style; zeroed `pad_`.
- **Pure helpers (36a).** `light_flicker_scale(const LightFlicker3D&, f64 time_s, u32 entity_salt) -> f32` in `(0, 1]`-ish, `1 - depth * x` with `x` in 0..1 from the style; polynomial and integer-hash value noise only, fixed evaluation order, no `rand`, NaN and out-of-range inputs fall back to the defaults; `light_flicker_radius_scale(...)` the same for the radius. The salt is the entity index so two lamps with the same seed still do not flicker in step.
- **Extract (36b).** In the `PointLight3D` and `SpotLight3D` loops of `kit3d_extract.cpp`: if the entity also has a `LightFlicker3D` (a component lookup), multiply `pl.intensity` and `pl.radius` by the helpers; culling by `in_view` uses the authored radius. Counted in the existing light stats.
- **Sample (36c).** `samples/kit3d_tour` gets a fire light ("Campfire light": a `PointLight3D` and a fire-style `LightFlicker3D`) at the existing `Fire` emitter at (-5, 0.2, 2); the tour's entity pin goes from 22 to 23. No Luau API: the component has no runtime state to script.
- **Determinism.** The scale depends on `(seed, entity, frame_time)` only; it never feeds the simulation or the state hash.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 36a. Component and pure helpers | `LightFlicker3D`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row (a sample allow-line until 36c) and the `components` bump in the MCP transcript golden, guide bullet; `tests/kit/test_light_flicker.cpp` | kit | Sonnet |
| 36b. Extract feed | the intensity and radius scale in the point and spot light loops; `tests/kit/test_light_flicker_extract.cpp` | kit | Sonnet |
| 36c. Sample | the tour's campfire light, entity pin 23, allow-line removed | samples | Sonnet |
| 36d. Docs and close | the `08-3d` paragraph, milestone status, follow-ups, ROADMAP row, changelog | docs | Sonnet |

No step needs Opus: nothing writes a shader; the light only changes numbers the renderer already takes.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 36a | planned | | |
| 36b | planned | | |
| 36c | planned | | |
| 36d | planned | | |

## Follow-ups

- Drive the lamp mesh's `emissive_tint` with the same scale so the bulb dims with its light.
- A tint shift towards orange at the low end of the fire style.
- A real GPU/windowed look at the depth and rate defaults.
