# M-EDITUX2 — the multi-entity editor commands under undo (milestone record)

**Status:** done 2026-11-17 (step 1 built, step 2 had nothing to fix). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of [M-EDITUX](M-EDITUX.md), defined on the
coordinator's instruction to start a new milestone when the Linear Backlog holds only device-bound work (SBA-112, SBA-58).
No Linear issues. Editor core and tests only; it does not touch render, rhi, physics3d, openworld or kit, so it does not
overlap the visual routine or the game layer.

## Why

M-EDITUX fuzzed reparenting, copy/paste, prefab overrides and the inspector against undo. The commands that act on a whole
selection each have a small unit test, but none runs inside a random sequence next to the others, and a sequence is where an
undo bug shows (a command that records its edit group half-way, or leaves `verify_world` false after the redo):

`group_entities` / `ungroup_entities`, `align_entities` / `distribute_entities`, `flip_entities`, `snap_entities`,
`match_transform`, `reset_transform`, `duplicate_offset`, batch rename.

## Steps

| # | Step | State |
|---|---|---|
| 1 | One fuzz over those commands (random selections including parents with children, empty and single-entity selections, a deleted id): after every command the document parses and re-serializes byte-stable, undo returns the previous text, redo returns the edited text, `verify_world` holds, and a no-op command (nothing selected, one entity to align) leaves no history entry | done 2026-11-17: `tests/editor/test_multi_commands.cpp`, 30 seeds x 120 commands, 11,806 assertions in about 1.2 s (PR #1279); a guard requires more than 40 history entries per seed so a run of no-ops cannot pass |
| 2 | Fix what step 1 finds, one cluster per PR, each with the failing seed as a named case | not needed: the fuzz found no defect (undo byte-identical, redo exact, `verify_world` held, no-ops left no entry, a stale id was tolerated) |
| 3 | Close: record, a line in the editor guide only if a behaviour changed | done 2026-11-17 (this record) |

## Acceptance

- The fuzz runs 30 seeds x 120 commands in the `--task` set in well under 60 s; a longer run, if wanted, is labelled `slow`.
- A failure prints the seed and the command index, and the seed is pinned as its own test case before the fix.
- No change to the document format and no new component fields.
