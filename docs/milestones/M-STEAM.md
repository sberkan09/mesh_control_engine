# M-STEAM — Steamworks (milestone record)

**Status:** done against the stand-in (steps 1–6, Linear SBA-106 … SBA-111). Open: SBA-112, the run against
the real Steam client, which needs the user's Steamworks account.
**Why:** Steam is the desktop distribution channel; the engine comparison (the project's
`assessments/motor-kiyaslama-eksikler.md`, item 2) lists it right after the web. Added 2026-10-15 under the
user's standing instruction to extend the roadmap when it runs out.
**Design:** ADR-0042 (the Steamworks flat API opened at run time; no SDK in the repo or the build).
**Measured on:** the Linux CI container, against a stand-in `steam_api`.

## Steps

| Step | Linear | State |
|---|---|---|
| 1. ADR-0042 and the run-time loader: `mce/platform/steam.h`, init and its refusals, manual-dispatch callbacks, the player's `-steam.app_id`, a stand-in library for tests, the Steam wall | SBA-106 | done |
| 2. Achievements, stats, leaderboards and cloud saves as `platform::game_services` `Backend::Steam` under `liveops::PlatformGameServices` | SBA-107 | done |
| 3. Overlay (holds the world like a lost focus) and rich presence as data | SBA-108 | done |
| 4. Steam Input: an action manifest from the cooked input map, glyphs, Steam Deck defaults | SBA-109 | done |
| 5. `ship --steam`: depots per platform, `steam_api` beside the binary, generated steamcmd build scripts | SBA-110 | done |
| 6. Demo, user guide, gates | SBA-111 | done |
| Later: the real Steam client (achievements configured, overlay, Steam Input, a beta-branch upload) | SBA-112 | needs the user's account |

## Step 1 — the run-time loader

| Item | Where | Evidence |
|---|---|---|
| `platform::steam::init` opens `steam_api` (the `steam.library` path or the platform's file name next to the executable), resolves the flat API by name and initialises Steam with manual dispatch; `NoLibrary`, `MissingEntry`, `NoSteamClient`, `VersionMismatch`, `Failed` and `Restarting` leave Steam off with Steam's reason | `mce/platform/steam.h`, `src/engine/platform/src/steam.cpp` | `test_platform` "steam: no library is an ordinary 'no Steam'…", "the client's refusals and a relaunch through Steam leave Steam off", "a library without the flat API's manual dispatch is refused" |
| `run_callbacks` pumps once a frame and hands each callback's id and bytes to the caller, freeing each; the player, the device (`app_id`, `steam_id`, `persona_name`, `on_steam_deck`, `big_picture`) and `interface()` over versioned accessors | same | `test_platform` "init through the stub, the player and device, callbacks pumped once a frame, shutdown" (two callbacks in order, each freed; `SteamAppId` set for a run outside Steam) |
| The stock player: `-steam.app_id`, `-steam.library`, `-steam.restart`; Steam opens first, a relaunch quits before anything loads or saves, callbacks are pumped every tick, Steam shuts down on quit | `src/player/src/player.cpp` | ctest `scene_demo_player_steam`: "Steam on (app 480, Stub Player)" and the same state hash as `scene_demo_player_600` (`cb0197e002e36782` in Debug) |
| A stand-in `steam_api` (and an old one without manual dispatch) built for tests | `tests/platform/steam_stub.cpp`, `steam_stub_old.cpp` | built only off phones |
| The Steam wall: Steamworks names only in `src/engine/platform/src/steam*.cpp` and `tests/platform/` | `scripts/ci/check_walls.py` | walls stage |

## Step 2 — game services on Steam

| Item | Where | Evidence |
|---|---|---|
| `.mcservices` entries take a `steam` name (Steam uses the catalog `id` without one); the catalog chunk is v2 with the Steam id | `mce/liveops/game_services.h`, `game_services_catalog.cpp`, `services_importer.cpp` | `test_cook_services` "cooks to CHNK_GSVC and decodes"; `test_liveops` "the catalog chunk round-trips" |
| `IPlatformGamesBackend::store_ids()` picks the ios, android or steam id | `mce/liveops/platform_games.h` | `test_liveops` "Steam takes the steam id, or the catalog id when there is none" |
| Asynchronous call results: `steam::await_call` waits for a call's handle, and `run_callbacks` fetches its result before freeing the callback | `mce/platform/steam.h`, `steam.cpp` | `test_platform` "an asynchronous call's result reaches the one waiting for it" |
| `game_services::Backend::Steam` on desktop while Steam runs: sign-in from the client, `SetAchievement` + `StoreStats` (progress below the total through `IndicateAchievementProgress`), `FindLeaderboard` then `UploadLeaderboardScore` keep-best with the handle kept, Steam Cloud files for blobs, the overlay's achievements and stats pages | `src/engine/platform/src/steam_games.cpp`, `game_services.cpp` | `test_platform` "game services on Steam: sign-in, achievements, leaderboards, Steam Cloud, the overlay" |
| The stock player picks Steam as the game services backend when no provider is named | `src/player/src/player.cpp` | ctest `player_game_services_steam`: `SignedIn`, `AchievementSent first_win`, `AchievementSent win_10 value 3`, `ScoreSent best_time value 4200`, `CloudSaved progress value 1` on the stand-in |

Not measured: the real Steam client (achievements and boards defined on a Steamworks page, Cloud quota);
that is SBA-112.

## Step 3 — the overlay and rich presence

| Item | Where | Evidence |
|---|---|---|
| `RichPresence` (data component: status, a localization token, two substitutions) and the `OpenOverlay` event kept in `OverlayRequests` until the player takes them; `kit::install_stock` installs them; neither changes the state hash | `mce/kit/presence.h`, `src/engine/kit/src/presence.cpp` | `test_kit` "the first enabled RichPresence is the one shown", "OpenOverlay requests wait until the player takes them", "presence and overlay requests leave the state hash alone" |
| The loader tracks the overlay from Steam's callback (331) and opens pages, the store page and web pages; rich presence keys set and cleared | `mce/platform/steam.h`, `steam.cpp` | `test_platform` "the overlay's state from its callback, its pages, and rich presence" |
| The stock player: the overlay holds the game like a lost focus (pause, mute, the focus-loss save), the scene's presence sent when it changes, overlay requests passed on | `src/player/src/player.cpp` (`update_steam`) | ctest `player_steam_overlay`: the stand-in opens its overlay on frame 3 and closes it on frame 6; "Steam overlay open: the game holds" then "closed"; the three presence keys sent once |
| The cook, the editor and the MCP server register `RichPresence` | the three `engine_components.cpp` | `capabilities.py --check` |

Not measured: the real overlay and the friends list's rendering of a localized presence (SBA-112).

## Step 4 — Steam Input

| Item | Where | Evidence |
|---|---|---|
| `InputSystem::set_action_feeds`: a platform's values added to actions by name like one more device (recorded and replayed with the sampled state) | `mce/input/input_system.h`, `input_system.cpp` | `test_input` "feeds hold a button and move a stick, until cleared" |
| A gamepad namer: the options screen names a gamepad binding by the platform's name for it | `mce/input/binding_names.h`, `options_panel.cpp` | `test_input` "a namer renames gamepad bindings only, and only the actions it knows" |
| The action manifest (`game_actions_<appid>.vdf`, one set `Game`) and default configurations for the Steam Deck, Xbox, PlayStation and Switch Pro controllers from the input map; the Deck adds the right trackpad | `mce/input/steam_config.h`, `steam_config.cpp` | `test_input` "the action manifest and default configurations come from the map" |
| `platform::steam::input`: Steam Input over the flat API (explicit frames, the set activated on every controller, digital and analog actions by handle, an action's origin name and glyph) | `mce/platform/steam_input.h`, `steam_input.cpp` | `test_platform` "Steam Input reads the actions of the set by name, and names their inputs" |
| The stock player: `-steam.input`, `-steam.write_input_config`; Steam Input started with the map's named actions (a stick's y flipped where the map reads it down), fed every frame, the namer set | `src/player/src/player.cpp` | ctest `player_steam_input`: one stand-in controller holding Jump and Move, both read; the five files written; the state hash equal to the run without Steam; `-steam.input 0` and a missing app id |

Not measured: a real controller through the Steam client, the Deck's trackpad, the Steamworks page's
manifest upload (SBA-112). Several maps share the one set `Game`; per-map sets (menus) are not generated.

## Step 5 — ship --steam

| Item | Where | Evidence |
|---|---|---|
| The depot layout (`content/<os>/` with `steam_api` beside the executable, inside the `.app` on macOS, no `steam_appid.txt`), the app and depot build scripts for every platform present (depot ids app + 1/2/3 by default), `UPLOAD.txt` with the steamcmd line; the library from `--steam-library` or `STEAMWORKS_SDK` | `tools/mce_cli/mce_steam.py` (installed beside `mce`) | ctest `steam_depot`: three platforms accumulated into one output, a chosen depot id and branch, the scripts' braces balance, the SDK lookup and its error |
| `scripts/ship.py --steam <app>` (`--steam-depot`, `--steam-library`, `--steam-branch`), `mce ship --steam`, MCP `ship_build` `steam_app_id` / `steam_library` (returns `steam_build_script`) | `scripts/ship.py`, `tools/mce_cli/mce.py`, `tools_code.cpp` | `mcp_transcript` (the tool's schema) |
| A player started the way Steam starts it: no `-steam.app_id`, `SteamAppId` in the environment, `steam_api` found beside the executable; `-steam.app_id -1` keeps Steam off | `src/player/src/player.cpp` (`steam_app_id`) | ctest `steam_depot`: the stock player copied into a depot with the stand-in as `libsteam_api.so` signs in as app 480, and runs without Steam when `SteamAppId` is unset |

Not measured: a real steamcmd upload and a build installed by the Steam client (SBA-112); Windows and
macOS depots made on their own OS (the layout is tested here from staged folders).

## Step 6 — the demo, the user guide and the gates

| Item | Where | Evidence |
|---|---|---|
| `samples/steam_demo`: a content project (a scene with a `RichPresence`, a `.mcservices` with Steam names) played by the stock player | `samples/steam_demo/` | ctest `sample_steam_demo` (below) |
| One run on the stand-in: Steam Input reading the controller's Jump, the presence sent once, achievement, progress, a score on "Best Time" and a Steam Cloud save; the overlay holding the sim; the same state hash with and without Steam (overlay aside, which pauses); the build laid out as a depot and started with `SteamAppId` only | `samples/steam_demo/steam_demo_test.py` | ctest `sample_steam_demo`, in every `local_ci.sh` debug stage |
| User guide chapter 26 (Steam): turning it on, the demo, game services, overlay, presence, Steam Input, shipping, a release checklist; 23.10 points to it | `docs/user-guide/26-steam.md` | — |

The gates are the ctests above (`ctest -R steam`: `test_platform` "steam:" cases, `player_steam_*`,
`player_game_services_steam`, `scene_demo_player_steam`, `steam_depot`, `sample_steam_demo`), all run by
`local_ci.sh`. Not measured: anything in Valve's client (SBA-112).
