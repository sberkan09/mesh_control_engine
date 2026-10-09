# M8 — Editor maturity (milestone record)

**Scope:** ROADMAP M8, "editor maturity: prefab override UI, asset thumbnails, timeline/curve editor (FR-ENT-110), multi-scene tabs". The binding cut order applies: the timeline/curve editor goes first, then asset thumbnails.
**Measured on:** the Linux CI container (headless editor tests; Xvfb + llvmpipe for the screenshot).

## Prefab overrides (FR-EDT-080)

| Item | Where | Evidence |
|---|---|---|
| Field-level overrides | `DocComponent::overrides` is a mask of top-level fields. On a prefab instance, `EditorScene::set_component` marks the fields an edit changes (inspector, gizmo, MCP alike). A file's instance component names exactly its overridden fields, and parse reads the mask back from the keys present. | `test_editor` "prefab overrides: field by field; an override back to the default survives a save" |
| Save writes overrides only | `cook::write_scene_component_fields` writes the masked fields, **including fields that hold the C++ default**. Before, an instance that overrode a field back to its default (e.g. `order: 0` over a prefab's 3) lost it on save, and the prefab's value came back after the cook. | same test: B's line stays `"Sprite": { "order": 0 }` across edit, undo, save and reopen |
| Effective values in the document | Each ghost sync copies the prefab root's values into an instance's non-overridden fields. The inspector therefore shows and edits what the cook will produce, even after the prefab changed on disk. This normalization is not an edit: only the overrides are saved. | B's `size` reads 2×2 (the prefab's) in the document; the ghost root shows `order 0` + `size 2` |
| Ghosts follow overrides, not whole structs | The ghost root takes the prefab root's values with the overridden fields on top, which is what the cook's flattener does. | ghost probes in the tests |
| Override display | The inspector calls `DrawCtx::override_mark` before each field of an instance's component. The ImGui adapter draws a blue bar in the margin. The prefab root's components the instance does not carry are listed as "Inherited". | `MarkCtx` asserts `Sprite.order` overridden and `Sprite.size` inherited; screenshot `m8-editor/prefab-overrides.png` |
| Revert | **Revert** on a component (`EditorScene::revert_override`) drops its overrides: the fields take the prefab's values again. A component left with nothing of its own, or one the prefab root lacks, leaves the instance. One undo step. The placement (`LocalTransform`) is always the instance's own. | "revert drops them…": the ghost goes back to `order 3`; undo restores the override |
| Apply | **Apply to prefab** (`EditorScene::apply_override` → `EditorSession::apply_to_prefab`) writes the overridden fields into the root of the `.mcprefab`, then re-cooks and hot-reloads. Every instance then sees them, and they are no longer overrides on this one. EntityRef fields are not applied: they name this scene's entities. The placement is refused. The prefab file write is outside the undo history; the instance side is undoable. | "…apply writes them into the prefab for every instance": the file gains `size: [5, 5]`; instance A, with no override, now shows size 5 and order 0 |
| Open-prefab-asset editing | `SceneDocument` has a `DocKind`: a `.mcprefab` opens like a scene (entity 0 is the root, no settings) and saves as `mce-prefab`. The asset browser opens a prefab on double-click or with "Open prefab". The Open dialog lists prefabs, and Save As keeps the extension. A saved prefab re-cooks its users, and their ghosts follow on hot reload. | "a prefab asset opens, edits and saves; instances follow on reload": A inherits `order 7`; B keeps its override |

**Model limits.** Overrides target the instance root only, matching the cook (`scene_importer.cpp` applies an instance's `components` to the flattened root). The prefab's child entities have no stable id; the source and cooked formats know them only by index. Per-child overrides therefore need a stable child id in the prefab format first, which is the next step if a game needs it. `docs/architecture/assets.md` sketches a separate `"overrides": {"path": value}` key; the shipped format keeps the existing `components` object, now read field by field, so no file migrates.

## Not built in this pass

- **Timeline / curve editor (FR-EDT-130 / FR-ENT-110):** the first rung of the binding cut ladder, so it goes first.
- **Asset thumbnails:** rung 1b; the asset browser stays a typed list.
- **Multi-scene tabs:** one open document at a time. A prefab opens in the same slot as a scene.
