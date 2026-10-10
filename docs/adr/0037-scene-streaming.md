# ADR-0037: Optional scene streaming and the asset loader thread

**Status:** Accepted (2026-10-10). **Milestone:** scene streaming (`docs/milestones/scene-streaming.md`).
**Requirements:** FR-ENT-120, promoted out of ROADMAP "M10+ — Explicitly deferred" on 2026-10-10. **Amends:**
ARCHITECTURE §6.1 (thread roster).

**Context.** The user asked for scene streaming with one constraint: it is optional, available to a game that
wants it and invisible to one that does not. Loading and unloading parts of a world is structural (`spawn`,
`destroy`), which the ECS forbids while a phase runs, and file reads should not stall the main thread. The design
documents had placed background IO on a platform "IO worker" thread that was never built.

## Decisions

1. **A chunk is data.** `SceneChunk` (scene reference, load and unload distances, `pinned`, `disabled`),
   `StreamingFocus` and `SceneChunkStatus` live in `mce/scene/streaming.h` and are registered in the cook, editor
   and MCP server like any reflected component. Streaming is authored in the editor or over MCP and needs no game
   code; a game module steers it by writing `pinned` / `disabled` and reads `SceneChunkStatus`. The gap between the
   load and unload distances is a hysteresis band.
2. **The host pumps, outside phases.** `install_streaming` adds the `SceneStreaming` resource; the host calls
   `pump_streaming` between ticks (`PumpMode::Settle` once after the base scene, then `Budgeted` before every
   `World::tick`). No system does it, because a phase cannot spawn or destroy structurally. `mce_player`,
   `PlaySession` and the MCP headless run all pump, so every host streams with no game code.
3. **One loader thread, in `mce_asset`.** `AssetSystem::request_async` / `poll_async` / `take_async` /
   `cancel_async` / `wait_async` (`mce/asset/asset_system.h`) hand dev-mount file reads to
   `AssetSystem::AsyncLoader`, one `std::thread` started on the first async request (`asset_system.cpp`). It reads,
   hashes and CRC-checks container files and never touches the asset tables; the main thread commits in
   `poll_async`. A pack is read whole at mount, so pack-backed requests are ready at once. Instantiation is spread
   over ticks instead (`max_instantiations_per_pump`, default 1).
4. **Sync when determinism matters, async when windowed.** Async completion depends on the disk, so
   `mce_player -headless`, play-in-editor and the MCP headless run set `async = false` (a wanted chunk loads inside
   the pump that asks); the windowed `mce_player` streams asynchronously. Sync streaming is a function of the pump
   sequence and passes the 3-run Debug/Release determinism stage.
5. **Free when unused.** The pump returns before touching the world when no `SceneChunk` was ever registered; every
   existing golden hash and the MCP transcript are unchanged.

## Consequences

- ARCHITECTURE §6.1 as built: the main thread, the SDL audio callback thread and, only once a game streams
  asynchronously, the asset loader thread. There is no platform IO worker; the job system runs serially; the cook
  and the editor file watch run on the main thread.
- Not built: chunk transforms offsetting their scene (chunk scenes are in world coordinates), splitting one large
  chunk across ticks, save games tracking chunks, and world-partition editor tooling.

## Tests

- `test_asset_system`: async loads pending until a poll, cancel in flight and when ready, a corrupt file failing
  with `CrcMismatch` on the loader thread.
- `test_scene`: load/unload with hysteresis, `pinned` / `disabled`, one instantiation per budgeted pump, missing
  scene → `Failed`, async loads landing on a later pump, sync determinism, a world without chunks keeping its hash.
- ctest `stream_demo_player_300` and `stream_demo_player_gpu`; the `determinism` stage runs stream_demo for 1,500
  ticks.
