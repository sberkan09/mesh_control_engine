# M-UNIQUENAMES — Make entity names unique (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Duplicating (Ctrl+D), pasting and prefab drops leave many entities called "Wall", which makes the Hierarchy filter, MCP clients and scripts that look entities up by name ambiguous. Edit > Make names unique renames the later holders of a name to "Wall (2)", "Wall (3)" among the selection (or the whole scene when nothing is selected). The first holder keeps its name, a name used by an entity outside the selection counts as taken, an existing " (n)" suffix is replaced instead of stacked, and the whole pass is one undo entry.

**Not in scope:** doing it automatically on duplicate or paste, a naming style setting.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `make_names_unique` in editor core (`mce/editor/unique_names.h`), with a test (`test_unique_names.cpp`) | done |
| 2 | Shell: Edit > Make names unique; user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
