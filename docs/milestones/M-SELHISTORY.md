# M-SELHISTORY — Selection history, Back / Forward (milestone record)

**Status:** step 1 done (#725), step 2 done. **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area; the routine had no Linear work left and the coordinator asked for new milestones). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Clicking from the player to a door to a lever and back loses the player: finding it again means scrolling the Hierarchy or hunting in the viewport. Alt+Left steps back to the entity you had selected before and Alt+Right forward again, like a browser's history. Unreal and Blender have no such list; Rider, Visual Studio and every browser do, and the Hierarchy is the one place in the editor where you jump around like that.

**Not in scope:** a history panel, history across scenes or sessions, remembering multi-selections (the anchor entity is what counts), the Assets panel's selection.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `SelectionHistory` in editor core (`mce/editor/selection_history.h`): the entities selected, `visit` (same twice is one entry; selecting while stepped back drops the entries ahead; 64 entries), `back` / `forward` that skip entities for which the caller's predicate says they are gone, with a test (`test_selection_history.cpp`) | done (#725) |
| 2 | Shell: the shell visits the selection's anchor whenever it changes; Alt+Left / Alt+Right step (the viewport and Hierarchy not owned by a text field); Edit > Previous selection / Next selection; the history clears when a project or scene opens; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
