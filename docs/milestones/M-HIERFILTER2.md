# M-HIERFILTER2 — fuzzing the Hierarchy search filter (milestone record)

**Status:** done 2026-11-18 (no defect found). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-HIERFILTER, defined on the coordinator's
instruction to keep opening editor and tooling work. No Linear issues. Tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random scenes (repeated and empty names; parents missing, the entity itself or a loop) and 8 queries (blank, one word, several words, no match) against a plain model: the match count, which entities match, and that every match and every entity on its parent chain is visible and nothing else is | done 2026-11-18: `tests/editor/test_hierarchy_filter.cpp`; the filter matched the model on every seed, loops included |
