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

## Step status

| Step | Status | PRs | Notes |
|---|---|---|---|
| A1 | in progress (RunPod GPU thread) | | budgets per target |
| A2 | design done | | [M-OPENWORLD-A-design.md](M-OPENWORLD-A-design.md) |
| A3 | design done | | same design |
| A4 | design done | | same design |

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

## Track D as built (D2, D3; thread "open-licence content line", 2026-10-17)

Linear is at its free issue limit (creating SBA issues for D2/D3 failed with "exceeded the free issue limit"), so this
section is the tracker. State: D2 and D3 implemented in one PR (PR number added at merge).

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
Open items: pin the Quaternius hash once someone downloads it; roughness maps are not packed into `metallic_roughness`
(needs an image tool); `--online` check is not run in CI; ledger entries for Quaternius city/vehicle packs, OpenGameArt and
Poly Haven models/textures; hooking the ledger into MCP `library_search` / `library_add`; a real-pack cook with `mce_cook` was
not run here (the offline test cooks a synthetic pack).

**D3: humanoid retarget.** `tools/mce_cli/mce_retarget.py` (`mce retarget <src>... --onto <character.glb>`): name-based humanoid
map (Mixamo, Unreal, Rigify, VRM, Biped, engine names; `--map` overrides), world-space delta retarget (A-pose to T-pose,
facing correction, spine chain resampling), hips translation scaled by hip height, `--in-place-motion`, `--no-translation`,
`--prefix`, `--clips`, `--replace`, `--strict`; output is the character `.glb` plus clips (the importer puts clips in the mesh
chunk), deterministic. Tests: `tests/import/test_retarget.py` (rigs built in code; cooks the output with `mce_cook` when
given). Docs: guide 8.9, 16.7. Open items: no IK/foot-slide fix, Y-up only, skinless node rigs (Kenney Blocky) unsupported,
fingers matched by name only, retarget not exposed as an MCP tool, not run on a real Mixamo/Quaternius file here.
