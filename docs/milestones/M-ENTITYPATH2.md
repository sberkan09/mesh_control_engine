# M-ENTITYPATH2 — fuzzing `entity_path` (milestone record)

**Status:** done 2026-11-18 (step 1 found one defect, step 2 fixed it). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-COPYPATH, defined
on the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Editor core and tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random scenes (parents missing, the entity itself or a loop) against a plain walk: a path has at most one segment per entity, and equals the walk when the links hold no loop | done 2026-11-18: `tests/editor/test_entity_path.cpp`; found that a loop of parents repeated entity names in the path (4 segments in a 3-entity scene) |
| 2 | The cause: the walk was cut by a step count, so a loop was walked past its start | fixed 2026-11-18: the walk stops at the first entity already in the chain (`entity_path.cpp`); named case "entity_path of a loop of parents names every entity once" |
