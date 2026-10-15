# M-PERF — Performance tooling for games and agents (milestone record)

**Status:** active (added 2026-10-15; step 1 done). Plan: ROADMAP §M-PERF; Linear SBA-113 … SBA-117.
**Why:** a game made with MCE, by a person or by an agent, has no way to answer "where does my frame go, and
does it fit the phone or the browser?". Today's parts are scattered and editor-only: the editor's Profiler
panel (`PerfMonitor`, FR-EDT-140), `World::set_profiling` per-system timings, Forward3D's GPU stage
timestamps, `-r.frame_report` (wall time only) and `project_check`'s sim-per-tick budget. The shipped player
cannot record a profile, the MCP server cannot measure a rendered frame, and nothing states a budget per
target. Unity, Godot and Unreal all ship a frame profiler with captures. M-PERF joins the parts into one
capture format, budgets per target, an MCP tool an agent can loop on, and a counter gate in `local_ci.sh`.
It needs no device: timings are measured on whatever machine runs the capture, and the gates use counters
(draw calls, triangles, allocations, bytes), which do not depend on the machine.

Not in scope: remote profiling of shipped games and telemetry (FR-DBG-080, N7 stay Won't); a Tracy
replacement (Tracy stays the developer's deep-dive tool).

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | `.mcperf` captures from the stock player: CPU stages, sim phases and systems, GPU passes, render counters, memory | SBA-113 | done: headless and windowed, hash-neutral |
| 2 | Budgets per target in the project and static `project_check` rules | SBA-114 | not started |
| 3 | MCP `perf_run` and `perf_compare` | SBA-115 | not started |
| 4 | The editor's Profiler panel reads captures: frame strip, frame breakdown, compare | SBA-116 | not started |
| 5 | Counter gate in `local_ci.sh`, user guide chapter, demo | SBA-117 | not started |

**Demo:** an agent, over MCP alone, runs `perf_run` on a sample with a planted hotspot (a system that walks
every entity per entity, and a scene with an unbatched texture per sprite), reads the report, fixes both, and
shows `perf_compare` before and after within the mobile budget.

**Gates:** a capture never changes the sim hash; the same run gives the same counters on two machines; a
seeded over-budget scene fails the budget verdict; the `local_ci.sh` counter stage catches a seeded draw-call
regression; `local_ci.sh` green on every step.

## Step 1 — `.mcperf` captures from the stock player

**What.** `mce_player -perf_capture <file>` (and `-perf_warmup N`) writes a JSON capture (`"format": "mce-perf"`,
version 1) at exit, and prints a one-line summary. The format and the summary live in `mce/perf_capture.h`
(`mce_core`, no dependencies), so the MCP server and the editor read the same thing the player writes.

**What a frame holds.** The wall time since the previous frame started; the CPU time of the ticks run since
then (`world_.tick` only) and of the frame itself (render systems, extract, record, submit, including any wait
for the GPU in `begin_frame`); system time per ECS phase, from `World::set_profiling` totals differenced each
frame; the GPU time and Forward3D's 14 stages (or the first-to-last timestamp span for a 2D frame), attached to
the frame they were written in when they complete frames later; and the counters: RHI draws, passes,
dispatches, ring overflows and upload bytes, 3D triangles and meshes after culling, 2D quads and batch groups,
live heap allocations. Per system: runs, total and the worst single frame over the captured frames.

**Summary.** avg/p50/p95/p99/max (nearest rank) of frame, sim, render and GPU time, the max and mean of every
counter, and up to 16 spikes: frames at least twice the median and 1 ms over it. A spike's cause is the part
that grew most over its median (sim, render or GPU; none over 0.5 ms is "wait"), then within it the phase or
GPU stage that grew most: "sim: FixedUpdate_Post", "gpu: shadow". `render_phases` tells sim phases from
render ones.

**Gates.** `test_core` (perf): percentiles skip unmeasured values, counters, the three spike causes and a
render spike looking at render phases only, the JSON text. `player_perf_capture`: a headless run with a capture
prints the same state hash as one without, and its file parses (CMake `string(JSON)`) with 100 frames after a
20-frame warm-up starting at tick 21 and the physics system's 100 runs. By hand on MarbleGarden (offscreen,
llvmpipe, 320x180): two runs give the same counters (107 draws, 1230 triangles, 15 passes), GPU stages filled,
and frame times differ as wall time does.

**Known limits.** Memory is the live heap allocation count; GPU memory and asset bytes are not counted yet.
Frame counts by tick depend on pacing, so a windowed run's ticks per frame vary; the counter gate (step 5) runs
with a fixed step. The last few frames have no GPU time (still in flight at exit).
