# ADR-0025 — ECS v0.5: change ticks, the ChildOf hierarchy, prefab cook + instantiate, saves and replays

**Status:** Accepted (2026-10-08).

**Context.** ROADMAP M6 asks the ECS for change detection (`is_added/is_changed`, FR-ENT-040), the `ChildOf`
hierarchy with depth-ordered dirty propagation (FR-ENT-050), prefab cook + instantiate without runtime
inheritance (FR-ENT-070), and save/persistence + replay (FR-RUN-065/070). `ecs-gameplay.md` §2.7, §2.8, §2.11,
§2.13, §3.6–§3.9 commit the shapes; this ADR records what was built and where it deviates. The M2 scene chunk
(CHNK_SCENE) existed, but nothing loaded it into a World yet: the instantiation path below is also the first
`load_scene`.

**Decisions.**

1. **Two tick columns per data column.** Every data column carries `added` and `changed` `u32` columns, 8 bytes
   per component instance instead of the 4 bytes in §2.7: `is_added` needs the creation tick, and deriving it
   from structure versions would cost more than it saves. Tags have no column and therefore no ticks
   (`added<Tag>()` is a compile error, not a silent "always").
2. **What a tick is.** One `u32` counter per World advances once per system run, once per sync point that applies
   commands, and once per direct (out-of-phase) write. A system's writes carry its run tick, its reader tick is
   its previous run tick, so a system never sees its own writes and sees everything written since it last ran
   (Bevy's model). Outside a system the reader tick is "everything is new" unless `Query::since(tick)` is given.
   Comparisons wrap (`now - tick < now - since`); every 2^29 ticks sync clamps stored ages to 3·2^30, so the
   comparison stays exact forever (the §3.6 "rotating window" footnote, made concrete).
3. **Stamping sites.** `set`/`add`, Commands, `touch<T>()`, mutable (non-const) query items (each visited row
   — "accessed mutably" counts as written) and `try_get_mut`. The last one deviates from §2.7, which exempted all
   raw pointer writes: `try_get_mut` is where code declares intent to write, and physics2d already writes poses
   through it, so exempting it would have hidden every physics move from transform propagation. A pointer kept
   and written later is still invisible until `touch<T>()`. Change ticks are bookkeeping: `state_hash` ignores
   them, so no determinism hash moved.
4. **Hierarchy in `mce_ecs`, opt-in.** `ecs::install_hierarchy(world)` registers `LocalTransform`, `ChildOf`,
   `WorldTransform`, the ChildOf hooks, a destroy hook and the `transform_propagate` system (FixedUpdate_Post).
   The parent→children index is a resource maintained by the hooks; a `set` of an existing ChildOf (no hook
   fires) is reconciled at the start of propagation, so the index cannot drift. `WorldTransform` carries the
   decomposed pose and its `T·R·S` matrix. Seeds are LocalTransform/ChildOf rows changed since the last run plus
   relinked entities and entities still missing a WorldTransform; they are ordered by (depth, entity index) and
   walked depth-first with a visited set, so rotating one parent re-composes exactly its subtree
   (`HierarchyStats::propagated`, asserted in `test_hierarchy`). Missing WorldTransforms are added through
   Commands and land at the FixedUpdate_Post sync — before render, same tick.
5. **New additive World API.** `add_destroy_hook` (orphans-destroy needs to see a parent before its row goes;
   component hooks cannot, the parent may carry no hierarchy component), `spawn_raw`/`Commands::spawn_raw`/
   `add_raw` (type-erased spawn of reserved handles), `find_component_by_refl`, `get_raw`, and the tick API.
   Nothing was renamed or removed (the editor inspector reads this API in parallel).
6. **`mce_ecs` links `mce_refl`.** `ComponentInfo::refl_id` (the StringId of the reflected type name) is how a
   cooked column finds its ComponentId; `FieldTraits<Entity>` makes `Entity` fields EntityRef. A column whose
   type is not registered in the World is `Errc::NotFound` naming the type — never a silent drop.
7. **A prefab chunk is a scene chunk.** CHNK_PREFAB (`PREFAB_VERSION` 1) uses the CHNK_SCENE layout and reader
   (`SceneView`) with entity 0 as the root; `asset::Prefab` is the asset type tag. `.mcprefab` is the scene
   document with `"format": "mce-prefab"` and no settings. The chunk-id enum already reserved `Prefab`; nothing
   was added to it.
8. **Flatten at cook, JSON-level.** An entity `{ "prefab": "guid:…", "name"?, "components"? }` in a scene or prefab
   is replaced by the prefab's entities (recursively; nested instances first), `"$i"` references resolve within
   the document that spells them, and `"components"` is written over the root's row field by field (adding
   components the root lacks). The importer reads the referenced prefab's parsed document through a new
   `IAssetLookup::native_document` (default: unavailable, so other lookups keep compiling). The dependency scan
   declares each instance as a Prefab dependency, so editing a prefab re-cooks every prefab and scene that
   instantiates it (dependency output hashes are in the cache key). Cycles are reported by the graph build and,
   defensively, by the flattener (depth 16). Rows are re-sorted per column after flattening (overrides can add a
   row for an earlier entity), and a field written twice keeps only its last fixup. The scene importer version
   went 1 → 2.
9. **`mce_ecs_scene` sits above `mce_ecs` and `mce_asset`.** `ecs::instantiate(world, SceneView, SpawnParams)` and
   `load_scene` live there, so the ECS still includes no asset header (§6 layering). Instantiation validates
   every column, override and template reference before reserving a handle (an error spawns nothing), then per
   replica: reserve all handles, copy each entity's rows, remap EntityRef fields to the replica's handles, stamp
   asset refs through an optional caller resolver (resolved once per call), apply overrides, attach roots under
   `SpawnParams::parent`. Inside a phase it records `Commands::spawn_raw` (handles valid at once, rows at the
   sync point); outside it places rows immediately. Overrides are `PrefabOverride::of("Component.path", value)`
   — the reflection property path of §2.11 plus a typed value checked against the resolved leaf.
10. **Saves are keyed by `SaveKey`, not by scene index.** §2.13 has the scene GUID re-load the base world and a
    remap re-apply live state; but entities carry no scene index at runtime, and runtime-spawned persistent
    entities have none. `SaveKey{u32}` is author-assigned identity: load finds the entity with the key (or
    spawns one), adds missing components default-constructed, and writes the `[Save]`-flagged fields by name.
    Unknown types/fields and kind/size changes are skipped and counted (`LoadStats`); a newer save format is
    `Errc::VersionTooNew`, corruption `CrcMismatch` (xxh64 over the body), and `load_game` puts the file path in
    the error (FR-RUN-065 acceptance). The header still carries the scene GUID, module version, tick and
    playtime.
11. **Save file mechanics live in `mce_ecs_scene` for now.** core.md §2.9 assigns `SaveWriter`/`SaveReader` to
    core. The mechanics (temp file, rotate previous to `.bak`, rename; read falls back to `.bak`; root defaults
    to `%LOCALAPPDATA%/MCE/<game>/saves`, or `$XDG_DATA_HOME|~/.local/share/MCE/<game>/saves`) are written
    against `std::filesystem` with error codes, which core avoids. They move to core when a second consumer
    below L2 needs them (input rebinding persistence, FR-INP-040, is the expected trigger).
12. **A replay is the input stream plus the hashes it must reproduce.** `write_replay`/`read_replay` frame the
    `InputRecorder` bytes with module version, scene GUID, start tick, tick count and the start/final
    `state_hash`; a corrupt stream is rejected before a tick runs. The test drives a 3,600-tick (60 s) session
    live, then replays it into a fresh World and compares final hashes (FR-RUN-070 acceptance).

**Not in this change.** Editor prefab authoring and the hierarchy tree view (the editor threads own
`src/editor/**`); render-extract interpolation driven by changed ticks (the renderer's side of FR-ENT-040);
wiring a sample to `load_scene`, saves or replays (samples belong to their own milestones); per-subsystem hashers
composed into `state_hash` (§3.9); live prefab redefinition (editor-time, §2.11).
