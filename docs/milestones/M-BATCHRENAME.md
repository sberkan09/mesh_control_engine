# M-BATCHRENAME — Rename selection (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area; the routine had no Linear work left and the coordinator asked for new milestones). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** A room with twelve torches means twelve F2 renames. Edit > Rename selection... names every selected entity from a pattern in one undo step: `Torch_##` gives Torch_01, Torch_02 and so on in scene order, `*` keeps the current name. Unreal has Batch Rename for actors and Blender has Batch Rename (Ctrl+F2); the Hierarchy had only single rename.

**Not in scope:** find and replace inside names, case conversion, renaming assets (the Assets panel renames one file at a time).

## Steps

| # | Step | State |
|---|---|---|
| 1 | `batch_rename_expand` in editor core (`mce/editor/batch_rename.h`): `#` runs are the zero-padded number (width is a minimum), `*` is the old name, the result is cut to the 63 characters an entity name holds, with a test (`test_batch_rename.cpp`) | done |
| 2 | Shell: Edit > Rename selection... opens a popup with the pattern box, the first number and a preview of the first four results; Rename applies it to the selection in document order as one undo step; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
