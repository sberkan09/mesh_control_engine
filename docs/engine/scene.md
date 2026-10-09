# scene — turns a cooked scene chunk into entities in a World

`mce_scene` is an L2 library in `src/engine/scene/`. Its public dependencies are `mce_ecs`,
`mce_asset` and `mce_refl` (`src/engine/scene/CMakeLists.txt`). It is the runtime half of the
authored-scene path: `.mcscene` text is cooked to a `CHNK_SCENE` chunk, the pack is mounted, and
`mce_scene` writes the rows into a `World` (assets.md §6, FR-ENT-060). It is linked by
`mce_editor_core` (play-in-editor and the preview world), the `mce_engine` umbrella,
`mce_player_runtime` on platforms without game modules, and `test_scene` / `test_kit`. Callers in
the tree: `src/player/src/player.cpp`, `src/editor/core/src/play_session.cpp`,
`src/editor/core/src/editor_scene.cpp` and `src/editor/mcp/src/headless_run.cpp`.

The loader is reflection-driven. Component columns are matched to the process's `mce_refl` registry
and written type-erased, so a new reflected component needs no loader change (FR-DX-020).

## Public surface

One header, `include/mce/scene/scene.h`, namespace `mce::scene`.

- `instantiate(world, SceneView, spawned = nullptr, hierarchy = true)`: spawns one entity per scene
  entity and writes every column. `spawned[i]` is the entity created for scene entity `i`. When the
  scene has a `ChildOf` column and `hierarchy` is true, it calls `ecs::install_hierarchy` and
  `ecs::propagate_transforms`, so `WorldTransform` is composed immediately and kept by the
  hierarchy system afterwards. The editor's preview world passes `hierarchy = false` because it
  places children itself.
- `load(world, AssetSystem&, Guid, spawned = nullptr)`: finds the Scene asset by GUID, loads it
  synchronously, instantiates it, and releases the asset reference before returning.
- `component_for(world, TypeRecord&)`: returns the ECS component for a reflected type, registering
  it from the record when the game has not. The identity is the same as `World::component_id<T>()`
  (`TypeRecord::type_hash` equals `kTypeHash<T>`), and the reflection id is recorded so that prefab
  instantiation (`find_component_by_refl`) works in a world that only loaded a scene. An empty
  reflected struct registers as a tag.

The input type `asset::SceneView` and the chunk layout live in `mce_asset` (`mce/asset/formats.h`).

## Rules and invariants

- All-or-nothing: every column is validated before anything is spawned. A column whose type is not
  reflected as a component in this process fails with `Errc::NotFound`; a size or alignment mismatch
  against the cooked data fails with `Errc::InvalidArgument` and a "re-cook" log line. The world is
  not touched in either case.
- `EntityRef` fields are rewritten from scene-local indices to the spawned entities, using the
  chunk's entity fixups (sorted by column, row, offset). An index outside the scene becomes
  `Entity::null`.
- `AssetRef` fields keep their GUID with a null handle; `asset::AssetSystem::resolve(AssetRef&)`
  stamps the handle on first use.
- Fields omitted in the `.mcscene` keep their C++ defaults.
- Instantiating the same scene twice spawns a second, independent copy.
- A missing scene GUID returns an error, never crashes.
- Engine walls that apply: the engine container ban (no `std::vector`, `std::string`, iostreams
  under `src/engine/`). The runtime never parses JSON; scene text is read only by the cook library.

## How it is tested

ctest `test_scene` (no GPU), `tests/scene/test_scene.cpp`. It cooks a temporary project with
`cook::CookService`, mounts the output and loads it.

- "scene: cook -> mount -> instantiate stamps components and entity references": `LocalTransform`
  defaults, a `"$0"` entity reference stamped to the spawned entity, a null reference, a kit
  `Sprite` with an asset GUID, `kit::primary_camera`, and a second independent instantiation.
- "scene: a missing scene GUID is an error, never a crash".
- "scene: components registered from reflection unify with typed registration".

End to end:

- `scene_demo_player_600`: `mce_player -headless 1 -ticks 600` on the cooked `samples/scene_demo`
  pack, matched against "600 ticks, 6 entities, state_hash ...".
- GPU (label `gpu`, needs `MCE_GPU_TESTS`): `scene_demo_player_gpu`,
  `scene_demo_player_console_gpu`, and with the editor built `editor_scene_smoke` and
  `editor_play_smoke`.
- `test_editor` "play:" and "hierarchy:" cases exercise `instantiate` from the editor side.

## Design records

- [ADR-0024 The runtime scene path](../adr/0024-runtime-scene-path.md): reflection-addressed ECS
  writes, `mce_scene`, the starter kit and `mce_player`.
- [ADR-0025 ECS v0.5](../adr/0025-ecs-v05-change-ticks-hierarchy-prefabs.md), "Two loaders for now".
- [ADR-0030 M6 editor authoring](../adr/0030-m6-editor-authoring.md) §5: the runtime hierarchy.
- [ADR-0032 Sequence runner](../adr/0032-sequence-runner.md) decision 7: the reflection id fix.
- [Assets architecture](../architecture/assets.md) §6 scene serialization, §7.3 scene load path.
- [Editor catch-up milestone](../milestones/editor-catch-up.md) and [M5b](../milestones/M5b.md).

## Known gaps

- Two loaders exist (ADR-0025). `scene::instantiate` loads plain scenes and auto-registers
  components; `ecs::instantiate` / `load_scene` in `mce_ecs_scene` add replicas, overrides, a parent,
  asset stamping and in-phase use through Commands. Folding `mce_scene` onto `ecs::instantiate` is a
  recorded follow-up.
- `load` is synchronous only (assets.md §7.3 describes the sync MVP load path).
