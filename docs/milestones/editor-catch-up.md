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

Run the editor on the sample with:

    mce_editor -project samples/scene_demo

The Viewport controls are: mouse wheel to zoom at the cursor, right or middle drag to pan, left click to select (Ctrl+click toggles), F to frame, and Del to delete.
