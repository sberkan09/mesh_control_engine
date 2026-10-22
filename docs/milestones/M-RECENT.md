# M-RECENT — Open Recent (milestone record)

**Status:** step 1 and 2 done (the milestone is complete). **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area; the routine had no Linear work left and the coordinator asked for a new milestone). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Opening a scene means File > Open Scene, a modal list of every scene in the project. The scene you worked on an hour ago is somewhere in that list. File > Open Recent lists the last few scenes you opened or saved, newest first, and one click opens one. Unreal has File > Recent Files; Unity and Godot keep the same list.

**Not in scope:** recent projects (the editor takes its project on the command line), pinning, recent assets other than scenes.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `RecentList` in editor core (`mce/editor/recent_list.h`): newest first, no duplicates, capped (10), `touch` / `remove`, text `serialize` / `parse`, with a test (`test_recent_list.cpp`) | done |
| 2 | Shell: the list is read from and written to a per-project file (`<root>/.mce/recent_scenes.txt`) when a scene is opened or saved; File > Open Recent submenu (a missing file is dropped from the list); user guide row | done |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
