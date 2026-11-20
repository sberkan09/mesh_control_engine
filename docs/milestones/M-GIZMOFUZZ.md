# M-GIZMOFUZZ — fuzzing the transform gizmo (milestone record)

**Status:** done 2026-11-20 (one step, no defect found). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-CAMNAMEFUZZ, defined on
the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random trees (turned and scaled entities, random parents, random selection) per mode: a move drag shifts every selected entity's world position by the drag and cancel restores the local values; a rotate drag turns every selected entity about the pivot; a uniform scale drag scales every offset from the pivot | done 2026-11-20: `tests/editor/test_transform_gizmo.cpp`; no defect: a selected child follows its selected ancestor and ends where a free entity would |
