# M-ASSETREFS — Who uses this asset (milestone record)

**Status:** done 2026-10-21. **Owner: the "Geliştirme rutini 3" thread** (a gap taken from the assessments: no unowned milestone was left). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/unreal-kiyaslama-2026-10-16.md`, Editor row ("deep editor tools"): Unreal has a Reference Viewer and "Unused assets"; the Assets panel could only warn about references when deleting.

**Goal.** Ask the Assets panel which assets name a given one (**Used by...**) and which assets nothing names (**Unused**), without deleting anything first.

**Not in scope:** following references through binary imports, references from code or scripts that load by path, a graph view.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `AssetRefs` in editor core (`mce/editor/asset_refs.h`): one pass over the project's text assets collects every GUID written in them; `used_by`, `use_count`, `unused` (scenes left out), with a test | done |
| 2 | Assets panel: **Used by...** in the context menu, an **Unused** toolbar button (click an entry to jump to its folder), guide row | done |

**What "unused" means.** No asset file of the project (up to 8 MB, text) writes the GUID. A scene's start-scene setting, a script that loads by path, and anything outside `assets/` are not seen, so the list is where to look, not a list of files safe to delete; the guide says so.

**Gates:** `test_asset_refs.cpp` (step 1); step 2 is build-checked (no display in CI).
