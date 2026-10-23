# M-SELSETS — Selection sets (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area; the routine had no Linear work left and the coordinator asked for new milestones). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Placing a level means working on the same group again and again: the four torches of a room, the guards of a camp. Re-selecting them one by one with Ctrl+click is slow. Ctrl+Shift+1..9 stores the current selection in a numbered slot and Alt+1..9 selects that group again, like Maya's quick selection sets.

**Not in scope:** named sets, a sets panel, saving sets to disk (entity ids belong to one open scene), adding to a set.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `SelectionSets` in editor core (`mce/editor/selection_sets.h`): nine slots of entity ids (null ids and repeats dropped, order kept), `save` / `recall` (skips entities the caller says are gone) / `clear` / `clear_all` / `count`, with a test (`test_selection_sets.cpp`) | done |
| 2 | Shell: Ctrl+Shift+1..9 stores and Alt+1..9 recalls while the Viewport or Hierarchy has the keyboard (no text field owns it); the sets clear when another scene becomes the open one; Edit > Selection sets lists the set slots; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
