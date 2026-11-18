# M-SELRELATED2 — fuzzing `select_related` (milestone record)

**Status:** done 2026-11-18 (step 1 found one defect, step 2 fixed it). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-SELRELATED, defined
on the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Editor core and tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random scenes (parents missing, the entity itself or a loop; the selection may name an id that is gone) against a plain scan of the parent links for all four relations (parent, children, descendants, siblings), each entity once | done 2026-11-18: `tests/editor/test_select_related.cpp`; found that the siblings of an id that is not in the scene came back as the roots. The documented rule that entities of a ChildOf cycle count with the roots (`children_of`) is part of the model |
| 2 | The cause: `children_of(parent_of(missing))` is `children_of(kNoEntity)`, the roots | fixed 2026-11-18: `select_related.cpp` skips an id that is not in the scene for Siblings; named case "an id that is gone has no siblings" |
