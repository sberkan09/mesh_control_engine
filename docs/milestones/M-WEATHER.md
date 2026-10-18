# M-WEATHER — Weather as data (milestone record)

**Status:** planned 2026-10-18. **Owner: routine 2 thread (the M-VISUAL line).** Taken after M-DAYNIGHT, which listed weather as out of scope. No Linear issues (free issue limit); this record is the tracker.
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
| 3 | implemented (not merged) | | needs 1; `Precipitation3D` + `Lightning` in `precipitation.h`, `LightningStruck` event (named `PrecipitationEmitter3D` and `ThunderStruck` in the plan) |
| 4 | planned | | |
