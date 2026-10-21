# M-ASSETNEW — Creating, duplicating and deleting assets in the editor (milestone record)

**Status:** in progress 2026-10-21. **Owner: the "Geliştirme rutini 3" thread** (a gap taken from the assessments: no unowned milestone was left). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/motor-erisim-bosluklari.md`, section 11 (Editor): the JSON source files "are hand-written for people too". The editor opens a material, particle, sequence or string table in a panel, but the Assets panel cannot make one: only MCP's `asset_create` can.

**Goal.** The Assets panel's context menu gets **New** (a starter file of any JSON source kind at a path the user types, cooked before it is written), **Duplicate** (a copy with a fresh GUID) and **Delete** (after a confirmation, the file and its sidecar). The new file opens in its panel.

**Not in scope:** scenes and prefabs (the scene menu already makes them), textures and meshes (imports, not templates), moving folders.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Seam: the asset kind table (extension, format, fields, starter object) moves from the MCP asset tools to editor core (`mce/editor/asset_kinds.h`); MCP uses it from there, nothing changes for MCP clients | in progress |
| 2 | `create_asset`, `duplicate_asset` and `delete_asset` in editor core, cook-first, with tests | planned |
| 3 | The Assets panel's New, Duplicate and Delete entries, guide row | planned |

**Gates:** `test_asset_kinds.cpp` and the MCP tests (step 1); the core tests (step 2); step 3 is build-checked (no display in CI).
