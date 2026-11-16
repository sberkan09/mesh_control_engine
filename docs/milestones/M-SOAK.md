# M-SOAK — Long runs that must stay flat (milestone record)

**Status:** done 2026-11-11 (steps 1 to 4). **Owner: the "Geliştirme rutini 3" thread.** Added 2026-11-10 on the coordinator's instruction to open a new stability milestone once the hardening sweeps (M-HARDEN3 to M-HARDEN6) ran dry. No Linear issues (the Backlog holds only the device-bound SBA-112 and SBA-58); this record is the tracker.

**Why.** Every test the repository has runs for seconds: a few hundred ticks, a few frames. A leak of one allocation per tick, a queue that never drains or a cache that never evicts passes all of them and shows up only after an hour in the player. `-perf_capture` already records the live heap allocation count of every frame (`PerfCounters::heap_allocations`, M-PERF), a number that does not depend on the machine. A soak gate compares that count after the warm-up with the count at the end of a long run.

**Scope.** Player and editor stability, device-free and GPU-free, test-only unless a gate finds a leak. Outside the modules the other threads own (render, rhi, physics3d, openworld, kit, GTA gameplay, weather). A fix needs its own regression gate.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `player_soak`: `mce_player -headless` runs 1500 ticks over a physics scene with a capture; the live heap allocation count of the last frame stays within 8 of the first frame after the 100-frame warm-up (`tests/perf/player_soak_test.cmake`) | done 2026-11-10: 300 allocations after the warm-up and 300 after 1400 more frames |
| 2 | The same gate over three shipped packs under `mce_player`: `scene_demo_soak`, `extras_2d_soak` and `stream_demo_soak` (the script takes `PACK`, `SCENE`, `TICKS`, `SLACK`); 1500 ticks each, about 2 s, so no `slow` variant | done 2026-11-10: scene_demo 353 → 353, extras_2d 367 → 367, stream_demo 365 → 368 (three chunks streamed in, inside the slack) |
| 3 | Editor soak (`tests/editor/test_editor_soak.cpp`, in-process over `EditorSession`): 200 rounds of open a scene, create and destroy entities, undo everything and switch to another scene; the live heap allocation count after round 2 and after round 202 differ by at most 8 | done 2026-11-10: 69 and 69 |
| 4 | A real-GPU soak, run by hand on a rented card (the pod machinery of `M-GPU-VERIFY.md`, job files only, nothing merged): `mce_player` windowed at 1280 x 720 High, 60000 frames after a 300-frame warm-up, on GL and on sdlgpu, over `tour_caustics` and `tour_river` (water, caustics and the post chain), with `nvidia-smi` sampling the card every 2 s | done 2026-11-11: no leak (see the step 4 notes) |

## Notes

- Step 1 uses a synthetic scene (a ball on a floor) because the capture flag lives in `mce_player`; the sample executables have their own mains and no `-perf_capture`.
- The 8-allocation slack covers caches that fill once (the first contact pair, the log's ring). A gate that needs more slack than that points at a real growth and should be looked at, not loosened.
- Step 3 runs inside `test_editor` instead of over MCP: the live count is a process-wide number and an in-process test reads it directly, where an MCP run would need a new tool to report it.
- Asset hot reload is covered by a second test in `test_editor_soak.cpp` (200 edits of a mounted material, each one re-cooked and swapped in); it stayed flat, so it found no leak.
- Play-in-editor start and stop is covered by a third test in `test_editor_soak.cpp` (100 rounds of enter, 30 ticks, stop on the sample scene); flat as well.
- Step 4 (2026-11-11), one secure-cloud RTX 4090 (driver 595.91.07, 16 vCPUs, $0.89/h; the community cards and the A5000 had no stock), pod `6g1bamikrlrcak`, about 30 minutes, about $0.45, terminated (`list-pods` empty afterwards). Four runs of 60000 frames each (GL and sdlgpu over `tour_caustics` and `tour_river`): 0 GL errors, exit 0 on all four; the live heap allocation count is the same in the first and the last frame (GL 415 / 415 and 414 / 414, sdlgpu 436 / 436 and 437 / 437, at most one more in between); the card's memory is flat (sdlgpu 262 MiB from start to end, GL 302 MiB). A second pass of `tour_river` (sdlgpu, GL, sdlgpu, GL) gave the same numbers.
- The one thing that moved is the sdlgpu frame time: 2.5 to 2.8 ms in the first tenth of a run and 3.1 to 3.6 ms in the last (GL stays at 0.55 ms the whole run). It is the card's clock, not growth in the engine: `nvidia-smi` shows the SM clock falling from about 2100 MHz to 900 to 1140 MHz during the sdlgpu runs (temperature 39 to 43 C, so not thermal), while GL holds 2805 MHz. The sdlgpu frame is mostly waiting, the driver lowers the clock, and the wait gets longer. A gate on frame time over a long run would therefore need a pinned clock, which a rented pod does not allow; the heap and memory numbers are the gate.
- Harness notes for the next round: the first `cmake --preset` died three times in a row on the astcenc `FetchContent` clone ("could not read Username for https://github.com", from the pod's address) and worked by hand a minute later, so the build job retries the configure with pauses; the HTTP proxy of the pod (`<pod id>-8000.proxy.runpod.net`, port 8000/http, `python3 -m http.server` over `/work/out`) carries the results off the pod where `stream-pod-logs` is too verbose.
- Not covered, open for a later step: a Metal run on a Mac, and a phone.
