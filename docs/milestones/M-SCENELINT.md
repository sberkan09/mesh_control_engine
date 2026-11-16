# M-SCENELINT — Scene lint (milestone record)

**Status:** steps 1, 2 and 3 done (milestone closed). **Owner: the "Geliştirme rutini 3" thread** (editor and asset tools). Defined 2026-10-25 on the coordinator's request to keep going when the Backlog is empty, in an area that needs no real account or device (editor, scenes and serialization, tools; outside gameplay and the traffic work). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Some scene mistakes load without an error and misbehave later: an entity with no name, two siblings with one name (so a path like `Room/Wall` names both), a scale of zero on an axis, a position that is not a number, a parent that does not exist. `lint_scene` finds them in a scene document without an ECS, so the editor, the command line and agents can ask the same question the same way.

**Not in scope:** fixing the findings automatically, project-wide asset checks (`project_check` owns those), gameplay rules.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `lint_scene` and `format_lint` in editor core (`mce/editor/scene_lint.h`) with a test (`test_scene_lint.cpp`) | done |
| 2 | `mce scene lint <scene>...` (the `mce_scene_lint` tool under `mce_cli`; exit 1 on findings), user guide row, three ctest cases over fixture scenes. The engine's own components only | done |
| 3 | MCP `scene_lint` tool (the open scene or a path) with a `test_mcp_team` case | done |

**Gates:** step 1 `test_editor`.
