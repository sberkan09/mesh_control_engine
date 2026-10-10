# Scene streaming (milestone record)

**Scope:** FR-ENT-120, promoted out of ROADMAP "M10+ — Explicitly deferred" by the user on 2026-10-10 with
one constraint: streaming is **optional**, available to a game that wants it and invisible to one that
does not. **Measured on:** the Linux CI container (4 cores), `scripts/ci/local_ci.sh`.

## What landed

| Item | Where | Evidence |
|---|---|---|
| Async asset loading: a loader thread reads, hashes and CRC-checks dev-mount containers; the main thread commits | `AssetSystem::request_async` / `poll_async` / `take_async` / `cancel_async` / `wait_async` (`mce/asset/asset_system.h`) | `test_asset_system`: async load (pending until a poll, upload at commit, resident and pack requests ready at once, bad type / key fail at once), cancel in flight and when ready (refcount back to 0), a corrupt file failing with `CrcMismatch` on the loader thread |
| Chunk components | `SceneChunk`, `StreamingFocus`, `SceneChunkStatus` (`mce/scene/streaming.h`), registered in the cook, editor and MCP server | the stream_demo scenes cook with them; the editor inspector and MCP `component_set` see them like any reflected component |
| Streaming state and pump | `SceneStreaming` resource, `install_streaming`, `pump_streaming(Budgeted / Settle)` (`src/engine/scene/src/streaming.cpp`) | `test_scene` streaming cases: load/unload with the hysteresis band, `pinned` / `disabled`, one instantiation per budgeted pump, chunk entity destroyed, scene GUID swapped, missing scene → `Failed`, async loads landing on a later pump with a cancel in flight, sync determinism, a world without chunks keeping its hash |
| Every host streams with no game code | `mce_player` (async windowed, sync headless), `PlaySession` and the MCP headless run (sync); each settles after the base scene and pumps before every `World::tick` | `stream_demo_player_300`: "300 ticks, 5 entities ... streaming 2/4 chunks loaded, 8 streamed entities, 3 loads, 1 unloads"; `stream_demo_player_gpu` (windowed, async) |
| Unchanged when unused | the pump returns before touching the world when no `SceneChunk` was ever registered | every existing golden hash and the MCP transcript (component counts) unchanged; "a world without chunks is untouched" |
| Determinism | sync streaming is a function of the pump sequence | `determinism` stage: stream_demo, 1,500 ticks (two full patrol legs, every area loaded and unloaded), 3 runs × Debug/Release, one hash |

## Demo

`samples/stream_demo`: a base scene (the traveler: camera, `StreamingFocus`, a looping patrol sequence; four
`SceneChunk` entities 20 m apart) and one scene per area. At tick 300 the traveler is at x = 27: area 1 holds
it, area 2 is 3 m ahead and in, area 0 is 17 m behind and out.

## Design notes

- **Data first.** A chunk is an entity with a `SceneChunk`, so streaming is authored in the editor or over MCP
  like any component and needs no game code. Game modules steer it by writing `pinned` / `disabled` and read
  `SceneChunkStatus`.
- **Pump outside phases.** Loading and unloading are structural (`spawn`, `destroy`), which the ECS forbids
  while a phase runs, so the host pumps between ticks instead of a system doing it.
- **Async where it is real.** A pack is read whole at mount, so only dev-mount file reads leave the main
  thread; instantiation is spread instead (`max_instantiations_per_pump`, default 1 chunk per tick).
- **Determinism is a mode, not a hope.** Async completion depends on the disk, so determinism runs set
  `async = false`; play-in-editor does too, because tools step it tick by tick and expect a wanted chunk to be in.

## Not built

- Chunk scenes are in world coordinates; a chunk entity's transform does not offset its scene.
- One chunk instantiates in one go; a very large chunk is not split across ticks.
- Save games do not track chunks (a `SaveKey` inside a chunk scene is unsupported).
- World-partition editor tooling (cutting one big scene into chunks) stays deferred.
