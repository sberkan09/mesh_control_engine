# M-OPENWORLD-DAYNIGHT — day and night, sky, street lamps and weather for the open-world sample (E6)

**Status:** done (2026-10-29), gaps A4 and A5 of [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md). Data and one small audio component; no render code
(the Aurora threads own `scene3d` and `Kit3DExtract`, untouched).

## What was built (`samples/09_openworld`, all from `gen_assets.py`)

| Piece | How |
|---|---|
| Clock | `WorldClock` (starts at 09:00, 720 s per day) with the default `DayCycle`: sun, ambient, fog and exposure follow the hour |
| Sky | `Environment3D.sky_enabled 0`: the sky clears to the fog colour, which the day cycle darkens at night, plus the `Moon` (stars, moonlight) and a `CloudLayer` |
| Street lamps | 50: two opposite corners of each of the 25 junction pads, a dark post and an emissive head (`materials/lamp.mcmat`) with a `PointLight3D` and a `TimedLight3D` (on from about 18:30, off at 05:45, 20 minute fades) |
| Weather | `Weather` with a seeded schedule (`auto_change_seconds` 150, seed 55: clear, rain, clear, storm, overcast, ... no snow in the first ten picks) and `WindGust`, `GroundMist`, `WetSurfaces`, `RainRipples`, `WindDirection`, `ThunderDelay`; `Precipitation3D` rain, `RainSplash`, `Lightning` with `LightningBolt` (storms only) |
| Sound | `WeatherAudio` drives a rain bed and a wind bed (CC0 loops, 22.05 kHz mono, Vorbis in the pack); the new `ClockAudio` (`kit/world_audio.h`, user guide 10.9) raises a crickets bed after 19:30 and thins the city bed at night |

## Decisions and findings

- **`atmosphere` is off.** The physically based sky is the nicer daytime sky, but together with `DayCycle` (which also writes sun colour and ambient) the
  day was dim and grey, and without `DayCycle` the dusk and night were black. A flat fog-coloured sky with the moon, stars and lamps looks consistent at every
  hour. **Update 2026-10-31:** done in [M-OPENWORLD-LIGHT](M-OPENWORLD-LIGHT.md) (atmosphere on, the DayCycle owns the ambient, the atmosphere yields to the moon). Old open item: tune a `DayCycle` preset for the atmosphere (sun colour is then the light above the air) or drive the atmosphere's own ambient; that touches the render path
  (Aurora) and was left alone.
- **Hard black shadows** (from buildings at a low sun) are the engine's shadow ambient, not a change here; they show in the baseline too.
- **Snow.** The seeded schedule picks among all five weather kinds; seed 55 has no snow in its first ten picks (25 minutes), `Precipitation3D.snow_rate` is 0 and
  there is no `SnowCover`, so a later snow pick would only grey the sky. A schedule that can exclude kinds would need a kit change.
- **Lamps are 50 point lights.** Look at the frame time on a real GPU (not measured here, llvmpipe only); lowering `radius` or keeping lamps to the inner junctions is the cheap fix.
- ~~No thunder sound and no headlights~~ done in E7, see [M-OPENWORLD-LOOK](M-OPENWORLD-LOOK.md).

## Verification

- Screenshots on llvmpipe at 09:00, 19:10, 22:00 and 23:00 (with and without lamps), rain and storm at 14:00 and 21:00: sky, stars, lamp pools, wet roads, ripples, mist and rain.
- `test_world_audio` (`ClockAudio`: rises after `on_hour`, falls after `off_hour`, wraps midnight), `openworld_sample_player` (hash replay), `--premerge`.
- Not verified: how the beds sound, the frame time with 50 lights on a real GPU.
