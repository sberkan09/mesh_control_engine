# M-SCENESTATS2 — fuzzing `scene_stats` (milestone record)

**Status:** done 2026-11-18 (step 1 found one off-by-one, step 2 fixed it). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of
M-SCENESTATS, defined on the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Editor core and
tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 random scenes (Sprite and LocalTransform added and removed; parents missing, the entity itself or a loop) against a plain scan: entity and root counts, component uses (most used first), bounds, depth (walked independently when the scene has no loop; below the entity count when it has one) | done 2026-11-18: `tests/editor/test_scene_stats.cpp`; found that a loop of parents reported a depth one above the longest possible chain (depth 4 in a 3-entity scene) |
| 2 | The cause: the depth walk's guard allowed one step too many | fixed 2026-11-18: the guard stops after n - 1 steps (`scene_stats.cpp`); named case "a loop of parents reports a depth below the entity count", which also checks that a real chain keeps its full depth |
