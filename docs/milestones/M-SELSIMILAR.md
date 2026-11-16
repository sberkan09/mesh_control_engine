# M-SELSIMILAR — Select similar entities (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** After clicking one coin, "all the other coins" meant clicking each. Edit > Select > Similar selects every entity that has exactly the same component types as the primary selection (the anchor), so they can be moved, aligned, grouped or deleted together. Blender has Select Similar, Unity Select All of Same Type.

**Not in scope:** comparing component values, partial matches (has at least these components), a shortcut.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `select_similar` in editor core (`mce/editor/select_all.h`): the entities with the anchor's component types, document order, with a test (`test_select_all.cpp`) | done |
| 2 | Shell: Edit > Select > Similar; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
