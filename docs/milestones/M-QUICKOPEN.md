# M-QUICKOPEN — One box to open anything (milestone record)

**Status:** step 1 done, step 2 open. **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area; the routine had no Linear work left and the coordinator asked for a new milestone). No Linear issues (Linear is unreachable from this thread and the workspace is at its free issue limit); this record is the tracker.

**Goal.** The editor has about two dozen panels, a project's assets in a tree, and entities in a hierarchy, and every one of them is reached by mouse. Ctrl+P opens a small search box that finds an asset, an entity, a panel or a menu command by typing a few letters, and Enter takes you there. Unreal has the same in Ctrl+P and the content browser search; Unity has Ctrl+K.

**Not in scope:** searching inside asset contents (that is `AssetRefs` and the Inspector filter), recent-files history, user-defined commands.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `QuickOpen` in editor core (`mce/editor/quick_open.h`): items of four kinds (asset, entity, panel, command) built from the session, the scene and a list of names; `search(query, limit)` ranks by fuzzy match (every query letter in order; a prefix, a word start and a short label rank higher; case ignored), with a test (`test_quick_open.cpp`) | done |
| 2 | Shell: Ctrl+P popup with a text field, up/down/Enter/Esc; an asset opens like a double-click in the Assets panel, an entity is selected, a panel is brought to the front, a command runs; user guide row | open |

**What the box searches.** The project's assets by path, the open scene's entities by name, every panel by its title, and the File/Edit/View commands. It reads the same lists the panels read; it adds none of its own.

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked, and `editor_panel_shots` (M-PANELSHOT) draws the shell with the popup closed.
