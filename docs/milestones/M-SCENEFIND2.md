# M-SCENEFIND2 — fuzzing `find_entities` (milestone record)

**Status:** done 2026-11-18 (the fuzz found no defect). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-SCENEFIND, defined on
the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Tests only (no source change).

## Why

`find_entities` answers name, component and prefab questions over a scene document and builds each hit's hierarchy path. A scene
file can carry parents that do not exist, an entity that is its own parent, or a loop of parents; the path walk must end on all of
them, and the hits must be exactly what a plain scan finds.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random scenes (mixed-case, empty, slashed and non-ASCII names; Sprite and LocalTransform added and removed; parents missing, itself or a loop) against 8 name parts x 3 component filters: the hits equal a plain scan, in file order, every path ends in its entity's name, the prefab-only query finds nothing in a scene without prefabs | done 2026-11-18: `tests/editor/test_scene_find.cpp`, 27,977 assertions, no defect and no hang on loops |
| 2 | Fix what step 1 finds | not needed |
