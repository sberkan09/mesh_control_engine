# M-HARDEN2 — Hostile text in the editor's own files (milestone record)

**Status:** step 1 in review. **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-29 on the coordinator's instruction to spend idle time on quality (no gameplay modules, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Why.** M-FUZZ put a deterministic mutation test over the pack readers, the player's data, the net, scripts, importers, the command line, `.mcperf` and the scene document. The editor and the MCP server also read and write small files that an agent or a person edits by hand: the recent-files list, the camera bookmarks, the project's `.mceproj` and the locale tables behind the Strings panel. None of those had a mutation test, and the first run of one found a real bug: a locale row with a key and no text column (or an empty file) made `StringsGrid::set_text` read past the end of a row.

**Scope.** Device-free and GPU-free: a fuzz test per reader (mutated bytes, and for JSON one value swapped for another kind), the fix of whatever it finds with a regression test beside the module's own tests, and a place in the `fuzz` ctest label so the ASan stage runs it.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `tests/fuzz/test_editor_files.cpp`: the recent list, the camera bookmarks, `.mceproj` (budgets and a settings patch) and the strings grid over mutated locale tables; fix of the short-row crash in the strings grid | in review |
| 2 | The remaining editor readers: selection sets, the sequence and mixer forms, `.mcanim`/behaviour-tree/UI document editors, tilemap and terrain files | todo |

**Gates:** `test_fuzz_editor_files` (label `fuzz`, run by the ASan stage); `StringsGrid: a row with a key and no text column…` in `test_strings_grid.cpp`. The fix is in editor code, not the engine core.
