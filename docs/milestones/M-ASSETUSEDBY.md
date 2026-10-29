# M-ASSETUSEDBY — Who uses this asset, for an agent (milestone record)

**Status:** done (one step). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-26 on the coordinator's suggestion (asset pipeline, device-free, outside traffic, pedestrians and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** M-ASSETREFS gave the Assets panel **Used by...**, but an agent working over MCP could only learn that an asset is unused (`project_check`, `unused`) and had no way to ask what names an asset before an `asset_delete` or a rewrite. `asset_used_by` answers it with the same `AssetRefs` the panel uses.

**Rules.** Same as M-ASSETREFS: only asset files are read; a script that loads by path, the start scene and files outside `assets/` are not seen.

## Steps

| # | Step | State |
|---|---|---|
| 1 | MCP `asset_used_by` (`path` or `guid`; `count`, `used_by`), a `test_mcp_team` case, user guide 14 row, capabilities row, the transcript golden | done (#803) |

**Gates:** `test_mcp_team`, `mcp_transcript`.
