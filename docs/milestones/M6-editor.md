# M6 — Editor v0.5 (milestone record)

**Scope:** the editor half of ROADMAP M6 (the ECS, subsystem, 2D-extras and content halves are tracked separately).
Design decisions: ADR-0030. **Measured on:** the Linux CI container (4 cores), gcc 13, `scripts/ci/local_ci.sh`.
This record is updated with every PR in the series.

| ROADMAP M6 item | Where | Evidence |
|---|---|---|
| Undo/redo, covering MCP edits (FR-EDT-040) | `EditorScene` snapshot history (`begin_edit`/`end_edit`, `seal_undo`, `undo`/`redo`); Edit menu + Ctrl+Z / Ctrl+Y; `mce_mcp` wraps each tool call in an edit group and adds `scene_undo` / `scene_redo`; a scene re-read from disk is one "Reload from disk" entry (ADR-0030 §1–§3) | `test_editor` "undo fuzz": 40 seeds × 200 random commands (create, delete, rename, reorder, components, references, duplicate, grouped multi-step edits), undo all → byte-identical document, redo all → the edited one, `verify_world` throughout; drag coalescing, edit groups, bounded history, reload-from-disk cases; `test_mcp` undo/redo over MCP; FR-EDT-160 transcript test green |
| Rotate/scale gizmos + multi-select (FR-EDT-030, FR-EDT-050) | `TransformGizmo` move/rotate/scale modes (W/E/R, Edit menu); viewport Ctrl/Shift click and marquee; hierarchy Shift range select (`EditorScene::select_range`) (ADR-0030 §4) | `test_editor` "gizmo:" cases: ring hit area, the group turns about the anchor, 15° snap, cancel restores; axis and uniform scale with offsets scaled, 0.1 snap, no flip through the pivot; "selection:" range select in document order and the marquee; the whole drag is one undo step |

## Open

| Item | State |
|---|---|
| Tree hierarchy (`ChildOf`) | next PR in the series |
| Prefab content in the viewport | next PR in the series |
| Asset browser with drag-drop | next PR in the series |
| Tilemap panel (rectangle fill + re-bake loop, FR-EDT-120 / FR-R2D-115) | next PR in the series |
