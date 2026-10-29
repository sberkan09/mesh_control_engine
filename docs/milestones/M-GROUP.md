# M-GROUP — Group and ungroup entities (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Putting several entities under one parent meant creating an empty entity and dragging each onto it. Ctrl+G makes a "Group" entity under the first selected entity's parent and moves the selection into it; Ctrl+Shift+G dissolves the selected parents, handing their children to the grandparent. Each is one undo entry and nothing moves in the scene (re-parenting keeps the world transform).

**Not in scope:** placing the group at the selection's centre (it starts at the origin), naming the group on creation (use F2 or Rename).

## Steps

| # | Step | State |
|---|---|---|
| 1 | `group_entities` and `ungroup_entities` in editor core (`mce/editor/group_entities.h`): top-level entities only, one undo entry each, with a test (`test_group_entities.cpp`) | done |
| 2 | Shell: Ctrl+G and Ctrl+Shift+G (no text field owns the keyboard), Edit > Group / Ungroup; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
