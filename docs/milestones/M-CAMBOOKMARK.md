# M-CAMBOOKMARK — Camera bookmarks (milestone record)

**Status:** step 1 open. **Owner: the "Geliştirme rutini 3" thread** (the editor and asset tools are its area; the routine had no Linear work left and the coordinator asked for new milestones). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Working on a big level means flying between the same few places: the spawn, the boss room, the far edge. Panning and zooming back each time is slow. Ctrl+1..9 stores the viewport's camera in a numbered slot and 1..9 flies back to it. Unreal calls these Camera Bookmarks (Ctrl+1..9 / 1..9). A slot holds the 2D camera and the 3D orbit camera together, so the same key works in either view mode.

**Not in scope:** named bookmarks, a bookmarks list panel, bookmarking the selection, animated fly-to.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `CameraBookmarks` in editor core (`mce/editor/camera_bookmarks.h`): nine slots, each with the 2D camera (centre, zoom) and the orbit camera (target, yaw, pitch, distance); `save` / `recall` / `clear`, text `serialize` / `parse` (one line per set slot), with a test (`test_camera_bookmarks.cpp`) | open |
| 2 | Shell: Ctrl+1..9 saves and 1..9 recalls while the Viewport has the keyboard (no text field owns it); the slots are kept per project in `<root>/.mce/camera_bookmarks.txt`; a View menu entry lists the set slots; user guide row | open |

**Gates:** the step 1 test (`test_editor`); step 2 is build-checked and `editor_panel_shots` draws the shell.
