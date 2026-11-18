# M-SCENEDIFF2 — fuzzing `diff_scenes` (milestone record)

**Status:** done 2026-11-18 (the fuzz found no defect). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-SCENEDIFF, defined on
the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Tests only (no source change).

## Why

`diff_scenes` matches entities by id and reports added, removed and changed ones, a reorder, and header changes. The unit tests cover
each report alone. A tool that reviews or merges scenes must never call two different scenes equal, nor two equal ones different.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 100 random edit sequences (create, delete, rename, transform set, component add/remove, reorder, reparent) on an `EditorScene`: the diff is empty exactly when the two documents serialize to the same bytes, `format_diff` is empty exactly then too, the reversed diff lists the same entities with Added and Removed swapped, and undoing every edit makes the diff empty | done 2026-11-18: `tests/editor/test_scene_diff.cpp`, 1,766 assertions, no defect found |
| 2 | Fix what step 1 finds | not needed |
