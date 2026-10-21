# M-WEATHERAUDIO — Rain and wind ambience beds that follow the weather (milestone record)

**Status:** planned 2026-10-21 (steps 10a to 10c; 10b is a drop-if-not-needed step). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-WEATHER, M-RAINFX (7e `ThunderDelay`) and M-WINDDIR: weather has visuals (rain, snow, ripples, splash, clouds, wind direction) and one delayed thunder one-shot, but no ambience beds, so a storm is silent apart from thunder. M-WINDDIR listed a wind audio loop as not in scope; this is that loop, kept small. No Linear issues (free issue limit); this record is the tracker. No render or shader work, so no Opus step.

**Goal.** Looping rain and wind ambience whose loudness follows `Weather` (kind, intensity, wind), fading smoothly, deterministic, saved with the scene, opt-in, and a scene without it hashes, sounds and renders exactly as today. Clips are user-supplied (`AssetRef<Sfx>` on ordinary `AudioSource` entities); a source without a clip is silent, and the engine ships no new audio assets.

**Not in scope:** new audio assets or procedural synthesis of rain and wind, changing `Weather` (its bytes stay as they are), per-material rain sounds (roof, leaves, puddles), 3D or positional beds, thunder (`ThunderDelay` stays as it is), indoor or occlusion muffling, a new audio bus or changes to `audio_bridge`, the wind direction driving panning, gust sounds tied to `Lightning`.

## Design

- **No spawned entities.** The component names the two `AudioSource` entities it drives instead of owning child entities: spawning from a system would change entity count and hashes, need a despawn story and fight the editor. The author places two ordinary looping sources (`loop`, `play_on_start`, a bus, an optional `fade_s`), points `WeatherAudio` at them, and the system only writes `AudioSource::gain_db` on them. `audio_bridge` already follows `gain_db` changes every tick (it re-sends only when the value changes), so no new audio code is needed. A null or dead entity, or one without an `AudioSource`, is skipped; a source with no clip stays silent whatever the gain.
- **State (kit).** New component `WeatherAudio` (`mce/kit/weather_audio.h`, one per scene, the first in ECS order drives, registered by `install_weather`): authoring `rain_source` and `wind_source` (`Entity`), `rain_db` 0 and `wind_db` 0 (the gain at full level, Range -60..12), `fade_seconds` 3 (Range 0..60), `enabled` 1. The per-kind rain weight (Clear 0, Overcast 0.15, Rain 1, Storm 1, Snow 0.35) is a fixed constant, not authoring, to keep the surface small. Engine-written and saved: `rain_q` and `wind_q`, the current levels as integers out of 2^24 (the same fixed-point convention as `Weather.wet_q`), explicit padding. Nothing is written to `Weather`.
- **System `kit_weather_audio`** (FixedUpdate_Pre, after `kit_weather`; step 10a decides the exact anchor by reading `install_weather`'s order). Targets from `Weather` only: rain target = `kind_gain(current)` x `intensity`, with Storm boosted by 1 (clamped), wind target = `Weather.wind` (0..1, so a calm 0.1 wind is a quiet bed). Each tick the level moves toward its target by a fixed integer step of `2^24 / max(1, fade_seconds / dt)` (a ramp, the same shape as the weather's own fade; ticks are counted, no wall clock), so the level is a pure function of saved state and the tick count. The written gain is `max_db + 20 log10(level)` floored at -60 dB, and exactly -60 for a level of 0, computed in f64 and stored as f32 (the `ThunderDelay` attenuation precedent). `NaN` authoring falls back to the defaults, ranges are clamped, fields are not rewritten.
- **Safety.** No `Weather`, no `WeatherAudio`, `enabled` 0, or no audio resources in the world (`event_queue<PlaySfx>` / `AudioRef` absent, a headless or no-audio-device run): the system returns before writing, so the world is untouched (a world without audio resources does not advance the levels either; documented in the header). The sources are never started or stopped by this system, so a Low tier or a missing device needs no branch of its own. Where gains are written but nothing plays, only `gain_db` moves.
- **Determinism and hash.** `AudioSource::gain_db` is saved authoring, so a scene with `WeatherAudio` hashes differently from one without it by design; a scene without it is byte-identical. Mid-fade save and load resumes on the same state hash because `rain_q`, `wind_q` and the authoring are all that the next tick reads. Padding is explicit and zeroed (`state_hash` hashes raw bytes) and gets a padding-audit entry.
- **Luau.** `weather.set_audio{rain_db=, wind_db=, fade=, enabled=}` and getters (`weather.rain_level()`, `weather.wind_level()`, read-only, plus the authoring getters); named errors, values clamped to the Range, nothing written on error, typed in `definitions.cpp`. `samples/12_weather` gets a `WeatherAudio` and two clip-less looping `AudioSource` entities, so the sample stays silent and deterministic (the entity count and the golden/log expectations are updated in the step, nothing else moves).

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 10a. Kit state and system | `WeatherAudio`, `kit_weather_audio`, `install_weather` registration, padding audit, tests (levels follow kind, intensity and wind; fade is a ramp; clamps and NaN; no-clip, dead and non-source entities; save/load mid-fade; no component = unchanged hash) | kit, tests/kit | Sonnet |
| 10b. Wiring and quality (drop if not needed) | Only if 10a's review finds that `audio_bridge` does not pick up a `gain_db` change on a looping source in a pure sim run or that the system order needs a seam in a shared header; otherwise this step is dropped and 10c becomes the last step | kit, audio | Sonnet |
| 10c. Luau, sample, docs | `weather.set_audio` and getters, `samples/12_weather`, guide, changelog | script, samples | Sonnet |
| 10d. Close | Step status, ROADMAP row to done, `capabilities.md` note for the integration runner (generated files are not edited) | docs | Sonnet |

## Gates
- A scene without `WeatherAudio` (or without `Weather`) has an unchanged state hash and unchanged frames; every existing sample hash and golden stays byte-identical.
- Save and load in the middle of a fade resume on the same state hash, and N ticks after the load equal N ticks without it.
- A Low quality tier and a world with no audio device or no `AudioRef` run the system without error and without writing; null clips stay silent.
- Kit: the only engine-written state is `rain_q` and `wind_q` (integers); `Weather` bytes are not touched (a test hashes the `Weather` column with and without `WeatherAudio`).
- Luau: setters clamp, bad types raise named errors and write nothing; `samples/12_weather` stays deterministic across two runs.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 10a | done | TBD | `WeatherAudio` + `kit_weather_audio` (after `kit_weather`, installed by `install_weather`); tests in tests/kit/test_weather_audio.cpp. The no-audio rule is `event_queue<audio::PlaySfx>()` or an `audio::AudioRef` resource present (documented in the header). `AudioSource` is not `Save`, so right after a load the source gains are the scene's; the next tick recomputes them from the saved levels, and the hashes match from then on. |
| 10b | dropped | | Read `audio_bridge`: for a started source with a live handle it compares the faded gain with `applied_gain_db` every tick and calls `set_handle_gain` when it changed, looping or not, with no device-dependent branch; the order needs no seam in a shared header (the system anchors on `kit_weather`). |
| 10c | planned | | |
| 10d | planned | | |
