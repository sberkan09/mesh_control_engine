# M-HIERFILTER — Hierarchy filter (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area; the routine had no Linear work left and the coordinator asked for new milestones). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** A level with hundreds of entities cannot be navigated by scrolling the Hierarchy. A "Filter by name" box above the tree shows only the entities whose name contains every word typed, with the parents that lead to them, like the Outliner search in Unreal and Blender. The Assets panel and the Inspector already have such a box; the Hierarchy was the gap.

**Not in scope:** filtering by component or tag, regular expressions, a saved filter, filtering the Viewport.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `HierarchyFilter` in editor core (`mce/editor/hierarchy_filter.h`): `name_matches` (space-separated words, all must appear, case-insensitive) and `rebuild` (the matches plus their ancestors, cycle-safe), with a test (`test_hierarchy_filter.cpp`) | done |
| 2 | Hierarchy panel: the box above the tree, rebuilt each frame; non-matching rows hide, the tree opens to the hits, hits are drawn in colour; user guide | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the panel.
