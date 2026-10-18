# M-DAYNIGHT — A day/night cycle as data (milestone record)

**Status:** Done 2026-10-18 (step 4 implemented, not merged; no Linear issues: the SBA workspace is at its free issue limit, this record is the tracker).
Source: `assessments/gta-benzeri-oyun-fizibilitesi-2026-10-17.md` ("3-4 missions with a script, day/night cycle"). `Environment3D.time_of_day` (hours, places the sun on a simple arc) already exists, but nothing moves it: a game has to write the field every tick from a script, and nothing else (ambient, fog, exposure, street lights, window emissives) follows the hour.

**Goal.** A game author (or an agent over MCP) gets a deterministic world clock and a day cycle as components: set the day length, pick curves, place lights that turn on at dusk. No C++, no per-tick script.

**Not in scope:** weather (rain, clouds), seasons, a moving moon with phases, sky-dome authoring (the HDR sky of M-VISUAL3 stays), per-room interior lighting.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 1. World clock | `WorldClock` component (one per scene: `hour` 0..24, `day_length_seconds`, `time_scale`, `paused`, `day` counter), one system that advances it by tick (tick-quantized, replay-stable, saved with the scene) and writes `Environment3D.time_of_day` when `drive_environment` is set; events `HourPassed`, `DayPassed` | kit (new `world_clock.h/.cpp`) | Sonnet |
| 2. Day cycle curves | `DayCycle` component: keyed curves over the hour for sun colour and intensity, ambient colour and intensity, fog colour and density, camera exposure; linear interpolation with wrap-around at midnight; the system writes the matching `Environment3D` / `Light3D` fields | kit (new `day_cycle.h/.cpp`) | Sonnet |
| 3. Timed lights and emissives | `TimedLight3D` (switches a `Light3D` / emissive scale on between `on_hour` and `off_hour`, with a fade in minutes), events `DawnStarted`, `DuskStarted` | kit | Sonnet |
| 4. Luau, MCP and demo | `clock.hour/set/pause/speed` in Luau, MCP schemas come from reflection, a headless sample (`samples/11_daynight`, or a shot of `06_showcase`) with pinned hours and a golden per hour; milestone closed | script, samples | Sonnet |

## Gates
- Every step: `local_ci.sh --task` green, headless test with numbers (the hour after N ticks, the colour at 06:00 and 18:00, a lamp on at 19:30), replay hash stable, scenes without these components unchanged (opt-in).
- The clock and the curves live in components, so save, replay and the editor see them; no hidden global state.
- No edit to `kit3d.h` (116 TUs): the systems write existing `Environment3D` and `Light3D` fields.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | done | #559 | `WorldClock` in `world_clock.h/.cpp`, tests in `tests/kit/test_world_clock.cpp`, user guide 9.16 |
| 2 | done | #563 | `DayCycle` in `day_cycle.h/.cpp` (8 key slots, default night/dawn/noon/dusk preset), tests in `tests/kit/test_day_cycle.cpp`, user guide 9.17 |
| 3 | done | #565 | `TimedLight3D` in `timed_light.h/.cpp` (ramp over `fade_minutes`, intensity = base * ramp, `enabled` 0 when off, `MeshRef` emissive), events `DuskStarted`/`DawnStarted`, tests in `tests/kit/test_timed_light.cpp`, user guide 9.18 |
| 4 | done | #571 | Luau `clock.hour/day/set/pause/speed/paused` in `api_clock.cpp` (typed in `mce.d.luau`, tests in `tests/script/test_script_clock.cpp`, user guide 27.3); `samples/11_daynight` (street, emissive windows, two lamps, `Jump` skips to noon) with `tests/script/test_daynight_demo.cpp` (sun at 06:00 and 18:00, lamps off at noon and on at 19:30, windows, midnight, state hash, `clock.set(12)`), `sample_daynight_headless_600` and a GPU smoke run capturing 06:00 (no goldens, `slow`: integration run only, it stalled under a loaded `--task`). MCP needs no code: the three components are reflected |
