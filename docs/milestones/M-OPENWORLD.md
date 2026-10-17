# M-OPENWORLD — Open-world game kit (milestone record)

**Status:** in progress (C1, C2 implemented in the C1-C2 thread, see "Track C progress" below); planned otherwise, high priority (added 2026-10-17 at Sami's request; no Linear issues yet: check the free limit, otherwise this record is the tracker).
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


## Track C progress (C1-C2 thread, 2026-10-17)

| Step | Status | PR | What |
|---|---|---|---|
| C1 enter/exit vehicles | implemented, awaiting merge | see the C1-C2 PR | `VehicleSeat3D` + `Rider3D` (`mce/kit/seats3d.h`), Interact or `enter`, `VehicleEntered3D` / `VehicleExited3D`, AI driver hand-off, camera retarget; user guide 9.9 |
| C2 weapons, health, damage | implemented, awaiting merge | same PR | `Health3D`, `Weapon3D`, `Projectile3D` + `apply_damage` (`mce/kit/combat3d.h`), hitscan and stepped projectiles with splash, teams, armor, regen, stagger, ragdoll on death; user guide 9.12 |
| C3 wanted + police BT | not started | | needs C2 (`Damage3D` / `Died3D` events, `apply_damage`) and B3 |
| C4 missions + minimap | not started | | |

Design notes and open items (C1-C2):
- Both steps live in new files (`seats3d.*`, `combat3d.*`); `vehicles3d.*` is untouched so the traffic thread (B2) does not conflict.
- A seated rider's `CharacterController3D` is held in an engine resource, not in the saved scene: a save taken mid-ride restores a default controller on exit. Fix when the save system needs it (store the controller in `Rider3D`).
- Weapons take the aim from the entity's own transform (camera or eye-height child); there is no recoil, no muzzle-flash or tracer asset yet (the `WeaponFired3D` event carries what an effect needs), and no hit-location damage or per-bone hit reactions (the ragdoll gets one whole-body impulse).
- Projectiles are stepped rays (no rigid body): a fast projectile cannot tunnel, but it does not push or get pushed by physics bodies beyond the `impulse` on hit. A body hit by a vehicle takes no damage yet (vehicle collision damage is a C-track follow-up: needs a contact event from `World3D`).
- Linear: the connector failed to connect in this thread (HTTP 404), so no SBA issue was updated; the table above is the tracker until it is back.
