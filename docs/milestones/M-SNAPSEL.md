# M-SNAPSEL — Snap the selection to the grid (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** The gizmo snaps while dragging (View > Snap step), but entities placed earlier, pasted or imported stay off the grid. Edit > Snap to grid rounds the world X and Y of each selected entity to a multiple of the snap step (Z and the rest stay), as one undo step. Unity and Unreal have the same command.

**Not in scope:** snapping rotation or scale, a separate step for the command.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `snap_entities` in editor core (`mce/editor/snap_entities.h`) with a test (`test_snap_entities.cpp`) | done |
| 2 | Shell: Edit > Snap to grid (uses the View snap step); user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
