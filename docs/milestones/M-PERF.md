# M-PERF — Performance tooling for games and agents (milestone record)

**Status:** done on the desktop (added 2026-10-15; steps 1–5, the demo and the gates). Plan: ROADMAP §M-PERF; Linear SBA-113 … SBA-117.
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
| 3 | MCP `perf_run` and `perf_compare` | SBA-115 | done |
| 4 | The editor's Profiler panel reads captures: frame strip, frame breakdown, compare | SBA-116 | done |
| 5 | Counter gate in `local_ci.sh`, user guide chapter, demo | SBA-117 | done |

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

## Step 3 — `perf_run` and `perf_compare`

**What.** Two MCP tools in `tools_perf.cpp`.

`perf_run` cooks the project and runs the player on a scene with `-perf_capture`. It returns:
- the verdict against the target's budget (`perf::judge`), with a sentence of advice for each limit it is over;
- the time stats, both counter sets, the slowest systems (ms per frame and the worst frame), the measured GPU
  stages by p50, and the spikes.

The capture goes to `cooked/.perf/last.mcperf`, and the one before it moves to `previous.mcperf`. `save` also
keeps it at a path in the project, and that run still counts as the last one.

`perf_compare` reads two captures (by default the last two). It returns:
- p50/p95 before and after;
- the worst frame's counters with their change;
- the systems whose ms per frame moved most;
- `better` and `worse`: a time counts when it moved at least 10% and 0.1 ms, a counter on any change, since
  counters repeat;
- the after capture's verdict.

`editor::read_mcperf` / `load_mcperf` (editor core) parse a `.mcperf` back into a `perf::Capture`. The summary is
recomputed from the frames; step 4 uses this too.

**One tick per frame.** The windowed run passes `-sim.fast_forward 1 -sim.max_catchup 1`, so frame N shows tick N
on any machine and the counters repeat. The run also turns off vsync, audio, saved settings and the focus pause.
The software rasterizer is used where there is no display, as `render_frame` does; the shared spawn code
(`find_player`, `spawn_player`, `script_text`) moved out of `tools_render.cpp`'s private namespace. `headless:
true` runs `-headless 1 -ticks N`: the sim alone, with no draws and no GPU time.

**Gates.** In `test_mcp_agent`:
- Headless case: argument errors are refused before anything runs. A mobile run returns 60 frames, no GPU time,
  a sim line in the verdict and the physics phase among the systems. A second run saved under `perf/` still
  feeds the default compare, and its counters are unchanged. A file that is not a capture is refused.
- `gpu` case (offscreen llvmpipe): two runs give identical `counters_max`. Three sprites with a texture each
  raise the draws, and `perf_compare` lists `draws` under `worse`.

**Known limits.** The advice is per metric, not per scene. "Which sprites share no texture" stays with
`project_check` (step 2).

## Step 4 — the Profiler panel reads captures

**What.** The Profiler panel has two tabs. **Live** is the old panel (`PerfMonitor`, FR-EDT-140). **Capture**
opens a `.mcperf` (default `cooked/.perf/last.mcperf`, the path relative to the project or absolute) and shows:
- the scene, backend, size and frame count, and the verdict against the chosen target's project budget
  (`project_budget` + `perf::judge`), with each limit that is over;
- p50/p95 of frame, sim, render and GPU time;
- a frame strip: one bar per frame, spikes red, frames over the budget's `frame_ms` amber, the budget line
  across; hover for the frame's time and draws, click to pick it (the worst spike is picked on open);
- the picked frame taken apart: its times, its sim phases, render phases and GPU stages with time in them
  (slowest first), and its counters;
- the spikes with their cause (click one to pick its frame) and the slowest systems in ms per frame.

**Compare with** reads a second capture (default `previous.mcperf`) as "before" and adds the time, counter and
system tables, coloured by the same better/worse rules as `perf_compare`.

`mce_editor -editor.perf_capture <file>` opens a capture at start-up (`EditorShell::open_perf_capture`), focuses
the Profiler and logs `Profiler: opened <file> (N frames, N systems, N spikes)`, or the reason it could not.

**Shared model.** The comparison moved from `tools_perf.cpp` into editor core (`mce/editor/perf_compare.h`:
`compare_captures`, `frame_parts`, `rank_capture_systems`, `system_ms_per_frame`, `perf_phase_name`), so MCP's
`perf_compare` and the panel give the same answer. `perf_compare`'s JSON is unchanged.

**Gates.**
- `test_editor` "perf compare" cases (headless): the 10% / 0.1 ms noise floor, counters on any change, systems
  matched by name and phase and sorted by how far they moved, a different scene flagged, a frame's parts
  in order, and a capture written and read back comparing equal to itself.
- `editor_profiler_capture` (`gpu`): the player records a headless capture; the editor opens it with
  `-editor.perf_capture`, draws the Capture tab for 20 frames and exits 0; a file that is not a capture is
  reported, not fatal.
- `test_mcp_agent`'s `perf_run` / `perf_compare` cases pass unchanged on the shared model.

**Known limits.** The panel picks files by path (the editor has no file dialog for project files outside
`assets/`). The strip draws one bar per frame, so long captures pack bars below a pixel wide.

## Step 5 — the counter gate, the guide chapter, the demo

**Counter gate.** `scripts/ci/perf_gate.py` reads a project's `perf/counters.json` (scenes, run sizes and the
counters each had). It runs MCP `perf_run` on a fresh copy of the project and fails when a held counter (draws,
passes, dispatches, triangles, meshes, sprites, sprite batches) is above its baseline. A counter below its
baseline passes with a note, and `--update` writes the measured counters back. Upload bytes and heap allocations
are printed, not held. Times are never judged. The gate drives the project's own `<game>_mcp`, as `demo_gate.py`
does, so it measures with the game's host and module.

- Baselines: Glowmoth `scenes/level1.mcscene` and `scenes/main.mcscene` (10 draws, 9 sprite batches each);
  Marble Garden `scenes/garden.mcscene` (109 draws, 17 passes, 1 dispatch, 1,230 triangles, 23 meshes).
- ctest: `glowmoth_perf_counters`, `marble_garden_perf_counters` (label `gpu`), so `local_ci.sh`'s gcc-debug stage
  runs them. `glowmoth_perf_counters_seeded` adds three sprites with a texture each to the copy (`--seed`) and
  passes only when the gate fails on draws. It caught 13 draws > 10, 135 sprites > 132 and 12 batches > 9.

**Guide.** Chapter 25, "Performance": captures, budgets per target, `perf_run` / `perf_compare`, the Profiler's
Capture tab, the counter gate, and a table of common fixes.

## Demo

Run in this container under Xvfb with Mesa's software GL, on a copy of Glowmoth, through the stock `mce_mcp
--module libglowmoth.so`. Everything below came from MCP calls; the code change was a file edit and `module_build`.

**Planted.**
- Data: 900 "petal" sprites in `level1`, each with its own 4×4 texture, all in the camera's view.
- Code: a `glowmoth_glow` system that, for every entity, walks every entity to count the ones within 3 m.

**Before** (`perf_run`, target `mobile`, 120 frames after 20 warm-up, 960×540): the verdict fails.

| Over | Measured | Mobile limit |
|---|---|---|
| `sim_ms` p95 | 8.41 | 4 |
| `render_ms` p95 | 14.61 | 8 |
| `draws` max | 910 | 400 |

`top_systems` led with `glowmoth_glow` at 7.55 ms per frame; the next system was `physics_step` at 0.03.
`draws` came with the advice "share textures (one spritesheet) so sprites batch". `project_check` with
`target: mobile` warned that "sprites here use 909 different textures ... the mobile budget allows 16".

**Fixed.**
- Code: `glowmoth_glow` buckets entities into 3 m cells and looks at each sprite's cell and its eight neighbours.
  `module_build` rebuilt and reloaded the module with 0 errors.
- Data: `component_set` pointed the 900 petals' `Sprite.texture` at one texture.

**After** (`perf_run`, same arguments): the verdict passes. `perf_compare`:

| | Before | After | Change |
|---|---|---|---|
| frame p95 | 25.31 ms | 9.95 ms | −60.7% |
| sim p95 | 8.41 ms | 1.95 ms | −76.8% |
| render p95 | 14.61 ms | 5.90 ms | −59.6% |
| draws | 910 | 11 | −899 |
| sprite batches | 909 | 10 | −899 |
| `glowmoth_glow` | 7.55 ms/frame | 1.52 ms/frame | |

`better`: frame_ms, sim_ms, render_ms, draws, sprite_batches, upload_bytes, heap_allocations; `worse`: none;
`pass_before: false`, and the after verdict passes. `project_check` gave no performance finding. The grid still
costs 1.5 ms in this debug build (it sorts 1,000 cells every tick); a release build or a grid kept between ticks
would cut it further.

## Gates (as run)

- A capture never changes the sim hash: `player_perf_capture` (step 1).
- The same run gives the same counters: `test_mcp_agent` runs `perf_run` twice and gets identical
  `counters_max` (step 3). The counter gate holds those counters on every `local_ci.sh` run. Counters come from
  the RHI's own counts, not from timing, so they do not depend on the machine. Only the GL backend is measured
  here (another backend can count passes or batches differently).
- A seeded over-budget scene fails the verdict: `perf_capture` / `judge` tests (step 2), and the demo above.
- The counter stage catches a seeded draw regression: `glowmoth_perf_counters_seeded`.
- `local_ci.sh` green on every step. The `--batch` at milestone end is recorded in the step 5 PR.

**Open.** A capture on a phone and in a browser (the mobile and web budgets are judged here on desktop timings;
the counters carry over). This needs a device.
