# M-RESETDUP — fuzzing Reset transform and Duplicate offset (milestone record)

**Status:** done 2026-11-19 (step 1 found one defect in Duplicate offset, step 2 fixed it). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-MATCH2, defined on
the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Editor core and tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random trees: Reset transform (selection at identity, the rest untouched, exact count, repeated ids once) and Duplicate offset (every copy at its original's world position plus the offset) | done 2026-11-19: `test_reset_transform.cpp` passes as is; `test_duplicate_offset.cpp` showed that a copy of a child whose parent was not selected lands at the root and was shifted from its local position |
| 2 | Duplicate offset starts each copy's offset from its original's world position (copies come back in document order) | fixed 2026-11-19: `duplicate_offset.cpp`; named case "shifts a copy from the original's world position when its parent is not copied" |
