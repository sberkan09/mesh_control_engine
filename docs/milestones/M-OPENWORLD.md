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

## Step status

| Step | Status | PRs | Notes |
|---|---|---|---|
| A1 | in progress (RunPod GPU thread) | | budgets per target |
| A2 | in progress: seam merged, floating origin (core + module listeners) in review | #495 design, #502 seam, floating-origin PR | [M-OPENWORLD-A-design.md](M-OPENWORLD-A-design.md) |
| A3 | design done | | same design |
| A4 | design done | | same design |

Linear: the SBA workspace is at its free issue limit (save_issue refuses new issues, 2026-10-17), so track A steps are recorded here only.

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
| C4 missions + minimap | moved to its own milestone [M-MISSION](M-MISSION.md) (2026-10-17) | | |

Design notes and open items (C1-C2):
- Both steps live in new files (`seats3d.*`, `combat3d.*`); `vehicles3d.*` is untouched so the traffic thread (B2) does not conflict.
- A seated rider's `CharacterController3D` is held in an engine resource, not in the saved scene: a save taken mid-ride restores a default controller on exit. Fix when the save system needs it (store the controller in `Rider3D`).
- Weapons take the aim from the entity's own transform (camera or eye-height child); there is no recoil, no muzzle-flash or tracer asset yet (the `WeaponFired3D` event carries what an effect needs), and no hit-location damage or per-bone hit reactions (the ragdoll gets one whole-body impulse).
- Projectiles are stepped rays (no rigid body): a fast projectile cannot tunnel, but it does not push or get pushed by physics bodies beyond the `impulse` on hit. A body hit by a vehicle takes no damage yet (vehicle collision damage is a C-track follow-up: needs a contact event from `World3D`).
- Linear: the connector failed to connect in this thread (HTTP 404), so no SBA issue was updated; the table above is the tracker until it is back.

## Track B progress (roads, traffic, crowds)

Design: [ADR-0046](../adr/0046-road-network.md). Guide: [chapter 29](../user-guide/29-roads-and-traffic.md).
Linear: the Linear connector was unreachable when B1 started; the steps are tracked here until it is back.

| Step | Status | PR | Notes |
|---|---|---|---|
| B1 road network: `.mcroads` cook (`CHNK_ROADS`), `RoadNetwork3D`, lane queries, lights, routes | done | #500 | the chunk type ships with its importer: the capabilities wall needs both |
| B1 authoring: MCP `roads_*` tools, editor overlay | next | | |
| B2 traffic: lane-following drivers on `Vehicle3D`, yielding, signals, spawn/despawn around a focus | planned | | |
| B3 crowds: `CrowdSpawner3D`, `Pedestrian3D` (`kit/crowd.h`), pavement edge graph, kerb hops, light-aware crossings, LOD stepping, spawn ring | in review | #513 | [ADR-0048](../adr/0048-pedestrian-crowds.md), guide 29.6; `tests/kit/test_crowd.cpp` |

Open items and risks:
- Lane changes along a road are not modelled; a car keeps its lane until a junction (ADR-0046 "Not now").
- Junction radii come from widths and angles; very acute junctions may need a hand-set `radius`.
- B3: pedestrians do not avoid each other or react to the player; unsignalled crossings never wait, so B2 traffic must yield
  to a walker on a crossing (it needs a crowd query: a cheap "who is crossing near here" lookup is the next seam). Heights
  follow the network's plane only. The prefab spawn path is exercised by the same code as co-op's player prefab but has no
  test of its own yet. Linear: not updated (connector unreachable this session); recorded here.
- The nearest-lane grid is rebuilt on every hot reload of a network; fine for a city, slow for a country.

## Track D as built (D2, D3; thread "open-licence content line", 2026-10-17)

Linear is at its free issue limit (creating SBA issues for D2/D3 failed with "exceeded the free issue limit"), so this
section is the tracker. State: D2 and D3 merged in #504; the follow-up below (ORM packing, real-pack cook test, node-rig retarget) is the second PR.

**D2: ledger and cook scripts.** `content/ledger.json` (13 packs, all CC0-1.0): Kenney City Kit commercial / suburban / roads,
Car Kit, Blocky Characters, Impact Sounds; ambientCG Asphalt031, Road007, Concrete034, PavingStones070, Grass004 (1K PBR);
Poly Haven `kloppenheim_06_puresky` 1K HDRI; Quaternius Universal Animation Library (manual download: itch.io needs a
browser session, no hash pinned yet). Every http pack pins its archive SHA-256 and size (read 2026-10-17); a changed upstream
file fails the fetch. `tools/mce_cli/mce_content.py` (`mce content list|info|check|fetch|add|verify|credits`) validates the
ledger (only CC0-1.0, CC-BY-3.0, CC-BY-4.0; CC-BY needs an attribution line; https URLs, sha256, safe paths), downloads into a
cache, extracts by rule into `assets/third_party/<pack>/`, writes sidecars with GUIDs derived from pack id + path, `.mcmat`
materials for PBR sets (colour + normal, roughness as a factor), `third_party_content.json` and the generated block of
`CREDITS.md`; `verify` is the ship gate. Tests: `tests/import/test_content.py` (offline: fake opener, synthetic zips; cooks with
`mce_cook` when given). Docs: guide 5.3, 16.7.
Follow-up (second PR): the ambientCG entries pin the 1K-PNG archives and an `orm` block packs roughness (+ occlusion) into
the `metallic_roughness` texture (pure-Python 8-bit PNG codec, deterministic); `tests/import/test_content_real.py`
(`import_content_real`, label `slow`, skips offline) fetches road, car, character, sound and PBR packs by their ledger
hash and cooks them with `mce_cook`: 0 failed, run once here against the live downloads.
Open items: pin the Quaternius hash once someone downloads it; `--online` check is not run in CI; ledger entries for Quaternius city/vehicle packs, OpenGameArt and
Poly Haven models/textures; hooking the ledger into MCP `library_search` / `library_add`; 

**D3: humanoid retarget.** `tools/mce_cli/mce_retarget.py` (`mce retarget <src>... --onto <character.glb>`): name-based humanoid
map (Mixamo, Unreal, Rigify, VRM, Biped, engine names; `--map` overrides), world-space delta retarget (A-pose to T-pose,
facing correction, spine chain resampling), hips translation scaled by hip height, `--in-place-motion`, `--no-translation`,
`--prefix`, `--clips`, `--replace`, `--strict`; output is the character `.glb` plus clips (the importer puts clips in the mesh
chunk), deterministic. Tests: `tests/import/test_retarget.py` (rigs built in code; cooks the output with `mce_cook` when
given). Docs: guide 8.9, 16.7. Follow-up: `--map-only` pairs exactly the nodes a `--map` file names, for skinless node rigs (Kenney Blocky Characters:
retargeting `walk` from character-a onto character-b cooks with `mce_cook`); a skinless rig without it is refused with that
explanation; the tool warns when the output is not beside the character's relative textures.
Open items: no IK/foot-slide fix, Y-up only, fingers matched by name only, retarget not exposed as an MCP tool, not run on a
real Mixamo/Quaternius skinned file (Quaternius needs a manual download).

### E1 content set (ready from the ledger)

Everything below is CC0 and comes from `mce content add <id> <project>`; `mce content verify` is the ship gate.
Counts are the GLB models in each archive (read 2026-10-17).

| Need | Pack id | What it gives | Gap |
|---|---|---|---|
| Roads, junctions, signals | `kenney-city-kit-roads` | 95 models: 62 road pieces (straight, bends, crossings, junctions, ramps, bridges), traffic lights (5), street lights (6), signs (3), electricity (8), construction props | no lane/graph data: the B1 `.mcroads` is authored from these piece sizes |
| Downtown | `kenney-city-kit-commercial` | 41 models: 19 buildings, 16 low-detail blocks, 6 details | one colormap, low-poly style; no interiors |
| Residential | `kenney-city-kit-suburban` | 40 models: 21 houses, fences, paths, driveways, trees | same |
| Traffic and player cars | `kenney-car-kit` | 50 models: sedan, suv, truck, delivery, ambulance, race cars, karts, 8 wheel pieces, debris | wheels are separate nodes; collision shapes need authoring (`MeshCollider3D`/convex) |
| Pedestrians | `kenney-blocky-characters` | 18 characters with idle, walk, sprint, sit, drive, die, pick-up, emotes, holding poses (node-animated) | blocky style; no weapon-fire clips; no skin, so no IK/ragdoll from the mesh |
| Skinned humanoid animation | `quaternius-universal-animation-library` | locomotion, combat and vehicle clips for `mce retarget` | manual download (itch.io), hash not pinned yet; needs a skinned CC0 character (none in the ledger) |
| Ground and walls | `ambientcg-asphalt031`, `-road007`, `-concrete034`, `-pavingstones070`, `-grass004` | 1K PBR sets with generated `.mcmat` (colour, normal, packed ORM) | 1K only; no displacement use yet |
| Sky | `polyhaven-kloppenheim-06-puresky` | 1K day HDRI for `Environment3D` | day only; a dusk/night HDRI should be added for the day-night cycle |
| Footsteps and impacts | `kenney-impact-sounds` | 130 `.ogg`: footsteps on surfaces, impacts | no engine, gun, siren, radio or ambience audio: needs new ledger entries (OpenGameArt/Kenney audio packs) |

Not covered by any pack yet (add ledger entries before E1): engine/gun/siren/ambience audio, a skinned civilian and police
character, weapons, a night HDRI, signage textures, a city skyline backdrop. The E1 slice can start with the table above and
treat these as placeholders.
