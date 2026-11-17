# M-EDITUX3 — selection commands and structural edits in one undo sequence (milestone record)

**Status:** done 2026-11-17 (the mixed fuzz found no defect). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of
[M-EDITUX](M-EDITUX.md) and [M-EDITUX2](M-EDITUX2.md), defined on the coordinator's instruction to keep opening editor and
tooling work while the Linear Backlog holds only device-bound issues. No Linear issues. Editor tests only (no source change).

## Why

M-EDITUX fuzzed structural edits (reparent, copy/paste, delete, prefab instances) and M-EDITUX2 fuzzed the selection commands
(group, align, flip, snap, match, reset, duplicate-with-offset), each in its own sequence. A user mixes them: group two
entities, delete the group's child, paste, reparent, align. An interaction bug (an edit group left open, an id reused after a
delete, a paste that leaves the world out of step) only shows when both kinds share one history.

## Steps

| # | Step | State |
|---|---|---|
| 1 | The M-EDITUX2 fuzz gains a mixed mode: half the commands are create, delete, reparent (including refused cycles) and copy/paste of a random selection, the rest the selection commands; same invariants (one entry or none, undo all byte-identical, redo all exact, `verify_world`) | done 2026-11-17: `tests/editor/test_multi_commands.cpp`, 30 seeds x 120 commands each mode, 24,222 assertions for both cases; no defect found |
| 2 | Fix what step 1 finds | not needed |
