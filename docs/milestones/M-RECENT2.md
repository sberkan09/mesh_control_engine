# M-RECENT2 — fuzzing the Open Recent list (milestone record)

**Status:** done 2026-11-19 (no defect found). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-RECENT, defined on the coordinator's
instruction to keep opening editor and tooling work. No Linear issues. Tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 sequences of 40 operations (touch, remove, clear, save-and-load) with caps 0 to 5 and a small path pool (an empty path included) against a plain vector: newest first, no duplicates, never above the cap, remove's return value, and a serialize/parse round trip that gives the same list | done 2026-11-19: `tests/editor/test_recent_list.cpp`; the list matched the model on every step. Paths holding a newline are outside the model (the file format is one path per line) |
