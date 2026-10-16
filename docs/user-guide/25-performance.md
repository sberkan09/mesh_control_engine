# 25. Performance

Two questions come up for every game: where does the frame go, and does it fit the phone or the
browser? MCE answers both from one file. The player records a **capture** (`.mcperf`), a target's
**budget** judges it, and the same capture opens in the MCP tools, in the editor and in a CI gate.
None of this needs a device. Times are the machine's own, but the counters (draw calls, triangles,
sprites, batches) are the same on every machine, so they are what the gates hold.

## 25.1 Recording a capture

Any run of the player can record one:

```
build/linux-gcc-debug/src/player/mce_player -pack cooked -scene <guid> -perf_capture run.mcperf -perf_warmup 30
```

The player skips the warm-up frames, records every frame after it, and writes the file when it
quits. It prints a summary line: `perf capture run.mcperf: 300 frames, frame p50 ... p95 ...`. A
capture never changes the simulation: the state hash is the same with or without one.

Each frame records:
- the wall time, the sim's CPU time, the render's CPU time and, with timestamp queries, the GPU time;
- the time per ECS phase, and the GPU time per stage of the 3D renderer (shadows, the main pass, post);
- the counters: draws, passes, compute dispatches, triangles, meshes, sprites, sprite batches,
  upload bytes and live heap allocations.

The run as a whole records each system's total time and its worst frame.

For counters that repeat from run to run, run one sim tick per frame:
`-sim.fast_forward 1 -sim.max_catchup 1 -r.vsync 0`. MCP's `perf_run` always does this. A headless
run (`-headless 1 -ticks N`) records the sim alone, which is the quickest way to find a slow system.

The file is JSON (`"format": "mce-perf"`, version 1). It holds the header, a summary (p50, p95, p99
and max of each time, the counters' max and mean, and the spike frames with their cause) and one
line per frame.

## 25.2 Budgets per target

A budget is the most a frame may cost on a target: `desktop`, `mobile` or `web`. The engine's defaults:

| | desktop | mobile | web |
|---|---|---|---|
| `frame_ms` | 16.7 | 33.3 | 16.7 |
| `sim_ms` | 4 | 4 | 4 |
| `render_ms` | 6 | 8 | 6 |
| `gpu_ms` | 12 | 25 | 12 |
| `draws` | 3000 | 400 | 1000 |
| `triangles` | 3 M | 500 k | 1 M |
| `sprites` | 50 k | 10 k | 20 k |
| `upload_bytes` | 32 MB | 4 MB | 8 MB |
| `sprite_textures` (per scene) | 64 | 16 | 32 |
| `texture_size` (largest edge) | 8192 | 2048 | 4096 |

A project changes any of these in its `.mceproj` under `"budgets"` (chapter 2.2); fields it leaves out
keep the defaults. Times are judged on p95, and an unmeasured time (no GPU timer, a headless run)
gets no line rather than a pass. Counters are judged on their worst frame.

`project_check` with a `target` applies the static part before anything runs. It flags images larger
than `texture_size` and scenes whose sprites use more than `sprite_textures` different textures, since
each texture is its own draw batch.

## 25.3 Measuring over MCP

`perf_run` cooks the project, runs the real player on a scene for N frames at one tick per frame, and
returns:
- the verdict against the target's budget, with a sentence of advice for each limit that is over;
- the times, the counters, the slowest systems, the GPU stages and the spike frames.

Its capture goes to `cooked/.perf/last.mcperf`, and the previous one moves to `previous.mcperf`.
`perf_compare` with no arguments compares those two. It lists what got better or worse: a time
counts when it moved at least 10% and 0.1 ms, and a counter on any change. Chapter 14 ("Is it fast
enough?") has the arguments.

The loop for an agent or a person: `perf_run` with the target, read what is over and what the
advice and `top_systems` name, change one thing, `perf_run` again, `perf_compare`.

## 25.4 Reading a capture in the editor

The Profiler panel has two tabs. **Live** shows the editor's own frames. **Capture** opens a
`.mcperf`, by default `cooked/.perf/last.mcperf`; the path is relative to the project or absolute.
`mce_editor -editor.perf_capture <file>` opens one at start-up. The tab shows:

- the scene, the backend, the size and the frame count, the verdict for the chosen target and each
  limit that is over;
- p50 and p95 of frame, sim, render and GPU time;
- the frame strip: one bar per frame, spikes red, frames over the budget amber, the budget as a line.
  Hover a bar for its time and draws; click it to pick the frame. The worst spike is picked on open;
- the picked frame taken apart: its sim phases, render phases and GPU stages, slowest first, and its
  counters;
- the spikes with their cause (`sim: FixedUpdate_Pre`, `render: ...`, `gpu: shadow`, or `wait` when
  nothing measured grew), and the slowest systems in ms per frame.

**Compare with** opens a second capture as the "before" (default `previous.mcperf`) and adds the
`perf_compare` tables: green for better, red for worse.

## 25.5 A counter gate in CI

`scripts/ci/perf_gate.py` stops a change from adding draw calls by accident. A project lists the
scenes to measure, and the counters each one had, in `perf/counters.json`:

```json
{"runs": [{"scene": "scenes/level1.mcscene", "frames": 60, "warmup": 10, "width": 480, "height": 270,
           "counters": {"draws": 10, "passes": 2, "dispatches": 0, "triangles": 0, "meshes": 0,
                        "sprites": 132, "sprite_batches": 9}}]}
```

```
python3 scripts/ci/perf_gate.py --mcp build/linux-gcc-debug/projects/<Project>/<game>_mcp --project projects/<Project>
```

The gate runs `perf_run` on a fresh copy of the project for each run. It fails when any held counter
is above its baseline: draws, passes, dispatches, triangles, meshes, sprites or sprite batches. A
counter below its baseline passes, with a note. After a change that should move the counters, review
the numbers and write them with `--update`; the baseline change shows in the diff. Upload bytes and
heap allocations are printed but not held, and times are never judged.

Glowmoth and Marble Garden register the gate as ctest tests (`glowmoth_perf_counters`,
`marble_garden_perf_counters`, label `gpu`), so `local_ci.sh` runs it. `glowmoth_perf_counters_seeded`
plants three sprites with a texture each in the copy and passes only when the gate fails on draws.
That test proves the gate can fail. For a project of your own, copy the `add_test` lines from
`projects/Glowmoth/CMakeLists.txt`.

The baselines come from `local_ci.sh`'s GL run (Mesa llvmpipe). Counters do not depend on the
machine, but another RHI backend can count passes or batches differently.

## 25.6 Common fixes

| What is over | What usually fixes it |
|---|---|
| `draws`, `sprite_batches` | Sprites batch by texture: pack small images into one spritesheet (`.mcsheet`) so they share one. Fewer materials; merge static meshes |
| `sim_ms` | `top_systems` names the system. Look for a walk over every entity inside a walk over every entity: bucket into a grid, or keep the answer between ticks. Fewer physics bodies |
| `render_ms` | The CPU side of drawing: fewer things to extract (cull, hide what is off screen), fewer batches |
| `gpu_ms` | `gpu_stages` names the pass: lower or turn off the costly effect (`Environment3D`: shadows, SSAO, SSR, bloom) |
| `triangles` | Lower-poly models; fewer meshes in view |
| `upload_bytes` | Fewer moving sprites or dynamic meshes rewritten each frame |
| `texture_size` (`project_check`) | Downscale the image in its import settings, or split it |
