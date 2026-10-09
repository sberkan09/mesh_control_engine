# M7 — Editor perf dashboards (milestone record)

**Scope:** ROADMAP M7's "editor perf dashboards" line, which is the in-editor profiling dashboard of FR-EDT-140.

**Measured on:** the Linux CI container (4 cores, Mesa llvmpipe, Xvfb), Debug build.

| Item | Where | Evidence |
|---|---|---|
| Per-system wall time | `World::set_profiling` / `system_timings` / `reset_system_timings` (`mce/ecs/world.h`). It is off by default, so the hot path reads no clock. When on, each system's run is timed with the steady clock. The timings are observation only: never hashed and never read by the sim. | `test_ecs` "system profiling…": the same run hashes the same with profiling on and off; it records per-system runs, last and total time in execution order, and reset and off both behave. |
| Frame stages per editor frame | `EditorShell` times its `build_ui` (the play-in-editor ticks separately from the panels) and `record`. The host times `RenderDevice::submit` and hands it over with `end_frame_perf`. When the device wrote GPU timestamps that frame, the span between them is the GPU stage. | `mce_editor -project … -editor.play 1 -editor.panel Profiler` (screenshot below) |
| History + summaries | `PerfMonitor` (`mce/editor/perf_monitor.h`, headless): a 300-frame ring (5 s at 60 fps) with mean and nearest-rank p50 / p95 / p99 / max per stage. Unmeasured stages are left out. It can be frozen. | `test_editor` "perf monitor:" cases cover rolling and freeze, the percentiles on 1..100 ms, and slow-frame counting. |
| The NFR-F-01/F-02 budget table | `PerfMonitor::budget` sets each stage's p95 against its allocation: frame 16.6 (NFR-B-06's 60 fps), simulation 3.0, cull + sort + record 1.5, RHI translate + submit 3.5, GPU 6.0, and the editor's own UI against the 2.6 ms headroom. A stage over its allocation shows red. | "the NFR-F-02 budget table flags the stages over their allocation" |
| Systems ranked by cost while playing | The play world profiles its systems (`PlaySession` turns profiling on). The panel ranks them by mean (`PerfMonitor::rank_systems`, top 24). `PlaySession::last_advance_ms` is the simulation stage. | "systems ranked by mean cost…"; "play-in-editor profiles the play world without changing its run". The play-session parity tests against `mce_player` still pass with profiling on. |
| The dashboard | The **Profiler** panel (docked beside Stats). It shows a frame-time graph with the 16.6 ms line, a choice of any stage to graph, the budget table and the systems table. **Pause** freezes it on a spike. **Copy as Markdown** puts the report on the clipboard in the milestone budget-table shape. `-editor.panel <name>` brings a panel's tab to the front at startup. | screenshot below |

On llvmpipe the editor's own frame is slow: about 32 ms mean in the screenshot, dominated by the GL translate and submit of the software rasterizer. The panel shows this directly as red p95 cells. NFR-B-06's 60 fps is a hardware measurement.

## Screenshot

`/mnt/project-files/m7-3d/editor-profiler.png`: `scene_demo` playing, with the Profiler tab in front.
