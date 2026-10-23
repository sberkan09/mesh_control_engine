# M-ALIGN — Align and distribute entities (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Lining entities up meant typing the same number into each Inspector. Edit > Align moves the selected entities to the lowest, middle or highest world position on x or y, and Distribute spaces three or more evenly between the two outermost. Positions are world positions, so a parent's transform is respected; only the chosen axis changes; each command is one undo entry.

**Not in scope:** aligning by sprite or collider bounds (positions only), a toolbar, the z axis.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `align_entities` and `distribute_entities` in editor core (`mce/editor/align_entities.h`), with a test (`test_align_entities.cpp`) | done |
| 2 | Shell: Edit > Align submenu (needs two selected entities, distribute needs three); user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
