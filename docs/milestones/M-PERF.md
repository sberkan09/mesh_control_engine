# M-PERF — Performance tooling for games and agents (milestone record)

**Status:** active (added 2026-10-15). Plan: ROADMAP §M-PERF; Linear SBA-113 … SBA-117.
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
| 1 | `.mcperf` captures from the stock player: CPU stages, sim phases and systems, GPU passes, render counters, memory | SBA-113 | in progress |
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
