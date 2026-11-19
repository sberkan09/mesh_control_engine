# M-SELHISTORY2 — fuzzing the selection history (milestone record)

**Status:** done 2026-11-19 (no defect found). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-SELHISTORY, defined on the
coordinator's instruction to keep opening editor and tooling work. No Linear issues. Tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 sequences of 40 to 240 operations (visit including nothing and the current entry, back and forward with a changing set of deleted ids, clear) against a plain vector and position: the size (never above the cap of 64), the current entry, `can_back` and `can_forward` after every step | done 2026-11-19: `tests/editor/test_selection_history.cpp`; the history matched the model on every step |
