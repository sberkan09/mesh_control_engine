# M-FLIP — Flip the selection (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Building the mirror half of a room meant moving each piece by hand and flipping its scale in the Inspector. Edit > Flip horizontally / vertically mirrors the selected entities across the middle of the selection (positions in world space, so a parent's transform counts) and negates their local scale on that axis, as one undo step. A single entity flips in place. Unity and Unreal have a mirror tool.

**Not in scope:** mirroring rotations of rotated entities beyond the sign of the scale, flipping across a chosen line.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `flip_entities` in editor core (`mce/editor/flip_entities.h`) with a test (`test_flip_entities.cpp`) | done |
| 2 | Shell: Edit > Flip horizontally / vertically; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
