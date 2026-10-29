# M-OPENWORLD-AUDIO — the sounds of the open-world sample (E3)

**Status:** done (2026-10-29). Gap A6 and step E3 of [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md).

**Why.** `samples/09_openworld` was silent: no engine, shot, footstep, siren or ambience. The mixer, buses, 3D sources, sound sets and
pitch (M-AUDIO2) all existed, but nothing tied a car's speed to a pitch, a shot to a sound or the wanted level to a siren, and a
scene cannot do that without code.

## What was built

| Step | Piece | State | PR |
|---|---|---|---|
| 1 | CC0 sounds: engine loop, two gun shots, siren, six footsteps, city bed (`assets/audio/`, `tools/fetch_audio.py`, `SOURCES.md`, `CREDITS.md`) | done | see changelog fragment |
| 2 | `kit/world_audio.h`: `EngineAudio`, `WeaponAudio`, `FootstepAudio`, `SirenAudio` and one system, `kit_world_audio` (FixedUpdate_Post, after `audio_bridge`) | done | same |
| 3 | The scene: `AudioListener` on the camera; footsteps on the player and police; shots on the camera's and the police's `Weapon3D`; engines on the parked cars and the traffic prefab; sirens on two police; the `SFX` city bed; `AudioLimiter` and a pause `AudioSnapshot` (`gen_assets.py`) | done | same |
| 4 | Tests (`tests/kit/test_world_audio.cpp`, padding) and the user guide (10.9) | done | same |

## Decisions

- **A kit module, not a sample trick.** No existing component read `Vehicle3D.speed`, `WeaponFired3D` or `Wanted3D.level` for sound, and
  `SfxTrigger` has no weapon or footstep condition. Four small components are the smallest change that keeps the sample data-only.
  The system sets the live voice's pitch through `AudioRef::device` after `audio_bridge`; the audio module is unchanged.
- **Sound state in the hash.** The fields the system writes (`AudioSource.gain_db`, `pick_rate`, the components' `applied_*`) are a function of
  sim state, so replays match; a world without audio resources is not touched, so the headless `openworld_sample_player` hash test is unchanged by design.
- **Content outside the ledger.** `content/ledger.json` can only extract unmodified archive members, and the useful CC0 sounds are loose OpenGameArt
  files and an MP3 that needed trimming into a loop. The sample keeps the processed files and a pinned, reproducible fetch script instead.
  The engine loop and the footsteps are the unmodified originals.
- **Traffic engines.** Every traffic car carries the engine (24 at most); the mixer's 48 real voices and virtual voices decide what is heard, and
  traffic has lower priority and gain than the player's car.

## Open items and risks

- Not heard here: the cloud has no output device. The offline mixer and the unit tests cover the logic; the loudness balance (engine -10 dB, shots -2 dB,
  siren -9 dB, bed -22 dB) is a first guess and needs a listen on a machine with speakers.
- Police are on foot, so sirens come from two of them until police cars (A9, E4) exist.
- No hit or impact sound (`WeaponHit3D`), radio, music or pedestrian voices yet; the kit has no component for them.
