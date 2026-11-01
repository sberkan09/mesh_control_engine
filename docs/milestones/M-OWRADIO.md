# M-OWRADIO: car radio in 09_openworld (2026-10-31)

Data only (design: section L of [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md)): two looping stations ("Radio 1" pop, "Radio 2" driving) on the `Music` bus play muted all the time; `assets/scripts/radio.luau` keeps the dial in `ScriptVars.v[1]` of "Script: radio" (saved, `SaveKey` 998) and, while the player is seated in a car, Q (`Radio` action) cycles station 1, station 2, off by setting `AudioSource.gain_db` (-8 on, -60 muted). The tracks are synthesized by `gen_assets.py` (`music_wav`, stdlib only, fixed seed, so reruns write the same bytes); no source files, no licence.

Changed from the design: two stations and no on-foot bed, because `Music` streams two clips at a time and a third logs a warning that `test_mcp_openworld` rejects.

Tests: `test_mcp_openworld` (one `ScriptSystem`; seated + Q gives station 1 at -8 dB and station 2 muted; three presses end off; on foot Q does nothing).

Limits: no station name on the HUD, one dial for every car, no duck under voice lines. Gamepad: D-pad left (added with M-GAMEPAD).
