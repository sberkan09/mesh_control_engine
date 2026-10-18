# M-MISSION — Missions, objectives and map UI as data (milestone record)

**Status:** done 2026-10-17 (steps 1-4; no Linear issues: the SBA workspace is at its free issue limit, this record was the tracker). This is step C4 of [M-OPENWORLD](M-OPENWORLD.md) ("missions + minimap"), split into its own milestone because it is unowned and touches modules the other open-world threads do not (new kit files, one UI widget, the Luau API).
Source: `assessments/gta-benzeri-oyun-fizibilitesi-2026-10-17.md` ("3-4 missions with a script, map and minimap, UI data driven").

**Goal.** A game author (or an agent over MCP) builds a mission chain and a map as data: components and events, no C++. Objectives are watched by one kit system; the minimap and the full map are UI documents over the same marker components.

**Not in scope:** a dialogue/cutscene system (M-CINEMA), the wanted level and police (C3), quest journals with branching text, multiplayer sync of mission state.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 1. Missions and objectives | `Mission` (id, ordered `objectives`, `state`: idle / active / complete / failed, `fail_on_death`, `time_limit`), `Objective` (kind: `reach_zone`, `kill`, `collect`, `interact`, `signal`, `timer`; target entity or tag; count; optional `next`), one system that advances them from engine events (trigger enter, `Died3D`, pickup, interact, a named signal), events `MissionStarted`, `ObjectiveDone`, `MissionCompleted`, `MissionFailed`; state saved in the scene so a save mid-mission resumes; deterministic (tick-quantized), replay-hash stable | kit (new `missions.h/.cpp`) | Sonnet |
| 2. Map markers and minimap | `MapMarker` (icon, colour, layer, `always_visible`, `follow_objective`), a `UiMinimap` widget (north-up or heading-up, radius, zoom, clamps off-map markers to the rim) and a full-map screen as `.mcui`; objective markers appear from step 1 automatically | kit, ui | Sonnet |
| 3. Luau and MCP reach | `mission.start/fail/status`, `marker.add/remove`, MCP component schemas and a `docs_search`-visible user guide chapter | script, mcp, docs | Sonnet |
| 4. Demo and close | a headless sample with three missions (reach, kill, deliver) over a toy terrain, gates: exact-number tests, replay hash, save/load mid-mission; milestone closed | samples | Sonnet |

## Gates
- Every step: `local_ci.sh --task` green, headless test with numbers, replay hash stable, existing scenes unchanged (opt-in components).
- No mission or marker state outside components and events (no hidden global registry), so save, replay and the editor see it.
- A change to a header included by many TUs ships as its own seam PR first (CLAUDE.md).

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | done | #524 | `src/engine/kit/{include/mce/kit/missions.h,src/missions.cpp}`, `tests/kit/test_missions.cpp`, user guide 9.13. Objectives chain by `Objective.next` from `Mission.objective` (reflected components have no arrays); `Mission.actor` names who reaches / interacts; no pickup or interact event existed, so `collect` counts `TriggerEnter3D` of a `Trigger3D` and `interact` reads the Interact action |
| 2 | done | #532 | `src/engine/kit/{include/mce/kit/map_markers.h,src/map_markers.cpp}`, `src/engine/ui/{include/mce/ui/minimap.h,src/minimap.cpp}` and the `Minimap` widget kind (`UiMinimap`, one record per Minimap after the string blob of CHNK_UIDOC, older chunks load unchanged), `tests/kit/test_map_markers.cpp`, `tests/ui/test_minimap.cpp`, user guide 9.15. A bare `MapMarker` on an Objective or Mission follows the active objective; the ui reads plain structs the host copies from `kit::collect_map_markers` (the UI never links the kit); feeding it from the stock player screen is not wired yet |
| 3 | done | 3a #537, 3b #543 | 3a: Luau API: `src/engine/script/src/api_missions.cpp` (globals `mission.start/fail/status/signal`, `marker.add/remove`, typed in `mce.d.luau`), `tests/script/test_script_missions.cpp`, user guide 27.3. `mission.fail` has no kit entry point, so the binding sets `Failed` and sends `MissionFailed` with reason 3; a signal sent by a script counts two ticks later (scripts run after the kit system). 3b (#543): the stock player feeds every visible `UiScreen` document's Minimap widgets each frame before the document updates (`src/player/src/minimap_feed.{h,cpp}`, called from `UiScreens::update`; widgets are found once at build, so a document without a Minimap costs nothing), markers from `kit::collect_map_markers`, the view from `kit::map_tracked_view` by the widget's `track`; `tests/player/test_minimap_feed.cpp`, user guide 9.15. |
| 4 | done | 4 (this PR, no number yet) | `samples/10_missions` (assets only: `scenes/missions.mcscene`, `ui/hud.mcui`, `scripts/missions.luau`, `input.txt`, `missions_test.cmake`): reach a zone, kill a `Health3D` target with the player's `Weapon3D`, deliver within 2.5 s; markers on the HUD `Minimap`; the script starts mission 1 and chains 2 and 3 by polling `mission.status`. `ctest -R sample_missions` (`sample_missions_headless_300`): completions pinned at script ticks 58, 82 and 176, a late run fails mission 3 at tick 233, a save at tick 120 resumes and completes mission 3 at 176, replay hash equal over two plays and two resumes. `Mission`, `Objective`, `MapMarker`, `Health3D`, `Weapon3D` left `sample_coverage.allow`; user guide 9.14 and 18 |

## Known limits

- **Ref remapping in saves.** Entity references a mission holds (`objective`, `actor`, `target`, `next`) are saved as handles and resolve only in the same base scene a load rebuilds; give the entities a `SaveKey` and spawn them from the same scene. `Health3D` is not saved, so a target that died before the save is alive again after the load.
- **Two-tick Luau signal latency.** Scripts run after the kit's mission system, so a `mission.signal` counts two ticks later, and a script sees a state change a tick after the kit made it (the sample's logged ticks are the script's).
- **Per-HUD `track` name for split-screen.** A `Minimap` follows the entity named by its `track` (default `Player`); every local player needs a differently named entity and its own HUD document.
- **No icon-by-name.** `MapMarker.icon` is a region index of the minimap skin's sprite set; 0 is a flat square.
- **No editor inspector for `UiMinimap`.** The widget is authored in the `.mcui` text (and read over MCP as `"minimap"`); the UI editor does not show its fields.
- **Objective chain instead of array.** Reflected components have no arrays, so a mission's goals are `Objective` entities linked by `next` from `Mission.objective`, and one goal is active at a time.
- **Scripts poll.** `mce.events` does not carry the mission events, so a script chains the next mission by reading `mission.status` of the one before.
