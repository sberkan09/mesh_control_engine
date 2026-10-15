# M-STEAM — Steamworks (milestone record)

**Status:** active. Step 1 in review; steps 2–6 to come (Linear SBA-106 … SBA-111; SBA-112 is the run against
the real Steam client, which needs the user's Steamworks account).
**Why:** Steam is the desktop distribution channel; the engine comparison (the project's
`assessments/motor-kiyaslama-eksikler.md`, item 2) lists it right after the web. Added 2026-10-15 under the
user's standing instruction to extend the roadmap when it runs out.
**Design:** ADR-0042 (the Steamworks flat API opened at run time; no SDK in the repo or the build).
**Measured on:** the Linux CI container, against a stand-in `steam_api`.

## Steps

| Step | Linear | State |
|---|---|---|
| 1. ADR-0042 and the run-time loader: `mce/platform/steam.h`, init and its refusals, manual-dispatch callbacks, the player's `-steam.app_id`, a stand-in library for tests, the Steam wall | SBA-106 | in review |
| 2. Achievements, stats, leaderboards and cloud saves as `platform::game_services` `Backend::Steam` under `liveops::PlatformGameServices` | SBA-107 | — |
| 3. Overlay (holds the world like a lost focus) and rich presence as data | SBA-108 | — |
| 4. Steam Input: an action manifest from the cooked input map, glyphs, Steam Deck defaults | SBA-109 | — |
| 5. `ship --steam`: depots per platform, `steam_api` beside the binary, generated steamcmd build scripts | SBA-110 | — |
| 6. Demo, user guide, gates | SBA-111 | — |
| Later: the real Steam client (achievements configured, overlay, Steam Input, a beta-branch upload) | SBA-112 | needs the user's account |

## Step 1 — the run-time loader

| Item | Where | Evidence |
|---|---|---|
| `platform::steam::init` opens `steam_api` (the `steam.library` path or the platform's file name next to the executable), resolves the flat API by name and initialises Steam with manual dispatch; `NoLibrary`, `MissingEntry`, `NoSteamClient`, `VersionMismatch`, `Failed` and `Restarting` leave Steam off with Steam's reason | `mce/platform/steam.h`, `src/engine/platform/src/steam.cpp` | `test_platform` "steam: no library is an ordinary 'no Steam'…", "the client's refusals and a relaunch through Steam leave Steam off", "a library without the flat API's manual dispatch is refused" |
| `run_callbacks` pumps once a frame and hands each callback's id and bytes to the caller, freeing each; the player, the device (`app_id`, `steam_id`, `persona_name`, `on_steam_deck`, `big_picture`) and `interface()` over versioned accessors | same | `test_platform` "init through the stub, the player and device, callbacks pumped once a frame, shutdown" (two callbacks in order, each freed; `SteamAppId` set for a run outside Steam) |
| The stock player: `-steam.app_id`, `-steam.library`, `-steam.restart`; Steam opens first, a relaunch quits before anything loads or saves, callbacks are pumped every tick, Steam shuts down on quit | `src/player/src/player.cpp` | ctest `scene_demo_player_steam`: "Steam on (app 480, Stub Player)" and the same state hash as `scene_demo_player_600` (`cb0197e002e36782` in Debug) |
| A stand-in `steam_api` (and an old one without manual dispatch) built for tests | `tests/platform/steam_stub.cpp`, `steam_stub_old.cpp` | built only off phones |
| The Steam wall: Steamworks names only in `src/engine/platform/src/steam*.cpp` and `tests/platform/` | `scripts/ci/check_walls.py` | walls stage |
