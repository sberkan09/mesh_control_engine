# M-LINTFIX — Scene lint findings repair themselves (milestone record)

**Status:** in progress (steps 1-2 done, step 3 open (PR)). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-27 on the coordinator's suggestion (more editor and agent tooling; device-free, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** `scene_lint` (M-SCENELINT, M-SCENEREFS) names six kinds of mistake, and the only cure today is editing the scene by hand or through many single-entity tool calls. Every kind has an unambiguous safe repair, so a tool can do it in one step: an agent finds the findings, fixes them, saves, and lints again.

**Design.**
- `fix_scene_lint(SceneDocument&)` in `mce_editor_core` repairs each finding of `lint_scene` in place and returns how many it fixed. After it, `lint_scene` is clean, and a second call changes nothing.
- Repairs keep what a scene could still mean: an empty name becomes `Entity <id>`; a later sibling with a taken name gets ` (2)`, ` (3)`, ...; a missing parent is dropped (the entity becomes a root); a reference to a missing entity becomes null; a position, rotation or scale that is not a number returns to its default; a zero scale axis becomes 1. A finite axis is never touched.
- It edits a loaded document directly, for tools that load a file, fix and save it. An open `EditorScene` keeps its undoable edits.
- No change to the file format, the cook or the runtime.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `fix_scene_lint` and its `test_editor` cases (every kind, idempotent, a tidy scene unchanged) | done (#810) |
| 2 | `mce_scene_lint --fix` rewrites the file in the canonical form and prints what it fixed; a `ctest` over a copy of `lint_bad.mcscene` | done (#812) |
| 3 | MCP `scene_lint_fix` (`path` of a saved scene; writes the file and reports the findings it fixed), capabilities and user-guide rows | added 2026-10-27, open (PR) |

**Gates:** `test_editor`, `scene_lint_cli_*` ctests; step 3 also `mcp_transcript`.
