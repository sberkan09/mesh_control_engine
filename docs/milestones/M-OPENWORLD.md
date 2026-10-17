# M-OPENWORLD — Open-world game kit (milestone record)

**Status:** planned, high priority (added 2026-10-17 at Sami's request; no Linear issues yet: check the free limit, otherwise this record is the tracker).
Source: `assessments/gta-benzeri-oyun-fizibilitesi-2026-10-17.md`. Goal: close the gaps between today's engine and a GTA-style small open world (1–2 km², driving, on-foot combat, traffic, pedestrians, police chase, a few missions). A GTA 5 clone project starts after this milestone.

**Why.** Terrain, streaming, vehicles, character, AI, audio and script exist. What is missing is the open-world layer: measured performance at city density, large-world precision, city LOD, road/traffic/pedestrian systems, enter/exit vehicles, weapons and damage, wanted/police, and open-licence content.

**Not in scope:** ray tracing, virtual geometry, destruction, cloth/hair, interiors beyond streamed scenes, multiplayer for this kit.

## Tracks (disjoint modules; run in parallel)

| Track | Steps | Modules | Model | Owner |
|---|---|---|---|---|
| A. Scale | A1 city stress scene + `perf_run` budgets on a real GPU (RunPod GPU thread, budget ≤ $4), A2 floating origin / rebase (Opus plan first), A3 HLOD + impostors + building batching, A4 streaming of render LOD per chunk | render, scene/streaming, terrain | Opus (render, perf) | — |
| B. Traffic and crowds | B1 road graph asset (`.mcroads`: lanes, junctions, signals) + editor/MCP authoring, B2 traffic cars on `VehicleDriver3D` (lane follow, yield, signals), B3 pedestrian crowds (sidewalks, BT-cheap LOD, spawn/despawn around focus) | kit, ai | Opus for design, Sonnet implements | — |
| C. Gameplay kit | C1 enter/exit vehicles and seat logic, C2 weapons, projectiles/hitscan, health and damage, hit reactions (ragdoll), C3 wanted level + police BT (algı + tokens), C4 mission/objective components over Luau, minimap and map UI (`.mcui`) | kit, ai, ui | Sonnet (Opus review for core) | — |
| D. Content | D1 M-IMPORT3D (FBX via ufbx, OBJ, glTF gaps): already running in the M-IMPORT3D thread, not part of this track, D2 open-licence content catalogue (CC0/CC-BY only: Kenney, Quaternius, Poly Haven, OpenGameArt, ambientCG) with a licence ledger and cook scripts, D3 humanoid retarget (clips onto the engine's skeleton) | asset/cook, tools | Sonnet | — |
| E. Integration | E1 `samples/09_openworld`: 1 km² slice using A–D, gates in `local_ci.sh` (replay hash, budget counters) | samples | Opus | after A–D |

Dependencies: C and B1 need nothing from A. B2/B3 need B1. C3 needs C2 and B3. E needs all. D1 first for fastest content flow.

## Gates
- Every step: `local_ci.sh --task` green, headless test with numbers, replay hash stable, existing scenes unchanged (opt-in per component).
- A1 states a budget per target; later steps stay inside it.
- Every content file in the repo or a fetch script has a recorded licence (D2 ledger); no non-redistributable assets.

## Thread split (agreed with the coordinator)

1. **A2–A4** (Opus): floating origin, HLOD, impostors, building batching. A1 runs in the RunPod GPU thread.
2. **B1–B2** (road graph and traffic), then **B3** (crowds) as its own thread once B1 lands.
3. **C1–C4** (enter/exit, weapons and damage, wanted and police, missions and map UI): C1+C2 first, C3/C4 after.
4. **D2–D3** (content ledger and cook scripts, retarget) after D1 lands in the M-IMPORT3D thread.
5. **E1** integration sample last.

## Track B progress (roads, traffic, crowds)

Design: [ADR-0046](../adr/0046-road-network.md). Guide: [chapter 29](../user-guide/29-roads-and-traffic.md).
Linear: the Linear connector was unreachable when B1 started; the steps are tracked here until it is back.

| Step | Status | PR | Notes |
|---|---|---|---|
| B1 road network: `.mcroads` cook (`CHNK_ROADS`), `RoadNetwork3D`, lane queries, lights, routes | in review | (this PR) | the chunk type ships with its importer: the capabilities wall needs both |
| B1 authoring: MCP `roads_*` tools, editor overlay | next | | |
| B2 traffic: lane-following drivers on `Vehicle3D`, yielding, signals, spawn/despawn around a focus | planned | | |
| B3 crowds | planned (own thread once B1 lands) | | uses `RoadRoad::sidewalk` beside each kerb |

Open items and risks:
- Lane changes along a road are not modelled; a car keeps its lane until a junction (ADR-0046 "Not now").
- Junction radii come from widths and angles; very acute junctions may need a hand-set `radius`.
- The nearest-lane grid is rebuilt on every hot reload of a network; fine for a city, slow for a country.
