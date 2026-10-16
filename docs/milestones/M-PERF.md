# M-PERF — Performance tooling for games and agents (milestone record)

**Status:** active (added 2026-10-15; steps 1–2 done). Plan: ROADMAP §M-PERF; Linear SBA-113 … SBA-117.
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
| 2 | Budgets per target in the project and static `project_check` rules | SBA-114 | done |
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

## Step 2 — budgets per target

**What.** `mce/perf_budget.h` (mce_core): `perf::Target` (desktop, mobile, web), `perf::Budget` and its
defaults, the fields by name (`budget_fields`), and `perf::judge(Summary, Budget)`. It gives one line per limited,
measured field: times on their p95, counters on their worst frame, and `pass`. A headless capture has no GPU
time, so the GPU limit gets no line instead of a pass. The `.mceproj` takes `"budgets": {"mobile": {...}}`.
`editor::check_budgets` validates it (`project_set` refuses an unknown target, an unknown field, a fraction
where a count goes, a negative time). `editor::project_budget` lays the file's fields over the defaults, and
`project_get` returns all three as `budgets_in_force`.

**Static rules.** `project_check` takes `target` (default desktop). The per-tick sim budget is now the target's
`sim_ms`; `budget_ms` still overrides it. Two warnings read the files without running anything:
- An image larger than `texture_size` on a side (`cook::image_size` reads the header only).
- A scene whose `Sprite`s name more distinct textures than `sprite_textures`, since each one is a separate
  batch and draw. The fix points at a spritesheet.

The result's `performance` block carries `target` and the `budget`.

**Defaults.** 60 fps on desktop and web, 30 on a phone. Draws 3000 / 1000 / 400, triangles 3M / 1M / 500k,
largest image 8192 / 4096 / 2048, distinct sprite textures 64 / 32 / 16, and 4 ms of sim per frame everywhere.
They are starting points for a mid-range phone and integrated graphics in a browser, not measurements. The
device session should tune them.

**Gates.** `test_core` (perf): targets and fields by name; judge passes at the limit, names what is over,
and leaves out an unmeasured GPU. `test_mcp_check` (M-PERF budgets): the defaults; a patch overrides only
what it names; four bad patches leave the file byte-identical. For mobile with tightened limits, the demo's
32-pixel textures and its three sprite textures are flagged with their numbers. The desktop defaults flag
nothing, and an unknown target is refused.

**Known limits.** The static rules see `Sprite` components written in the scene file, not ones game code
or prefab instances add. A capture's verdict over MCP comes with `perf_run` (step 3).
