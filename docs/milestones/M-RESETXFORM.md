# M-RESETXFORM — Reset transform (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Undoing a stray move, spin or stretch meant retyping nine numbers in the Inspector. Edit > Reset transform puts the local transform of each selected entity back to position 0, no rotation, scale 1, as one undo step. Unity and Unreal have the same command.

**Not in scope:** resetting only position, rotation or scale; resetting the world transform.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `reset_transforms` in editor core (`mce/editor/reset_transform.h`) with a test (`test_reset_transform.cpp`) | done |
| 2 | Shell: Edit > Reset transform; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
