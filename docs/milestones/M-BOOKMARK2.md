# M-BOOKMARK2 — fuzzing camera bookmarks (milestone record)

**Status:** done 2026-11-19 (step 1 found one defect, step 2 fixed it). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-CAMBOOKMARK, defined on
the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Editor core and tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 seeds: a serialize/parse round trip keeps every slot exactly, and parsing random text never stores a non-finite number | done 2026-11-19: `tests/editor/test_camera_bookmarks.cpp`; round trip is exact; a number such as 1e300 is finite as a double but became infinity in the float slot |
| 2 | `parse` also rejects a number that is not finite as an f32 | fixed 2026-11-19: `camera_bookmarks.cpp`; named case "parse skips a line whose number overflows a float" |
