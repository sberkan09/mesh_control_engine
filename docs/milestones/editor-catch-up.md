# Editor catch-up: the M2/M3/M4 editor items (milestone record)

**Why:** the M4 record found that the editor items of M2 and M3 never landed, which also blocked the editor items
of M4 (gizmo polish, autosave, the UC-C loop rehearsal). This record covers the catch-up work done before M5a.
It is updated with every PR in the series.

**Design source:** `docs/architecture/editor-tools.md`: the canonical `SceneDocument`, the `EditorScene` mutation
funnel with a preview World that runs no systems, the `DrawCtx` widget seam, CPU picking, and run-in-player as a
child process.

## Runtime path (PR #27, ADR-0024)

The editor's Save → Cook → Play loop needs a runtime that can load an authored scene:

- `mce_scene` instantiates a cooked scene through reflection, with all-or-nothing validation and EntityRef fixups.
- `mce_gameplay_kit` provides `Sprite`, `TextLabel` and `Camera2DComponent`, plus their extract.
- `mce_player` runs a cooked pack, windowed or headless, with a pause overlay.
- `samples/scene_demo` is an editor project with no code.

## M2 editor items

| ROADMAP M2 item | Where | Evidence |
|---|---|---|
| Scene save/load in the editor via `SceneDocument` + reflection serialization | `editor_core`: `SceneDocument`, `EditorScene::open/save` (atomic temp + rename); cook-side `scene_text.h` reader/writer shared with the `.mcscene` importer | `test_editor`: parse → serialize → parse is byte-stable; the demo scene is stored in the editor's canonical form; newer versions and unknown components are `file:line` errors |
| Inspector round-trip with zero per-type code (FR-EDT-020, LRN-4 part 1) | `draw_inspector` over `DrawCtx`; `ImGuiDrawCtx` is the only ImGui adapter | `test_editor`: a scripted `DrawCtx` edits vec3/quat (Euler)/bool/int/float/colour/string/enum/AssetRef/EntityRef fields; the edits land in the document *and* the preview world, then survive serialization |
| Minimal entity spawn/select/delete (finding F54) | Hierarchy panel (create, rename, delete, drag to reorder, Ctrl multi-select), Viewport click-pick, Del | `test_editor`: 400 random commands never let the preview world drift (`verify_world`); deleting an entity nulls references to it |
| A broken asset gives a `file:line` cook error in an editor panel while the old asset keeps rendering | Cook panel (catalog, scene-open and cook diagnostics; failed/blocked assets) | `test_editor` session case: a broken scene reports `copy.mcscene:2`, the previous cooked scene still loads |
| Author a scene in the editor → Save → Cook → load in the player | File menu (New/Open/Save/Save As/Cook/Cook + Pack), `EditorSession` | `test_editor` session case: open the demo project, add a sprite entity, save, cook, load the cooked scene; `editor_scene_smoke` (GPU) opens the cooked copy, draws its sprites, and picks |

## M3 editor items

| ROADMAP M3 item | Where | Evidence |
|---|---|---|
| Entity CRUD + copy/paste | Hierarchy (create/rename/delete/reorder), Edit menu + Ctrl+C/X/V/D; `EditorScene::copy_entities/paste_entities/duplicate_entities` (ADR-0025 §5) | `test_editor` clipboard case: internal references remapped, external ones cleared, duplicate lands after its source, garbage pastes change nothing |
| Translate gizmo | `TranslateGizmo` (hand-rolled, ADR-0025 §1): X/Y arrows + XY square, screen-constant; dragging a selected entity body moves it too | `test_editor` gizmo case: pixel hit areas at two zooms, axis lock, multi-select, cancel restores |
| Grid/snap placement aids (finding F34) | Grid overlay (View menu), snap toggle + step (Edit menu), Ctrl inverts during a drag | `test_editor` gizmo case: anchor snaps, the rest keep their offsets |
| Run-in-player button (A4) | Play/Stop in the menu bar (F5): save → cook `--pack` → `mce_player -scene <guid>` as a child process (`platform::Process`) | UC-C rehearsal case below drives the same path headless |
| Asset/entity pickers (finding F6 MVP) | Inspector `AssetRef` combo (catalog filtered by asset type, text filter) and `EntityRef` combo | exercised by `editor_scene_smoke` (Ball selected, inspector drawn every frame under the GL debug layer) |
| Minimal scene-state menu + pause overlay (A9) | `mce_player` Esc overlay (PR #27) | `scene_demo_player_gpu` |

## M4 editor items

| ROADMAP M4 item | Where | Evidence |
|---|---|---|
| Editor gizmo polish | hover highlight, active-handle highlight, live Δ readout with a snap marker, Esc/right-click cancel, Ctrl snap inversion, frame selection (F) | `test_editor` gizmo case (cancel, snap) |
| Autosave | `EditorSession::autosave/tick_autosave/has_recovery/recover` (ADR-0025 §4): 5-minute dirty timer + focus loss, atomic, outside `assets/`; recovery prompt on open | `test_editor` autosave case: timer, crash → recover → save clears it, untitled + discard |
| UC-C loop rehearsal | see below | `test_editor` "UC-C rehearsal" case |

### UC-C loop rehearsal (re-walked after the catch-up)

| Step | Now |
|---|---|
| launch editor | `mce_editor -project <dir>`, or open a project folder from the Hierarchy panel |
| open scene | File → Open Scene (Ctrl+O): the project's `.mcscene` files from the catalog |
| drag a prefab into the viewport | prefabs are v0.5 (M6); the M3 equivalent is **+ Entity** at the camera centre, or duplicate (Ctrl+D) |
| gizmo-nudge it | translate gizmo or body drag, with grid snap |
| Play / Stop (scene reverts) | Play (F5) runs the cooked scene in `mce_player`; the editor's scene is never touched |
| tweak a component in the inspector | reflection inspector, every field kind, asset/entity pickers |
| Save | Ctrl+S (atomic); autosave covers crashes |
| Cook | F7, errors with `file:line` in the Cook panel |
| launch the game | Play, or `mce_player -pack <dir>/assets.mce` |

The rehearsal test drives the same editor-core calls the panels make, with no window. It creates a new scene,
adds a camera, a floor and a crate, duplicates and gizmo-drags the crate three times with snap, adds a label, saves
the scene as `scenes/level1.mcscene`, and cooks with `--pack`. It then runs `mce_player` headless on that scene as
a child process and checks for 7 entities and a state hash. The timed version, "under 30 minutes of editor-only
actions" (REQUIREMENTS UC-C, the v1.0 gate), needs a person at the keyboard and stays a v1.0 measurement.

## Open

- Undo/redo, multi-select editing in the inspector, rotate/scale gizmos, the asset browser and prefabs are M6
  (v0.5) by the ROADMAP.
- Play-in-editor (embedded host, pause/step) is M5b: shipped (ADR-0029, `docs/milestones/M5b.md`).
- Prefab instances and `ChildOf` round-trip through the editor (ADR-0025 §7). Expanding prefabs in the
  viewport, a tree Hierarchy, and parent-relative placement are M6 editor work.
- Windows: `platform::Process` uses SDL3's process API, which is portable but has only run on Linux here.

Run the editor on the sample with:

    mce_editor -project samples/scene_demo

The Viewport controls are: mouse wheel to zoom at the cursor, right or middle drag to pan, left click to select (Ctrl+click toggles), F to frame, and Del to delete.
