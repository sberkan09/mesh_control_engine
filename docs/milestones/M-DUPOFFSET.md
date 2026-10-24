# M-DUPOFFSET — Duplicate and step (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** A row of pillars or a column of tiles is "duplicate, then move the copy one step", repeated. Edit > Duplicate right / Duplicate up duplicates the selection (with subtrees) and moves the copies by one snap step (View > Snap step) in world space, as one undo step. The copies become the selection, so pressing the command again continues the row. Unity and Unreal have a step-and-repeat.

**Not in scope:** a count or a free offset (a dialog), rotating copies around a pivot.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `duplicate_offset` in editor core (`mce/editor/duplicate_offset.h`) with a test (`test_duplicate_offset.cpp`) | done |
| 2 | Shell: Edit > Duplicate right / Duplicate up (use the View snap step); user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
