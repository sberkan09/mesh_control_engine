# Sound sources (09_openworld)

Every sound is CC0 (public domain) from OpenGameArt, read 2026-10-29. `tools/fetch_audio.py` downloads the originals, checks their
SHA-256 (the pins are in the script) and writes these files; run it again after changing a rule there.

| File here | Origin | Author | Source page | Change |
|---|---|---|---|---|
| `engine_loop.wav` | `loop_0.wav` | qubodup | https://opengameart.org/content/racing-car-engine-sound-loops | none (the pitch follows the car's speed at run time) |
| `shot_1.wav` | `22 Pistol.wav` | LarkPay | https://opengameart.org/content/gunshots | first 0.9 s, mono 44.1 kHz, peak -1 dBFS, 150 ms fade out |
| `shot_2.wav` | `22 Magnum.wav` | LarkPay | https://opengameart.org/content/gunshots | first 1.1 s, same |
| `siren.wav` | `siren.mp3` | looneybits | https://opengameart.org/content/sirens-and-alarm-noise | 8 whole wail periods (5.5 s) cut where the pitch falls back, mono 44.1 kHz, 5 ms join |
| `footstep_1..6.ogg` | `01-footstep.ogg` .. `06-footstep.ogg` | GboxMikeFozzy | https://opengameart.org/content/footsteps-0 | none |
| `city_ambience.wav` | `crowd_shouting.ogg` | StarNinjas | https://opengameart.org/content/crowd-shoutingspeaking-ambience | first 14 s with a 1.5 s crossfade into the start (a seamless loop), mono 44.1 kHz |
| `hit_1..5.ogg` | `metal_hit_01..05.ogg` | rubberduck | https://opengameart.org/content/100-cc0-metal-and-wood-sfx | none (taken from the pack's zip) |
| `crash_1..3.ogg` | `metal_slam_01.ogg`, `metal_falling_01.ogg`, `metal_falling_02.ogg` | rubberduck | same | none |
| `rain_loop.wav` | `amb_rain_loop_1.ogg` | kresiek-the-furry | https://opengameart.org/content/amb-rain-loop-1 | first 16 s with a 2 s crossfade (a loop), mono 22.05 kHz |
| `wind_loop.wav` | `wind background noise 2.wav` | bashar3a | https://opengameart.org/content/mild-wind-background-noise | first 16 s with a 2 s crossfade, mono 22.05 kHz |
| `night_crickets.wav` | `crickets_1.mp3` | wolfgang | https://opengameart.org/content/crickets-ambient-noise-loopable | first 10 s with a 1.4 s crossfade, mono 22.05 kHz |

The authors ask for nothing under CC0; StarNinjas would like a link to their OpenGameArt profile (https://opengameart.org/users/starninjas)
and to hear where the sound is used, so it is credited in `CREDITS.md`. Cooked sidecars (`*.mceasset`) and the two sound sets
(`footsteps.mcsound`, `shots.mcsound`) are written by `gen_assets.py`.
