# M-SELMATCH — Select the Hierarchy filter's matches (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** The Hierarchy "Filter by name" box (M-HIERFILTER) only shows the hits. With the box active, a **Select N matches** button under it makes every entity whose name matches the selection, so "all Wall pieces" can be moved, renamed (M-BATCHRENAME) or deleted at once. Unreal's Outliner and Blender's Select Pattern do the same.

**Not in scope:** matching by type, tag or component; regular expressions.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `select_matching(scene, query)` in editor core (`mce/editor/select_all.h`): the entities whose name matches, in document order; a blank query matches nothing; with a test (`test_select_all.cpp`) | done |
| 2 | Hierarchy panel: the **Select N matches** button while the filter has hits; user guide text | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
