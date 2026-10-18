# M-WEATHER — Weather as data (milestone record)

**Status:** done 2026-10-18 (step 4 implemented, not merged). **Owner: routine 2 thread (the M-VISUAL line).** Taken after M-DAYNIGHT, which listed weather as out of scope. No Linear issues (free issue limit); this record is the tracker.
Sources: `M-DAYNIGHT.md` ("Not in scope: weather"), `assessments/gta-benzeri-oyun-fizibilitesi-2026-10-17.md` (open-world feel), `assessments/3d-gorsellik-degerlendirmesi.md`.

**Goal.** A game author (or an agent over MCP) sets a weather state and gets the sky, fog, light, wind, rain or snow and lightning that go with it, deterministically and saved with the scene. No per-tick script.

**Not in scope:** clouds as geometry or volumetrics, seasons, wet-surface shading (a render change: roughness and puddles; a possible step 5 that needs an Opus-reviewed shader plan), interior/exterior masks, weather in the net replication (the state is in components, so a host that replicates components gets it).

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 1. Weather state | `Weather` component (one per scene: `target` kind clear/overcast/rain/storm/snow, `transition_seconds`, `intensity` 0..1, `wind`, `wetness`), fixed-point blending, optional seeded random schedule (`auto_change_minutes`, `seed`); event `WeatherChanged`; system after `kit_day_cycle` | kit (new `weather.h/.cpp`) | Sonnet |
| 2. Weather to environment | the system scales sun intensity, ambient, exposure and fog density by kind and intensity on the `Environment3D` resource (after the day cycle, so it stacks, with no drift: the component keeps the base and the last written values); wind stays exposed as `Weather.wind` for scripts and step 3 | kit | Sonnet |
| 3. Precipitation and lightning | `PrecipitationEmitter3D` follows the primary camera and drives a `ParticleEmitter3D` (rate, gravity, speed, colour from kind and intensity; snow vs rain); lightning (seeded flashes in storms: an ambient/exposure spike over a few ticks) and a `ThunderStruck` event for audio | kit | Sonnet |
| 4. Luau, MCP, demo | `weather.set/get/intensity/wind` in Luau, `samples/12_weather` (headless numbers per kind, GPU smoke at one frame), milestone closed | script, samples | Sonnet |

## Gates
- Every step: `local_ci.sh --task` green, headless test with numbers, replay hash stable, scenes without `Weather` unchanged (opt-in).
- State in components (save, replay and the editor see it); no hidden global state. No edit to `kit3d.h` (116 TUs).
- GPU smoke tests are one short frame (see the M-GPU-VERIFY note on long captures).

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | done | #575 | |
| 2 | done | #577 | |
| 3 | done | #581 | needs 1; `Precipitation3D` + `Lightning` in `precipitation.h`, `LightningStruck` event (named `PrecipitationEmitter3D` and `ThunderStruck` in the plan) |
| 4 | implemented (not merged) | | `weather.*` in Luau (`api_weather.cpp`); `samples/12_weather` (tests `test_weather_demo.cpp`, `sample_weather_headless_600`, slow `sample_weather_gpu` at tick 1500: a storm at tick 700 does not exist with 10 s transitions, the rain fades out first); `Weather`, `Precipitation3D` and `Lightning` left `sample_coverage.allow`. The demo's emitter is spawned from the cooked scene and its state hash is stable (checked over repeated player runs) |

## Open items

- **`ParticleEmitter3D` tail padding is uninitialised** (found in step 3): the struct in `kit3d.h` has 4 bytes of tail padding that stay uninitialised when an emitter is spawned from a temporary, and `World::state_hash` reads them, so a scene holding an emitter can get a flaky state hash. The weather tests spawn the emitter over zeroed storage. The fix is an explicit `u32 pad2_ = 0;` in `kit3d.h`, a 116-TU header, so it ships as its own seam PR (not part of M-WEATHER).
