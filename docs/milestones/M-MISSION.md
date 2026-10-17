# M-MISSION — Missions, objectives and map UI as data (milestone record)

**Status:** planned 2026-10-17, step 1 starting (no Linear issues: the SBA workspace is at its free issue limit, this record is the tracker). This is step C4 of [M-OPENWORLD](M-OPENWORLD.md) ("missions + minimap"), split into its own milestone because it is unowned and touches modules the other open-world threads do not (new kit files, one UI widget, the Luau API).
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
| 1 | implemented, awaiting CI | | `src/engine/kit/{include/mce/kit/missions.h,src/missions.cpp}`, `tests/kit/test_missions.cpp`, user guide 9.13. Objectives chain by `Objective.next` from `Mission.objective` (reflected components have no arrays); `Mission.actor` names who reaches / interacts; no pickup or interact event existed, so `collect` counts `TriggerEnter3D` of a `Trigger3D` and `interact` reads the Interact action |
| 2 | planned | | needs 1 for the objective markers |
| 3 | planned | | needs 1 and 2 |
| 4 | planned | | |
