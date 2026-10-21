# M-RAINFX — Rain you can see land (milestone record)

**Status:** done 2026-10-21 (steps 7a to 7f). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-WEATHER and M-WETSURFACE: rain falls and the ground gets wet and puddled, but nothing happens where a drop lands and thunder is only an event. No Linear issues (free issue limit); this record is the tracker. Render steps (7b) are Opus-written and merged after an independent Opus review plus a green `--task` (Sami, 2026-10-20: "İzin veriyorum sormayın bana").

**Goal.** While it rains, puddles ripple and drops splash where they land, thunder reaches the listener after a delay that depends on how far the strike was, all deterministic, saved with the scene, opt-in, and a scene without the new components renders and sounds exactly as today.

**Not in scope:** rain occlusion under roofs (needs a top-down depth mask, same as M-WETSURFACE), rain on the camera lens, droplets running down surfaces, water volumes and real ocean/river surfaces, a new audio mixer feature (thunder uses the existing one-shot path).

## Design

- **Ripples (render).** `render::Ripples {strength, density, speed, ring_width, enabled, pad_}` in `scene3d.h` next to `WetSurfaces` (seam PR first), `QualitySettings::rain_ripples` and cvar `r.rain_ripples` (Low drops them), by-value `apply_quality` overload with clamping. Shader: one `vec4 p_ripple` (strength, density, 1/speed scale, time) in `Forward3DPassU` and `MCE_GLSL_PASS_U`; GLSL after `apply_wet`, behind `p_ripple.x > 0.0` (so a dry or ripple-less frame is bit-identical): concentric rings from an integer-hash cell grid (periodic over 256 m like the puddles), each cell a drop with a hashed phase, a normal perturbation inside puddles only. Time is the simulation tick (a pass uniform), never the wall clock, so goldens are stable.
- **Ripple state (kit).** Component `RainRipples` {strength 1, density 0.5, speed 1, ring_width 0.15}, registered by `install_weather`; `Kit3DExtract::apply_environment` builds the render struct from `Weather.intensity` and `wetness` and only when `Weather`, `WetSurfaces` and `RainRipples` exist and it rains.
- **Splashes (kit).** Component `RainSplash` {rate 1, height_m 0.15, life_seconds 0.25, radius_m 25}: a `kit_rain_splash` system after `kit_precipitation` drives a ring of `ParticleEmitter3D` bursts around the primary camera at ground height (seeded, tick-quantized), scaled by `Weather.intensity`; no per-drop physics.
- **Thunder (kit + audio).** The `LightningStruck` event already exists. Component `ThunderDelay` {speed_mps 343, volume 1, clip}: a system turns each strike into a `PlaySfx` that starts `distance / speed_mps` seconds later (tick-quantized), attenuated by distance; the listener is the primary camera.
- **Luau.** `weather.set_ripples{}`, `weather.set_splash{}`, `weather.set_thunder{}` and their getters; `samples/12_weather` gets all three.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 7a. Render data | `render::Ripples` POD, `set_ripples()`, `QualitySettings::rain_ripples`, `r.rain_ripples`, `apply_quality` overload with clamping, test | render, tests/render | Sonnet; seam PR for `scene3d.h` |
| 7b. Ripple shader | `p_ripple`, GLSL, `ripple_plaza` golden and pixel tests, all goldens byte-identical | render, tests/render_gl | **Opus writes**, independent Opus review |
| 7c. Kit | `RainRipples`, `RainSplash`, `ThunderDelay` components and systems, extract, determinism and save tests, padding audit | kit, tests/kit | Sonnet |
| 7d. Luau, sample, docs | the three setters, `samples/12_weather`, guide, close | script, samples | Sonnet |
| 7e. Thunder | `ThunderDelay` component and `kit_thunder_delay` (7c split: ThunderDelay only), tests, padding audit, guide 9.19a | kit, tests/kit | Sonnet |
| 7f. Splash | `RainSplash` and `kit_rain_splash` (the rest of 7c) | kit, tests/kit | Sonnet |

7c does not depend on 7b and can run in parallel with it after 7a.

## Gates
- A scene without the components (or without `Weather`/`WetSurfaces`) gives `Scene3D::ripples()` with strength 0 and an unchanged state hash; every existing golden under `tests/render_gl/*` stays byte-identical (compare the raw readback), including `wet_plaza` and `snow_plaza`.
- New golden `ripple_plaza` with pixel tests: ripples change only puddle pixels, a dry frame is identical, Low equals the no-ripple frame, an origin shift of 1000 m moves the frame by less than the baseline plus 0.1%.
- Kit: all new state is integer or fixed-point and saved; a save/load mid-storm resumes with the same state hash; a thunder strike at distance d starts its sound exactly ceil(d / speed / dt) ticks later; a clear scene spawns nothing.
- Carry-overs from the M-WETSURFACE and M-SNOWCOVER reviews that touch the same code are folded in: share `unit01` between `quality.cpp` and `forward3d.cpp`; measure `kMeshFS` register pressure on a device when one is available (open since 5b).

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 7a | done | #640 | `render::Ripples`, `r.rain_ripples`, tier field, `apply_quality` overload |
| 7b | done | #645 | `p_ripple`, `apply_ripples` on the puddle mask apply_wet returns (snow stills it), `ripple_plaza` golden, tests/render_gl/test_render3d_ripple_gl.cpp; clock = `Scene3D::time()`; `unit01` shared (`src/engine/render/src/unit_clamp.h`) |
| 7c | done | #648 | `RainRipples` + extract only (7e is thunder, 7f splash) |
| 7d | done | | `weather.ripples_*`, `splash_*`, `thunder_*` getters and `weather.set_ripples{...}`, `set_splash{...}`, `set_thunder{...}` (clamped to the Range, named errors, nothing written on error) in `api_weather.cpp`, typed in `definitions.cpp`; `samples/12_weather`: `RainRipples` and `ThunderDelay` (no clip: the sample has no sfx asset, so no thunder sounds) on the Weather entity, `RainSplash` on a new `Splash` entity with a `ParticleEmitter3D` (entity count 11 to 12) |
| 7e | done | #658 | `ThunderDelay` only (component, `kit_thunder_delay`, tests/kit/test_thunder_delay.cpp, padding audit, guide 9.19a); the strike distance is derived from `LightningStruck.thunder_delay_ticks` (the event has no position) |
| 7f | done | #659 | `RainSplash` (component with `seed` and a saved `counter`, `kit_rain_splash`, tests/kit/test_rain_splash.cpp, padding audit, guide 9.19c); one burst per tick on the author's own `ParticleEmitter3D`, no pool entities; ground y = the entity's own y |

## Notes for 7b from the 7a review (independent Opus review, no blockers)

- `apply_quality(q, Ripples)` passes `enabled` through; it is not the off switch. 7b's upload must treat `strength == 0 || !enabled` as off (no `p_ripple`), so a "disabled" carrier never draws.
- Ripples are not tied to the wet gates in the carrier: with `r.wet_surfaces 0` or `puddles_enabled` false, strength stays non-zero. The shader must multiply by the puddle mask (which is 0 when wetness or puddles are off) so the dry-frame gate holds.
- `ring_width` and `density` may be 0 after clamping: guard any `smoothstep` width or divisor (floor at 0.01 in the upload).
- Share the `unit` clamp helper between `quality.cpp` (three copies now) and `forward3d.cpp` while in this code (carry-over from M-WETSURFACE).
- `r.rain_ripples` is a main-thread cvar like the others; keep `apply_quality` in the main-thread extract.
