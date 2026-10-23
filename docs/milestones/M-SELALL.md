# M-SELALL — Select all, invert selection (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area; the routine had no Linear work left and the coordinator asked for new milestones). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** The Hierarchy could select one entity, a range or a group, but not "everything" or "everything except these". Ctrl+A selects every entity of the scene and Ctrl+I the ones that are not selected now (select the few odd ones, invert, delete the rest). Blender has A and Ctrl+I, Unreal Ctrl+A and Select > Invert Selection.

**Not in scope:** select none (clicking empty space already does it), selecting by type or tag.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `select_all` and `invert_selection` in editor core (`mce/editor/select_all.h`): the entities in document order, with a test (`test_select_all.cpp`) | done |
| 2 | Shell: Ctrl+A and Ctrl+I (no text field owns the keyboard), Edit > Select > All / Invert; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
