# M-HIERTOOLS — hierarchy commands as MCP tools (milestone record)

**Status:** step 1 done 2026-11-20. **Owner: the "Geliştirme rutini 3" thread.** Follows M-LAYOUTTOOLS. The editor's Hierarchy and Edit menus can re-parent, rename, group, ungroup and reset the transform of entities; the MCP server could only create, delete, duplicate and set components, so an agent could not build or reshape a hierarchy headless.

## Steps

| # | Step | State |
|---|---|---|
| 1 | MCP tools `entity_set_parent`, `entity_rename`, `entity_group`, `entity_ungroup` and `entity_reset_transform` (entities by id or name, one undo step each) | done 2026-11-20: `tools_hierarchy.cpp`, `test_mcp_layout.cpp`; transcript golden updated |
| 2 | Reorder tool (`move_entity`) and batch rename by pattern | open |
