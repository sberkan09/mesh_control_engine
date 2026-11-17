# M-SCENELINT2 — fuzzing `fix_scene_lint` (milestone record)

**Status:** done 2026-11-17 (step 1 found one defect, step 2 fixed it). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of
M-SCENELINT, defined on the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Editor core and
tests only.

## Why

`fix_scene_lint` promises that `lint_scene` is clean afterwards. The unit tests check each finding kind alone; a real broken scene
mixes them, and one repair can change the groups another finding is measured in.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random broken scenes (empty and repeated names, parents that are missing, the entity itself or a loop, references to nothing, zero scales, NaNs): the repair leaves the scene clean, a second run changes nothing, every entity and id is kept, an entity under a parent that stays keeps its name, and the result saves and loads to the same bytes | done 2026-11-17: `tests/editor/test_scene_lint.cpp`; 26 of the 200 seeds failed, all one cause |
| 2 | The defect: a parent link that names nothing (or the entity itself) is dropped, so the entity becomes a root and may share its name with other roots, but the duplicate findings were taken before that change, so it stayed unrenamed and the scene was not clean after one repair | fixed 2026-11-17: the duplicates are looked up again after the parent links and names are repaired (`scene_lint.cpp`); named case "fix_scene_lint renames the duplicates a dropped parent link creates" |
