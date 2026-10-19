# M-SCENETABS — Several scenes open at once in the editor (milestone record)

**Status:** done 2026-10-19 (steps 1–2). **Owner: the "Geliştirme rutini 3" thread** (a gap taken from the assessments at the coordinator's request: no unowned milestone was left). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/unreal-kiyaslama-2026-10-16.md`, Editor row ("çoklu sahne sekmesi ... kesilmişti"); the M8 editor milestone cut multi-scene tabs (ROADMAP, M8 row). Opening a second scene today asks to discard the first one's changes, so moving between a level and its sub-scenes loses work or forces a save at every switch.

**Goal.** The editor keeps a row of scene tabs. Switching tabs never loses changes: a dirty scene is parked in the autosave file the editor already writes (crash recovery uses the same file) and comes back dirty, as it was left. Nothing changes for MCP or the headless session: they still see one open scene.

**Not in scope:** two scenes edited *at the same time* (one `EditorScene`, one preview world and one undo history stay; undo history is per visit, a switch starts a fresh one), side-by-side viewports, dragging entities between scenes, an untitled dirty scene parked in a tab (it has no file to park beside; the usual "unsaved changes" prompt stays for it).

## Steps
| Step | What | Modules | Model |
|---|---|---|---|
| 1. Tab model | `SceneTabs` in editor core (`mce/editor/scene_tabs.h`): the open files, the active one, park-and-restore through `EditorSession::autosave` / `recover`; headless tests | editor core | Sonnet |
| 2. Scenes panel | the `Scenes` panel (a tab bar with a close button and a dirty mark), Assets and File > Open go through the tabs instead of the discard prompt, quit asks when any tab is dirty; guide 03; the milestone closed | editor layer, docs | Sonnet |

## Gates
- Every step: `local_ci.sh --task` green. Tests pin: a dirty scene survives a switch and comes back dirty with its edits, a clean one is re-read from disk, closing a dirty tab keeps or drops its parked edits as asked, an untitled dirty scene is refused.
- No change to a wide header; `editor_shell.h` (17 TUs) gets one member in step 2.
- The panel is UI like its peers: it compiles and the model is tested; its look needs a display check later.

## Step status
| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | done (this PR) | | `SceneTabs` (`mce/editor/scene_tabs.h`): `sync`, `open`, `activate`, `close`, `dirty`, `any_dirty`. A dirty scene parks in the session's autosave file (`EditorSession::autosave`) and returns through `recover()`; `EditorSession::autosave_path_for(path)` lets a parked tab's autosave be dropped without opening it. 3 tests |
| 2 | done (this PR) | | `Scenes` panel (`scenes_panel.cpp`): a tab bar with a `*` for unsaved changes and a close button that asks first for a dirty tab. `EditorShell::open_scene` goes through `SceneTabs::open` (so Assets, File > Open Scene and the Open Scene list park instead of asking; the discard prompt is kept for an untitled dirty scene), `open_project` clears the tabs, Quit asks while any tab is dirty. `SceneTabs::clear()` added. Build-checked only here (no display) |
