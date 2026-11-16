# M-SCENEREFS — Entity references that point nowhere (milestone record)

**Status:** done (one step). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-25 as a follow-up of M-SCENELINT, on the coordinator's suggestion (scene reference check). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** A component can hold a reference to another entity (an `AttachToBone` target, a follow target, a trigger link). When that entity is deleted from the file by hand, or an id is mistyped, the scene still loads and the reference silently does nothing. `scene_lint` (editor `Scene lint`, `mce scene lint`, MCP `scene_lint`) now reports it as `refers to an entity the scene lacks` (`dangling_entity_ref` in MCP).

**Rules.** Every reflected entity-reference field, nested structs and arrays included, is checked. `ChildOf` is left to the existing missing-parent finding. Prefab instances are not checked: their references may name entities of the prefab, which the document does not hold. A null reference is fine.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `LintKind::DanglingRef` in `lint_scene`, the MCP kind name and tool description, user guide rows, a `test_scene_lint` case | done (#795) |

**Gates:** `test_editor`, `mcp_transcript` (the tool description is in the golden).
