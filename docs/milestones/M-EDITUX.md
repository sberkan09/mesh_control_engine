# M-EDITUX — Editor authoring that must survive undo (milestone record)

**Status:** in progress, steps 1 to 3 done 2026-11-11. **Owner: the "Geliştirme rutini 3" thread.** Added 2026-11-11 on the coordinator's instruction to move from hardening to the editor side. No Linear issues (the Backlog holds only the device-bound SBA-112 and SBA-58); this record is the tracker.

**Why.** The editor is the tool people spend their time in, and a lost edit is the worst bug it can have. `test_undo.cpp` already fuzzes entity create, delete, rename, move, component add/remove/set and duplicate. The commands an open-world scene is built from (parenting, copy and paste, deleting a parent with children) were outside that fuzz.

**Scope.** `tests/editor` and `editor_core` only; test first, a fix only where a gate fails. Stays out of the modules the other routines own (render, rhi, physics3d, openworld, kit, GTA gameplay, weather).

## Steps

| # | Step | State |
|---|---|---|
| 1 | Undo fuzz over `set_parent`, `copy_entities` + `paste_entities`, `destroy_entity` and `create_entity` on a tree (30 seeds, 120 commands each): undo all is byte-identical to the start, every intermediate state matches, redo all reaches the edited scene, `verify_world` holds after every command | done 2026-11-11: passes, no defect found |
| 2 | Undo fuzz over prefab-instance commands (`create_prefab_instance`, a Sprite edit that becomes an override, `revert_override`, delete, duplicate) on the two-instance scene (20 seeds, 80 commands each, `test_prefab_overrides.cpp`): same checks as step 1 | done 2026-11-11: passes, no defect found (`apply_override` writes the prefab asset, outside the document's undo, so it is not in the fuzz) |
| 3 | Inspector sweep (`test_inspector_sweep.cpp`): every component the Add menu offers is added, every field is nudged through a scripted `DrawCtx`; the result parses and re-serializes byte-stable, undoes to the byte, redoes, and `verify_world` holds | done 2026-11-11: found one defect, the menu offered `WorldTransform` (derived from the hierarchy each tick, so the preview world drifted from the document); the menu no longer offers it |
