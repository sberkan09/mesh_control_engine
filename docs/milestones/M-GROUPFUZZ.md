# M-GROUPFUZZ — fuzzing group and ungroup against a plain model (milestone record)

**Status:** done 2026-11-18 (no defect found). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-GROUP and M-EDITUX2, defined on the
coordinator's instruction to keep opening editor and tooling work. No Linear issues. Tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random trees with 4 rounds each of group or ungroup over a random selection (a repeated id, an id that was never there, a parent with its child) against a plain parent map: the group's parent, the top-level entities under the group, every other parent unchanged, the ungrouped entities gone and their children handed up in selection order, one undo entry or none, world positions unchanged, `verify_world` | done 2026-11-18: `tests/editor/test_group_entities.cpp`; both commands matched the model on every seed |
