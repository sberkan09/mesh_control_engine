# ADR-0041 — Steam through the Steamworks flat API, loaded at run time

**Status:** Accepted (2026-10-15). **Milestone:** M-STEAM step 1 (off-tier, opt-in). **Plan:**
`docs/milestones/M-STEAM.md`. **Reference:** `src/engine/platform/include/mce/platform/steam.h`.

**Context.** Steam is where desktop games are sold, and the engine comparison (the project's
`assessments/motor-kiyaslama-eksikler.md`, item 2) lists it as the largest distribution gap after the web.
A Steam game needs the Steam client's sign-in, achievements and stats, leaderboards, cloud saves, the
overlay and Steam Input. Valve ships them in the Steamworks SDK: C++ headers plus a redistributable
`steam_api` library (`libsteam_api.so`, `steam_api64.dll`, `libsteam_api.dylib`). The SDK is downloaded
behind a Steamworks partner login and its license does not let a public repository carry it, and MCE's
cloud CI has no Steam client.

**Options weighed.**

| Option | For | Against |
|---|---|---|
| **Open `steam_api` at run time and look up the flat API's C entry points by name** | No SDK in the repo or the build; a game without Steam never loads anything; CI covers the loader with a stand-in library; one engine binary works with and without Steam | MCE declares the few signatures it uses itself, so a change in Valve's flat API is caught by the stand-in tests and a real run, not by the compiler |
| Link the SDK at build time behind an option (`-DMCE_STEAMWORKS_SDK=<dir>`) | The compiler checks every call | Every game build needs the SDK; CI can never build that code; a missing library stops the game from starting instead of just turning Steam off |
| A community wrapper (Steamworks.NET-style generated bindings) | Generated, complete | Another dependency to pin; still needs the library; most of it unused |

## Decisions

1. **Run-time loading.** `platform::steam::init()` opens the library named by `steam.library` (default: the
   platform's file name next to the executable) through `platform::load_library`. Every entry point is
   looked up by name; a missing library is `InitResult::NoLibrary`, a library without the flat API's init
   or manual dispatch is `MissingEntry`, and in both cases the game runs on without Steam.
2. **The flat API only.** MCE calls `SteamAPI_InitFlat` (falling back to `SteamAPI_Init` on older SDKs),
   `SteamAPI_RestartAppIfNecessary`, `SteamAPI_Shutdown`, the `SteamAPI_ManualDispatch_*` functions and the
   flat `SteamAPI_ISteam*_*` methods, with the interface objects from their versioned accessors
   (`SteamAPI_SteamUtils_v010`, ...). `steam::interface()` tries a list of accessor versions, newest
   first, so a newer library that keeps exporting an older version still works.
3. **Manual dispatch, main thread.** Manual dispatch is chosen before init, so Steam never calls into the
   game on a thread of its own. `run_callbacks()` pumps once a frame and hands each callback (its
   Steamworks id and bytes) to the caller, who decodes it (step 2 onward). Nothing Steam answers touches the
   simulation directly: what the game sees goes through the liveops game services, which record answers for
   replays (M-LIVEOPS step 2).
4. **The app id.** The stock player opens Steam when `steam.app_id` is set (`-steam.app_id 480`). A run
   outside Steam names the app to the client through `SteamAppId` / `SteamGameId` in its environment, the
   effect of `steam_appid.txt`, so no file is written next to the game. With `steam.restart 1` (release
   builds) a run started outside Steam asks Steam to relaunch it and quits before loading anything.
5. **The Steam wall.** Steamworks names (`SteamAPI_*`, `ISteam*`, `<steam/...>`) appear only in
   `src/engine/platform/src/steam*.cpp` and the stand-ins under `tests/platform/` (`check_walls.py`).
   `steam.h` exposes MCE types only.
6. **Tests without Steam.** `tests/platform/steam_stub.cpp` builds a shared library with the same entry
   points, answers the test sets and a count of every call. `test_platform` covers init and its refusals,
   the relaunch, the callback pump and shutdown; `scene_demo_player_steam` plays scene_demo with Steam on and
   prints the same state hash as without. A run against the real client is SBA-112 and needs the user's
   Steamworks account.

**Consequences.** A game ships `steam_api` beside its executable (step 5 lays out the depots). Linux builds
need the library next to the binary or on the loader path; `ship --steam` handles that. Steam's own
interfaces (stats, leaderboards, remote storage, input) are added per step behind `steam.h` and the
game-services and input interfaces the engine already has, so game code never sees Steam.
