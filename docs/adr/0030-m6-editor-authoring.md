# ADR-0030: M6 editor authoring (undo/redo, gizmos, hierarchy, prefabs, asset browser, tilemaps)

**Status:** Accepted (2026-10-08). **Milestone:** M6 (editor v0.5). **Requirements:** FR-EDT-040 (undo), FR-EDT-030
(gizmos), FR-EDT-050 (multi-select, drag-drop), FR-EDT-080 (prefab placement), FR-EDT-110 (asset browser),
FR-EDT-120 / FR-R2D-115 (tilemaps).
**Design:** `docs/architecture/editor-tools.md` §2.4–§2.7, §3.2.

**Context.** M6's editor half turns the M3/M4 authoring loop into the v0.5 editor: undo/redo (which must also cover
edits the MCP server makes), rotate/scale gizmos with multi-select, a tree hierarchy over `ChildOf`, prefab content in
the viewport, an asset browser with drag-drop, and a tilemap panel. The design asked for a command stack over
reflection-serialized property diffs with drag coalescing, bounded at 256 entries / 16 MiB, and named its acceptance
test: N random inspector/gizmo operations, undo all, the document byte-identical to before. Since then the editor MCP
server (ADR-0027) also edits through `EditorScene`, and hot reload (ADR-0029 §2) re-opens a scene another process
saved. Both need to sit in the same history. Sections are added as each part lands.

## 1. Undo/redo: the funnel records, commands do not

1. Every `EditorScene` mutation records each entity it is about to change as an `EntitySnapshot` (index in the
   document, name, prefab link, a byte copy of every component; or "absent"). When the edit ends, the same entities
   are snapshotted again. An `UndoEntry` is the pair of sides; undo applies the before side, redo the after side.
   There is no per-command inverse to get wrong: deleting an entity records it and every entity whose reference to it
   was nulled, so undo restores them all; paste records its new entities as absent.
2. Applying a side erases those entities and re-inserts the present ones in ascending index. Snapshots taken after a
   structural change inside the same edit (an insert, an erase or a reorder) have their indices mapped back through
   the edit's structural log, so each side holds indices valid for the document it restores. The preview world is
   rebuilt from the document after an undo/redo (the fuzz test also runs `verify_world`).
3. Not in the history: the selection, the camera, Save As's fresh GUID.

## 2. Edit groups, coalescing, bounds

1. `begin_edit(label)` / `end_edit()` (nesting; the outermost pair wins) make one entry of anything in between: a
   gizmo drag of a multi-selection, Delete of a selection, Cut, Create + place, Duplicate, a whole MCP `tools/call`.
   A mutation outside a group is its own entry. Entities whose snapshots did not change are dropped (unless they were
   reordered), and an edit that changed nothing (a cancelled drag) pushes nothing.
2. `set_component` carries a key of (entity, component type). An edit with the same key as the newest entry merges
   into it until `seal_undo()`; the shell seals whenever no ImGui widget is active, so one inspector drag is one step.
3. 256 entries and 16 MiB of snapshot bytes; the oldest entries go first. Opening, recovering or starting a scene
   clears the history. Undo/redo refuse while a group is open.

## 3. MCP edits share the history

1. `mce_mcp` wraps every tool call in an edit group named after the tool and adds `scene_undo` / `scene_redo`.
2. In the editor, a scene that changed on disk (an MCP client's `scene_save`, a text editor) is re-read with
   `EditorScene::reload_text` as one "Reload from disk" entry when the document has no unsaved edits, so Ctrl+Z brings
   back what the other tool replaced. When the GUID, name or settings differ, it is a different scene and the history
   starts fresh.

**Consequences.** Memory per entry is the touched entities' component bytes twice, which is small for authoring edits
(a drag of 50 entities is a few KiB). A "Reload from disk" entry holds every entity on both sides; at sample scale
that is tens of KiB, and the byte bound evicts old entries if a large scene reloads often.

## 4. Rotate/scale gizmos and multi-select

1. **Still hand-rolled (ADR-0025 §1).** `TransformGizmo` (formerly `TranslateGizmo`, kept as an alias) gains two
   modes. Rotate is a ring around the pivot; the drag angle turns every selected entity about the pivot (snap: 15°).
   Scale has X/Y handles and a centre square for uniform scale; each entity's scale is multiplied and its offset from
   the pivot scaled with it (snap: 0.1; the factor never goes below 0.01, so a drag through the pivot cannot mirror).
   W / E / R pick the mode (Edit menu too); the undo label is the mode's name.
2. **A multi-selection transforms as one group** about the selection anchor. Esc or right-click cancels and restores every
   entity's `LocalTransform`; the whole drag is one undo entry (§2.1).
3. **Selecting several entities.** In the viewport, Ctrl toggles and Shift adds; a drag over empty space draws a
   marquee and selects what it touches on release (Ctrl/Shift add). In the hierarchy, Shift selects the range from
   the last clicked row (`EditorScene::select_range`), Ctrl+Shift extends it.

## 5. Tree hierarchy (`ChildOf`)

1. **The document stays flat; the tree is a view of `ChildOf`.** The Hierarchy panel draws roots and their children
   as a tree in document order. Dropping a row on another re-parents it (`EditorScene::set_parent`), Shift-drop
   reorders it before that row, and dropping below the tree unparents it. The context menu has Create Child, Create
   Entity Below and Unparent.
2. **Re-parenting keeps where the entity sits.** `set_parent` computes the `LocalTransform` that keeps the world
   placement under the new parent, adds or removes `ChildOf`, moves the child after its new parent's subtree, refuses
   cycles, and is one undo entry. Deleting an entity deletes its subtree, as the runtime does.
3. **The preview places children itself.** The preview world runs no systems, so `EditorScene` writes
   `WorldTransform` (the `LocalTransform` composed up the chain) into it whenever a scene uses `ChildOf`. Picking,
   selection outlines, gizmos and the orbit colliders use `world_transform()`; gizmos write back through
   `local_for_world()`, and an entity whose ancestor is also selected is left to its parent, so a subtree never
   moves twice. `verify_world` ignores the derived
   `WorldTransform`.
4. **The runtime gets the real hierarchy.** `scene::instantiate` installs the hierarchy (`install_hierarchy`) when a
   scene has a `ChildOf` column, so the play world and `mce_player` propagate transforms. `kit::SpriteExtract` draws
   an entity at its `WorldTransform` when it has one and at its `LocalTransform` otherwise, so flat scenes draw as
   before.

## 6. Prefab content in the viewport

1. **Ghosts, not document entities.** A prefab instance stays one entity in the document (its prefab link and its
   overrides). The preview world shows the prefab's entities as preview-only "ghosts": `EditorScene` reads the cooked
   prefab from the mounted tree and instantiates it into the preview world without the runtime hierarchy. Ghosts are
   never picked, saved or seen by undo.
2. **Placed at the instance.** The instance's components (except `ChildOf`) override the prefab root's, which is how
   the cook flattens an instance. Ghost `WorldTransform`s are composed from the instance's world transform down the
   prefab's `ChildOf` links, so moving, rotating or undoing the instance moves its content.
3. **Kept current.** Ghosts are rebuilt with the preview (a document change, a rebuild) and after a hot reload that
   swapped assets, so a prefab edited on disk shows its new layout without reopening the scene.
