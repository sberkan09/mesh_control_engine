# scene — turns a cooked scene chunk into entities in a World

`mce_scene` is an L2 library in `src/engine/scene/`. Its public dependencies are `mce_ecs`,
`mce_asset` and `mce_refl` (`src/engine/scene/CMakeLists.txt`). It is the runtime half of the
authored-scene path: `.mcscene` text is cooked to a `CHNK_SCENE` chunk, the pack is mounted, and
`mce_scene` writes the rows into a `World` (assets.md §6, FR-ENT-060). It is linked by
`mce_editor_core` (play-in-editor and the preview world), the `mce_engine` umbrella,
`mce_player_runtime` on platforms without game modules, the `mce_cook` CLI (`tools/cook`, for the
reflected streaming components) and `test_scene` / `test_kit`. Callers in
the tree: `src/player/src/player.cpp`, `src/editor/core/src/play_session.cpp`,
`src/editor/core/src/editor_scene.cpp` and `src/editor/mcp/src/headless_run.cpp`.

The loader is reflection-driven. Component columns are matched to the process's `mce_refl` registry
and written type-erased, so a new reflected component needs no loader change (FR-DX-020).

## Public surface

Loading, `include/mce/scene/scene.h`, namespace `mce::scene`:

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

- Scene changes: `request_change(world, target)` (inline; a source path, `"guid:<uuid>"` or a
  `Guid`) records the target in the `SceneChange` world resource (`next`, `current` source path,
  `current_guid`, `changes`), which `load` creates and keeps current. `apply_change(world, assets,
  spawned)` is the host's call between ticks: with nothing pending it returns false; otherwise it
  resolves the target (`find_scene`), unloads every streamed chunk, `World::destroy_all()`, loads the
  target and `World::rerun_startup()`. `find_scene(assets, want)` resolves GUID text or a source path
  (a leading `assets/` is ignored) to a Scene GUID.

The input type `asset::SceneView` and the chunk layout live in `mce_asset` (`mce/asset/formats.h`).

Streaming, `include/mce/scene/streaming.h` (FR-ENT-120, opt-in):

- Components: `SceneChunk` (an `AssetRef<Scene>`, a box `half_extents` around the entity's position,
  `load_distance`, `unload_distance`, `pinned`, `disabled`), `StreamingFocus` (`enabled`) and the
  pump-written `SceneChunkStatus` (`state`, a `ChunkState`: `Unloaded`, `Loading`, `Loaded`, `Failed`;
  `entity_count`). All three are registered in the cook,
  the editor and the MCP server (`engine_components.cpp`).
- `install_streaming(world, assets, StreamingConfig)` adds the `SceneStreaming` resource;
  `pump_streaming(world, PumpMode)` runs it, once per tick, outside phases (it spawns and destroys).
  `Budgeted` follows the config: `async` requests go through `AssetSystem::request_async` and at most
  `max_instantiations_per_pump` chunks instantiate per pump, with at most `max_loads_in_flight`
  (default 4) async reads queued at once; with `async` off a wanted chunk loads in the same pump
  (still budgeted). `Settle` loads everything wanted now, synchronously.
- `SceneStreaming::entities(chunk)` lists what a loaded chunk spawned; `stats()` counts chunks,
  states, streamed entities, loads and unloads; `config()` returns the mutable `StreamingConfig`;
  `unload_all(world)` destroys every streamed entity and drops pending loads (chunks go back to
  `Unloaded`); `set_assets` repoints the resource at another `AssetSystem`, which must outlive the
  World. `distance_to_box` is the distance rule.
- Hosts: `mce_player` (async when windowed, sync when headless), `PlaySession` and the MCP headless
  run (sync) install it, settle after the base scene and pump before every `World::tick`. The same
  three hosts call `apply_change` after every `World::tick` (the player settles streaming after a
  change); a failed change ends a headless run with the error and `mce_player` with exit code 1.

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
- A scene change keeps resources and systems; every entity goes, including ones spawned at run
  time, and Startup runs again on the next tick. An unknown target is an error that leaves the world
  untouched. The cook treats a scene's `AssetRef<Scene>` as a link (`DepRef::link`): type checked,
  but no ordering edge, so scenes may reference each other in a loop.
- Streaming: a chunk loads when any enabled focus is within `load_distance` of its box and unloads
  when every focus is beyond `max(unload_distance, load_distance)`; `disabled` wins over `pinned`.
  Unloading destroys only the entities the chunk spawned (those still alive). A destroyed chunk
  entity, or one that lost its `SceneChunk`, unloads on the next pump; a changed `scene` GUID swaps
  the contents. A failed chunk (missing GUID, bad scene) is marked `Failed` and retried after the
  focus leaves and returns. Ready async loads instantiate in request order. With `async` off the
  outcome is a function of the pump sequence alone (the determinism test).
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

`tests/scene/test_streaming.cpp` (same binary) cooks a base scene with three chunks:

- "streaming: chunks load near the focus and unload past the hysteresis band": settle, walk to the
  next chunk, the band between the two distances, `pinned` and `disabled`.
- "streaming: budgeted pumps instantiate one chunk at a time; removed chunks take their entities",
  including a changed `scene` GUID.
- "streaming: a chunk whose scene is missing fails without touching the world".
- "streaming: async loads read off-thread and land on a later pump", including a cancel in flight.
- "streaming: synchronous streaming is deterministic" and "a world without chunks is untouched".
- "scene change: request_change swaps every entity for the target scene and reruns Startup" (by path
  and by GUID text, an unknown target, a run-time entity, a kept resource) and "scenes that name each
  other in a loop cook". `test_mcp_playtest` checks a game's change in an MCP run, with the `.mctest`
  `scene` check and a target the pack lacks.

`test_asset_system` covers `request_async` / `take_async` / `cancel_async` on dev and pack mounts and
a corrupt file read on the loader thread.

End to end:

- `scene_demo_player_600`: `mce_player -headless 1 -ticks 600` on the cooked `samples/scene_demo`
  pack, matched against "600 ticks, 6 entities, state_hash ...".
- GPU (label `gpu`, needs `MCE_GPU_TESTS`): `scene_demo_player_gpu`,
  `scene_demo_player_console_gpu`, and with the editor built `editor_scene_smoke` and
  `editor_play_smoke`.
- `test_editor` "play:" and "hierarchy:" cases exercise `instantiate` from the editor side.

## Design records

- ADR-0024 The runtime scene path (`docs/adr/0024-runtime-scene-path.md`): reflection-addressed ECS
  writes, `mce_scene`, the starter kit and `mce_player`.
- ADR-0025 ECS v0.5 (`docs/adr/0025-ecs-v05-change-ticks-hierarchy-prefabs.md`), "Two loaders for now".
- ADR-0030 M6 editor authoring (`docs/adr/0030-m6-editor-authoring.md`) §5: the runtime hierarchy.
- ADR-0032 Sequence runner (`docs/adr/0032-sequence-runner.md`) decision 7: the reflection id fix.
- [Assets architecture](../architecture/assets.md) §6 scene serialization, §7.3 scene load path.
- Editor catch-up milestone (`docs/milestones/editor-catch-up.md`) and M5b (`docs/milestones/M5b.md`).

## Known gaps

- Two loaders exist (ADR-0025). `scene::instantiate` loads plain scenes and auto-registers
  components; `ecs::instantiate` / `load_scene` in `mce_ecs_scene` add replicas, overrides, a parent,
  asset stamping and in-phase use through Commands. Folding `mce_scene` onto `ecs::instantiate` is a
  recorded follow-up.
- `load` is synchronous only (assets.md §7.3 describes the sync MVP load path); streaming is the async
  path.
- Streaming does not offset a chunk scene by its chunk entity's transform (chunk scenes are in world
  coordinates), does not split one chunk's instantiation across ticks, and is not tracked by save
  games (a `SaveKey` inside a chunk scene is unsupported).
