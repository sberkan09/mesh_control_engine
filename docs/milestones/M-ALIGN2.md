# M-ALIGN2 — fuzzing Align, Distribute and Snap (milestone record)

**Status:** done 2026-11-19 (step 1 found the defect, step 2 fixed it for all three commands). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-FLIP2, defined on
the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Editor core and tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random trees with a random, shuffled selection for Align, Distribute and Snap to grid | done 2026-11-19: `test_align_entities.cpp`, `test_snap_entities.cpp`; a child listed before its parent, or an on-grid child under a parent that snaps, ended off its target because the parent moved afterwards and carried it |
| 2 | Shared helper `sort_parents_first` (depth = selected ancestors, stable) used by Flip, Align, Distribute and Snap; every selected entity is placed, parents first | done 2026-11-19: `parents_first.h`; named cases for Align and Snap |
