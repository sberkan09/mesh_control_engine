# M-MATCHXFORM — Match the anchor's transform (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Making ten crates the same size, or putting a lamp where another one stands, meant copying numbers through the Inspector one entity at a time. Edit > Match to anchor > Position / Rotation / Scale copies that part of the first selected entity (the anchor) onto the other selected entities, as one undo step. Position is matched in world space; rotation and scale are the local values.

**Not in scope:** matching a single axis, matching other components.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `match_transforms` in editor core (`mce/editor/match_transform.h`) with a test (`test_match_transform.cpp`) | done |
| 2 | Shell: Edit > Match to anchor submenu; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
