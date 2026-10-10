# ecs — L1 archetype ECS (World, phases, Commands, events, change ticks, hierarchy) and its L2 scene layer

Two targets live in `src/engine/ecs/CMakeLists.txt`. `mce_ecs` is layer L1: archetype tables, generational
entities, queries, phases, `Commands`, events, resources, change ticks and the `ChildOf` transform hierarchy.
It depends publicly on `mce::mce_core` and `mce::mce_refl`. Component identity is `mce::kTypeHash<T>`, and
`refl::TypeRecord::type_hash` carries the same value, so reflected components can also be reached type-erased.
It is linked by `mce_input`, `mce_audio`, `mce_physics2d`, `mce_physics3d`, `mce_ui`, `mce_scene`,
`mce_gameplay_kit`, `mce_module_host`, the samples and the `mce_engine` umbrella.

`mce_ecs_scene` is layer L2: cooked scene and prefab chunks into live rows, and saves and replays. It depends
publicly on `mce::mce_ecs` and `mce::mce_asset`, so the ECS itself never includes an asset header
(ecs-gameplay.md §6 layering). It is linked by `mce_gameplay_kit`, `tools/cook`, the platformer sample and
`mce_engine`.

## Public surface

`mce_ecs` (`src/engine/ecs/include/mce/ecs/`)
- `entity.h`: `Entity` (index + generation, 8 bytes; generation 0 is null), `ComponentId` (u16, at most
  `kMaxComponents = 256`), `ComponentMask`, `ComponentInfo` (type hash, name, size, align, tag flag,
  `refl_id`), `RawComponent`, `is_component_v<T>`. Specializes `refl::FieldTraits<Entity>` as
  `FieldKind::EntityRef`.
- `world.h`:
  - `World`: `register_components<Ts...>()`, `spawn`, `spawn_with`, `destroy`, `alive`, `has`, `try_get`,
    `try_get_mut`, `set`, `add`, `remove`; hooks (`set_hooks`, `add_destroy_hook`); type-erased access
    (`find_component`, `find_component_by_refl`, `get_raw`, `set_raw`, `remove_raw`, `spawn_raw`); resources
    (`set_resource`, `resource`, `try_resource`); `add_system(phase, name, fn, after)`; `tick(dt, tick_index,
    time_scale)` (Startup once, then the three fixed phases), `render(alpha)` (Render_Pre, Render_Submit),
    `run_phase`, `sync`; `state_hash(seed)`; `metrics()`; per-system profiling (`set_profiling`,
    `system_timings`).
  - `Phase`: `Startup`, `FixedUpdate_Pre`, `FixedUpdate_Physics`, `FixedUpdate_Post`, `Render_Pre`,
    `Render_Submit`, `Editor`. The list is closed; adding a phase is a design event.
  - `Frame`: the only context a system (`void (*)(Frame&)`) receives: `world()`, `commands()`, `events()`,
    `resource<T>()`, `query<Cs...>()`, `is_added`/`is_changed`, plus `tick`, `dt`, `time_scale`, `alpha`.
  - `Query<Cs...>`: `with<>`, `without<>`, `added<>`, `changed<>`, `since(tick)`, `each(fn)`, `count()`.
    Iteration is tables in creation order, rows in row order.
  - `Commands`: deferred `spawn`, `spawn_n`, `spawn_raw`, `destroy`, `add`, `remove`, `set`. Spawned handles
    are valid at once; the rows appear at the next sync point.
  - Events: `register_event<T>(producer_phase)`, `EventAccess::push` / `read`, `event_registry()` (counts,
    producers, consumers).
  - Change detection: `change_tick()`, `reader_tick()`, `is_added<T>/is_changed<T>(e, since)`, `touch<T>(e)`,
    `write_tick()`.
- `transform.h`: `LocalTransform` (position, rotation, scale; all `Save`), the pose of record, plus
  `rotation_z` / `angle_z` for 2D.
- `hierarchy.h`: `ChildOf{parent}`, engine-written `WorldTransform` (read-only, transient, with a composed
  `matrix`), `install_hierarchy` (registers the components, hooks and the `transform_propagate` system in
  `FixedUpdate_Post`), `propagate_transforms`, `set_parent`, `parent_of`, `children_of`, `depth_of`,
  `hierarchy_stats`, `compose`, `to_world`.

`mce_ecs_scene`
- `prefab.h`: `ecs::instantiate(World&, SceneView | payload bytes, SpawnParams, out)` and `ecs::load_scene`.
  `SpawnParams` has replica `count`, a `parent` for template roots, an `AssetResolveFn` that stamps asset
  handles, and `PrefabOverride`s (a `"<Component>.<property path>"` plus a typed value of at most 64 bytes,
  built with `PrefabOverride::of`).
- `save.h`: `SaveKey` (author-assigned stable identity), `save::write_save` / `read_save` (a `[Save]`-field
  reflection dump with `LoadStats`), `write_replay` / `read_replay` (input stream framed with start and final
  state hashes), and file helpers `save_game`, `load_game`, `write`, `read`, `set_root`, `default_root`,
  `path_of`.

## Rules and invariants

- Components are plain, trivially copyable, trivially destructible structs: no heap, no destructors. Row moves
  are `memcpy`, and `state_hash` hashes column bytes, so avoid padding. Empty structs are tags: they live in
  the mask only, with no column and no ticks. Reflected components use `MCE_COMPONENT` (see
  [refl.md](refl.md)).
- Gameplay code may hold only `Entity` across frames. Destroy bumps the generation, so stale handles fail and
  never alias.
- Nothing mutates table structure while a phase runs. Immediate structural calls (`spawn_with`, `add`,
  `remove`, `destroy`, `set` that adds a component, `set_parent`) assert inside a phase. Systems change
  structure through `Commands`, applied at the sync point that ends each phase: spawns first, then
  add/remove/set in recorded order, then destroys. A system does not see its own structural changes.
- Registration order within a phase is execution order. `after` names a system already registered in the same
  phase; it is asserted, never reordered.
- Events are double-buffered and swapped at the producer phase's sync point; readers never see a partially
  written list.
- Change ticks: each data column has parallel `added` and `changed` u32 tick columns. The counter advances
  once per system run, once per sync point that applies commands, and once per direct write outside a phase.
  Stamping writes are `set`/`add`, `Commands`, `try_get_mut`, `touch`, and mutable query items for every row
  visited. A pointer kept and written later is not seen; call `touch<T>()`. Comparison wraps, and `World`
  clamps stored ages below `kMaxChangeAge`.
- Determinism (FR-RUN-040): query order is table creation order then row order; `state_hash` covers the alive
  count and, per non-empty table, the component type hashes, entity handles and column bytes. Profiling
  timings are observation only and never hashed.
- Hierarchy: propagation seeds entities whose `LocalTransform` or `ChildOf` changed since the system's
  previous run (plus those lacking a `WorldTransform`), orders them by (depth, entity index), and walks
  depth-first. Destroying a parent destroys its subtree. A `ChildOf` that would close a cycle is rejected; the
  entity stays a root. `install_hierarchy` must run before any `FixedUpdate_Post` system that reads
  `WorldTransform`.
- Instantiation: every chunk column names its type by `mce_refl` name, and the World must have it registered;
  an unregistered type is `Errc::NotFound` naming the column, never a silent drop. On any error nothing is
  spawned. Inside a phase, instantiation records `Commands`. Nested prefabs are flattened by the cook; there
  is no runtime prefab inheritance.
- Saves: identity is `SaveKey`, not the `Entity` handle. Unknown types or fields from another module version
  are skipped and counted; a newer save format is `Errc::VersionTooNew`. Files are written atomically (temp
  file, previous save rotated to `.bak`, rename).
- Threading: the World is main-thread only. The sim and render phases run on the main thread through v0.5; the
  audio thread never touches the World (ecs-gameplay.md §4.1).

## How it is tested

ctest `test_ecs` (`tests/ecs/CMakeLists.txt`, links `mce_ecs`):
- `test_world.cpp`: "entities: generational handles never alias (FR-ENT-010)", archetype moves, "queries:
  with/without filters and deterministic order", hooks, resources, "systems: registration order is execution
  order; Startup runs once", "type-erased access: raw calls address the same component as the typed API".
- `test_commands.cpp`: sync-point visibility, recorded order, destroys after mutations, "spawn storm: 10k
  spawns in one tick apply in bulk at the sync point (FR-ENT-020)", "systems cannot see their own structural
  changes; immediate structural calls assert in a phase".
- `test_events.cpp`: "events: double-buffered, swapped at the producer phase's sync point (FR-ENT-030)", the
  event registry.
- `test_determinism.cpp`: "state_hash: identical inputs give identical hashes (FR-RUN-040)", "system
  profiling: per-system timings, off by default, never in the hash".
- `test_change_detection.cpp`: direct writes and `since()`, ticks carried through swap-remove and archetype
  moves, system reader ticks, Commands stamping at sync, "ticks survive counter wraparound".
- `test_hierarchy.cpp`: same-tick parent rotation, dirty subtrees only, re-parenting, cycle rejection, subtree
  destroy, "moving 1 of 5,000 entities re-composes exactly that entity (FR-ENT-040/050)".

ctest `test_ecs_scene` (links `mce_ecs_scene`, `mce_asset_cook`, `mce_input`):
- `test_prefab.cpp`: cook-time flattening, handle remap and overrides, replicas and a parent, scenes with
  prefab instances, re-cook of users after a prefab edit, cook errors (unknown prefab, cycles, empty),
  "runtime errors spawn nothing", "instantiation inside a phase goes through Commands".
- `test_save.cpp`: `[Save]` round-trip by `SaveKey`, other module versions, corrupt/truncated/newer payloads,
  atomic files with one `.bak`, "replay: the recorded input stream reproduces the live run's state hash".

Headless sample runs exercise the ECS end to end, for example `sample_02_boids_300`, `sample_04_cubes_300`,
`scene_demo_player_600` and `extras_2d_save_write` / `extras_2d_save_load`.

## Design records

- [../architecture/ecs-gameplay.md](../architecture/ecs-gameplay.md): entities and registration (§2.1), World
  (§2.2), queries (§2.3), systems and phases (§2.4), Commands (§2.5), events (§2.6), change detection (§2.7),
  hierarchy (§2.8), scene lifecycle (§2.10), prefabs (§2.11), saves (§2.13), internals (§3, including §3.9
  determinism machinery), threading (§4).
- [../ARCHITECTURE.md](../ARCHITECTURE.md) §9: ADR-0004 (archetype tables over sparse sets), ADR-0007
  (determinism enforced by CI), ADR-0009 (reflection as the metadata source).
- `docs/adr/0025-ecs-v05-change-ticks-hierarchy-prefabs.md`:
  change ticks, hierarchy, prefabs as scene chunks, cook-time flattening, `mce_ecs_scene`, `SaveKey` saves,
  replays.
- `docs/adr/0024-runtime-scene-path.md`: reflection-addressed type-erased
  World writes and `mce_scene`.
- `docs/adr/0021-game-module-loader-m3.md`: game modules and the shared
  engine instance.
- Milestones: `docs/milestones/M6-ecs.md`.

## Known gaps

- Two scene loaders exist: `scene::instantiate` in `mce_scene` (ADR-0024) and `ecs::instantiate` /
  `load_scene` here. They read the same chunk layout; folding `mce_scene` ([scene.md](scene.md)) onto
  `ecs::instantiate` is a follow-up (ADR-0025).
- Save file mechanics live in `mce_ecs_scene` although core.md §2.9 places them in core; they move when a
  second consumer below L2 needs them, such as input rebinding persistence (`save.h`, ADR-0025 §11,
  M6-ecs.md).
- The renderer's extract does not use `changed<WorldTransform>()` for interpolation yet (M6-ecs.md, ADR-0025).
- Per-subsystem hashers composed into `state_hash` are not done (ADR-0025, "Not in this change").
- Systems run serially on the main thread, and `mce::jobs` runs inline. The auto-parallel experiment
  (ecs-gameplay.md §4.1) was built, measured and not merged; see `docs/research/auto-parallel-scheduling.md`.
