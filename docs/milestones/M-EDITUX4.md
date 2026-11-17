# M-EDITUX4 — component edits, inspector passes and gizmo drags in the same undo sequence (milestone record)

**Status:** done 2026-11-17 (the fuzz found no defect). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of
[M-EDITUX3](M-EDITUX3.md), defined on the coordinator's instruction. No Linear issues. Editor tests only (no source change).

## Why

M-EDITUX to M-EDITUX3 fuzzed structural edits and the selection commands against undo. Still outside any random sequence:
`add_component` / `remove_component`, a whole inspector pass (every field of an entity nudged), `move_entity`, and a gizmo
drag over a selection (move, rotate, scale; committed or cancelled), the edits a user makes most.

## Steps

| # | Step | State |
|---|---|---|
| 1 | A third mode of the multi-entity fuzz adds those commands (random offered component types, `NudgeCtx` inspector pass, `TransformGizmo` drag with snap and cancel) to the M-EDITUX3 mix; same invariants (one entry or none, undo all byte-identical, redo all exact, `verify_world`) | done 2026-11-17: `tests/editor/test_multi_commands.cpp`, 30 seeds x 120 commands, 12,162 assertions, about a second; well under the 60 s limit, so no `slow` variant |
| 2 | Fix what step 1 finds | not needed. The only mismatch was the test's: a panel pass over several components is several undo entries by design, so the pass runs in one `begin_edit` group, as a frame's edits are in the editor |
