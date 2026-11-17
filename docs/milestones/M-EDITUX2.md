# M-EDITUX2 — the multi-entity editor commands under undo (milestone record)

**Status:** planned 2026-11-17. **Owner: the "Geliştirme rutini 3" thread.** Follow-up of [M-EDITUX](M-EDITUX.md), defined on the
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
| 1 | One fuzz over those commands (random selections including parents with children, empty and single-entity selections, a deleted id): after every command the document parses and re-serializes byte-stable, undo returns the previous text, redo returns the edited text, `verify_world` holds, and a no-op command (nothing selected, one entity to align) leaves no history entry | planned |
| 2 | Fix what step 1 finds, one cluster per PR, each with the failing seed as a named case | planned |
| 3 | Close: record, a line in the editor guide only if a behaviour changed | planned |

## Acceptance

- The fuzz runs 30 seeds x 120 commands in the `--task` set in well under 60 s; a longer run, if wanted, is labelled `slow`.
- A failure prints the seed and the command index, and the seed is pinned as its own test case before the fix.
- No change to the document format and no new component fields.
