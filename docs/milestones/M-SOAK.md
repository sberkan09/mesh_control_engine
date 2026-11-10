# M-SOAK — Long runs that must stay flat (milestone record)

**Status:** done 2026-11-10 (steps 1 to 3). **Owner: the "Geliştirme rutini 3" thread.** Added 2026-11-10 on the coordinator's instruction to open a new stability milestone once the hardening sweeps (M-HARDEN3 to M-HARDEN6) ran dry. No Linear issues (the Backlog holds only the device-bound SBA-112 and SBA-58); this record is the tracker.

**Why.** Every test the repository has runs for seconds: a few hundred ticks, a few frames. A leak of one allocation per tick, a queue that never drains or a cache that never evicts passes all of them and shows up only after an hour in the player. `-perf_capture` already records the live heap allocation count of every frame (`PerfCounters::heap_allocations`, M-PERF), a number that does not depend on the machine. A soak gate compares that count after the warm-up with the count at the end of a long run.

**Scope.** Player and editor stability, device-free and GPU-free, test-only unless a gate finds a leak. Outside the modules the other threads own (render, rhi, physics3d, openworld, kit, GTA gameplay, weather). A fix needs its own regression gate.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `player_soak`: `mce_player -headless` runs 1500 ticks over a physics scene with a capture; the live heap allocation count of the last frame stays within 8 of the first frame after the 100-frame warm-up (`tests/perf/player_soak_test.cmake`) | done 2026-11-10: 300 allocations after the warm-up and 300 after 1400 more frames |
| 2 | The same gate over three shipped packs under `mce_player`: `scene_demo_soak`, `extras_2d_soak` and `stream_demo_soak` (the script takes `PACK`, `SCENE`, `TICKS`, `SLACK`); 1500 ticks each, about 2 s, so no `slow` variant | done 2026-11-10: scene_demo 353 → 353, extras_2d 367 → 367, stream_demo 365 → 368 (three chunks streamed in, inside the slack) |
| 3 | Editor soak (`tests/editor/test_editor_soak.cpp`, in-process over `EditorSession`): 200 rounds of open a scene, create and destroy entities, undo everything and switch to another scene; the live heap allocation count after round 2 and after round 202 differ by at most 8 | done 2026-11-10: 69 and 69 |

## Notes

- Step 1 uses a synthetic scene (a ball on a floor) because the capture flag lives in `mce_player`; the sample executables have their own mains and no `-perf_capture`.
- The 8-allocation slack covers caches that fill once (the first contact pair, the log's ring). A gate that needs more slack than that points at a real growth and should be looked at, not loosened.
- Step 3 runs inside `test_editor` instead of over MCP: the live count is a process-wide number and an in-process test reads it directly, where an MCP run would need a new tool to report it.
- Not covered, open for a later step: the GPU paths (a windowed player), asset hot reload, and play-in-editor start and stop.
