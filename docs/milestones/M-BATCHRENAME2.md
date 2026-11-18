# M-BATCHRENAME2 — fuzzing `batch_rename_expand` (milestone record)

**Status:** done 2026-11-18 (step 1 found one defect, step 2 fixed it). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-BATCHRENAME, defined
on the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Editor core and tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 500 random patterns (`*`, runs of 1 to 20 `#`, plain text), old names up to 40 characters, random index and start, against a plain expansion cut to 63 characters | done 2026-11-18: `tests/editor/test_batch_rename.cpp`; found that a run of more than 16 `#` was padded to 16 digits, not to the run's length |
| 2 | The cause: the padding loop had a stray cap of 16 | fixed 2026-11-18: `batch_rename.cpp` pads to the run's length (the name capacity still cuts the result); named case "pads a long run to its length" |
