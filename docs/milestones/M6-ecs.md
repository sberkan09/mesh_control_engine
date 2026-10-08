# M6 — ECS v0.5: change detection, hierarchy, prefabs, saves, replays (milestone record)

**Status:** the ECS line items of ROADMAP M6 plus FR-RUN-065/070, merged into `dev_claude`. Design record:
[ADR-0025](../adr/0025-ecs-v05-change-ticks-hierarchy-prefabs.md). The other M6 items (editor v0.5, 2D extras,
audio v0.5, input rebinding, physics v0.5, SequenceRunner, lifecycle polish, shader cook v0.5) are tracked by
their own milestone records.
**Measured on:** the Linux CI container (4 cores), `linux-gcc-release`.

## What shipped

| ROADMAP M6 / FR item | Where | Evidence |
|---|---|---|
| Change detection: per-row added/changed ticks, `added<T>()`/`changed<T>()` query filters, `is_added/is_changed` (FR-ENT-040) | `mce/ecs/world.h` (Table tick columns, Query filters, `Frame::is_added/is_changed`, `World::touch`) | `test_ecs` "change detection" cases: system reader ticks, Commands stamping at sync, mutable query stamping, swap-remove/archetype moves, counter wraparound |
| `ChildOf` hierarchy, engine-maintained children, depth-ordered dirty propagation (FR-ENT-050) | `mce/ecs/hierarchy.h` (`install_hierarchy`, `WorldTransform`, `transform_propagate`) | `test_ecs` "hierarchy" cases: same-tick parent rotation, only dirty subtrees (13+1 of 650), re-parenting, cycle rejection, orphans-destroy, Commands-spawned children |
| FR-ENT-040 acceptance: moving 1 of 5k entities touches exactly 1 | `HierarchyStats`, `changed<WorldTransform>()` | "moving 1 of 5,000 entities…" asserts 1 seed, 1 propagated, 1 changed WorldTransform seen by a Render_Pre reader |
| Prefab cook: `.mcprefab` → CHNK_PREFAB, nested prefabs flattened at cook, instance overrides (FR-ENT-070) | `prefab` importer (`scene_importer.cpp`), `IAssetLookup::native_document`, `asset::Prefab` | `test_ecs_scene` "prefab" cases: nested flatten row-identical to the hand-composed prefab (state hash), editing a prefab re-cooks its users with overrides kept, cook errors (unknown, cycle, empty) |
| Prefab instantiate: bulk row copy + handle remap + overrides, no runtime inheritance | `mce/ecs/prefab.h` (`mce_ecs_scene`: `instantiate`, `load_scene`, `PrefabOverride`) | replicas + parent + property-path overrides; asset refs stamped through a resolver; runtime errors spawn nothing; in-phase instantiation through Commands |
| Scenes with prefab instances load through the same path (FR-ENT-060) | `ecs::load_scene` | "a scene with prefab instances loads…" |
| Save/persistence: versioned `[Save]` reflection dump, atomic files + `.bak`, diagnosable newer-version error (FR-RUN-065) | `mce/ecs/save.h` (`SaveKey`, `write_save/read_save`, `save::write/read`, `save_game/load_game`) | `test_ecs_scene` "save" cases: SaveKey round trip, other module versions, corrupt/truncated/newer/bad-magic, `.bak` rotation and fallback, error path |
| Replay: per-tick input snapshots replay to the live run's final hash (FR-RUN-070) | `save::write_replay/read_replay` over `input::InputRecorder` | "replay: …" — 3,600 ticks (60 s) live, then replayed into a fresh World: identical final `state_hash` |

`mce_cook` now links `mce_ecs`, so the CLI cooks scenes and prefabs that use `LocalTransform`, `ChildOf` and
`SaveKey` (engine components reflected in `mce_ecs`).

## Budget table

| Measure | Target | Result |
|---|---|---|
| NFR-T-01 `02_boids` integrate, 100k entities | < 1.5 ms/iteration | integrate 0.10–0.11 ms/tick, full tick 1.06–1.15 ms (3 runs, 300 ticks; mutable query items now stamp change ticks) |
| Hierarchy tick, 5,000 entities (1,000 roots × 4 children), nothing dirty | — | 0.03–0.05 ms |
| Same, every root dirty (5,000 re-composed) | — | 0.82–1.19 ms |
| Same, one entity dirty | — | 0.03 ms (the idle cost: the seed queries; 1 entity re-composed) |
| Change-tick memory | 4 B/instance (§2.7) | 8 B/instance (added + changed; ADR-0025 §1) |

## Determinism

Change ticks are bookkeeping and `state_hash` does not include them; the determinism stage's hashes for
`02_boids`, `physics2d_headless`, `03_platformer`, `04_diorama` and the module devhost are unchanged by this work
and still identical across 3 runs and Debug vs Release.

## Open

- Editor: prefab authoring, the hierarchy tree view and inspector change highlighting read this API; they are
  editor-thread work (`src/editor/**`).
- Render interpolation of only-moved entities consumes `changed<WorldTransform>()`; the renderer's extract does
  not use it yet.
- No sample loads a cooked scene, writes a save or records a replay yet; Artifact A's "best time persists"
  (FR-RUN-065 acceptance) needs the platformer to put its record on a `SaveKey` entity.
- Save file mechanics move from `mce_ecs_scene` to core when input rebinding persistence (FR-INP-040) needs them
  (ADR-0025 §11).
