# M-MATCH2 — fuzzing Match transform (milestone record)

**Status:** done 2026-11-19 (step 1 found the defect, step 2 fixed it). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-ALIGN2, defined on
the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Editor core and tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random trees with a random, shuffled selection (anchor never under a selected parent): every selected entity lands on the anchor's world position | done 2026-11-19: `tests/editor/test_match_transform.cpp`; a child already on the anchor was skipped, then carried away when its parent matched; a child's local value was also computed before its parent moved |
| 2 | Position matching places every selected entity parents first and works out each local value at apply time; the returned count is still the entities that differed | fixed 2026-11-19: `match_transform.cpp`; named case "puts a child on the anchor when its parent is matched too" |
