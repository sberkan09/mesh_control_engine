# M-SELRELATED — Select parent, children, siblings (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area; the routine had no Linear work left and the coordinator asked for new milestones). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Moving a whole room means selecting the room and everything under it; fixing one door means jumping from a child up to its parent and back. Today that is Ctrl+click on each row. Alt+Up selects the parents of the selection, Alt+Down the children, Alt+Shift+Down everything below, and Edit > Select adds the siblings. Blender has Select Parent / Child / Grouped > Siblings, Unreal has Select All Children and Select Parent.

**Not in scope:** selecting by component or tag, extending (adding to) the selection instead of replacing it, selecting across scenes.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `select_related` in editor core (`mce/editor/select_related.h`): parents, children, descendants (cycle-safe) and siblings of a set of entities, each once, with a test (`test_select_related.cpp`) | done |
| 2 | Shell: Alt+Up / Alt+Down / Alt+Shift+Down and Edit > Select (Parent, Children, All descendants, Siblings) replace the selection with the result; an empty result leaves it alone; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
