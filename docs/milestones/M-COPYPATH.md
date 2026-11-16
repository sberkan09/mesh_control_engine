# M-COPYPATH — Copy an entity's hierarchy path (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Naming an entity in a bug report, a script or an MCP prompt meant typing its place in the tree by hand. Edit > Copy path puts the path of each selected entity (`Room/Wall/Torch`, names from the root down, one path per line) on the system clipboard. Unity and Unreal have the same command.

**Not in scope:** pasting a path back to select the entity, ids in the path.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `entity_path` and `entity_paths` in editor core (`mce/editor/entity_path.h`), cycle-safe, with a test (`test_entity_path.cpp`) | done |
| 2 | Shell: Edit > Copy path; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
