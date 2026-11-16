# M-SCENESTATS — Scene statistics (milestone record)

**Status:** done (all three steps). **Owner: the "Geliştirme rutini 3" thread** (editor and asset tools). Defined 2026-10-25 on the coordinator's standing request to keep going when the Backlog is empty, in an area that needs no real account or device (editor, scenes and serialization, tools; outside gameplay and the traffic work). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** A reviewer or an agent opening a scene wants the size of it at a glance: how many entities, how deep the hierarchy goes, how many are prefab instances, which components the scene uses and how often, and the box its entities span. `scene_stats` computes this from a scene document without an ECS, so the editor, the command line and agents read the same numbers.

**Not in scope:** world-space bounds (needs the transform chain and mesh sizes), per-frame cost estimates (`perf_run` owns those), fixing anything (`scene_lint` finds mistakes).

## Steps

| # | Step | State |
|---|---|---|
| 1 | `scene_stats` and `format_stats` in editor core (`mce/editor/scene_stats.h`) with a test (`test_scene_stats.cpp`) | done |
| 2 | `mce scene stats <scene>...` (the `mce_scene_stats` tool under `mce_cli`), user guide row, two ctest cases over the fixture scenes. The engine's own components only | done |
| 3 | MCP `scene_stats` tool (the open scene or a path) with a `test_mcp_team` case | done |

**Gates:** step 1 `test_editor`.
