# Dungeon (Luau)

A top-down crawl over levels the engine generates from a seed, written only in Luau scripts and data: no C++,
no compiler. It is the demo of `ProcLevel` (user guide chapter 28) and of a script-only project (chapter 27).

| Path | What |
|---|---|
| `assets/scenes/main.mcscene`, `caves.mcscene`, `walk.mcscene` | one `ProcLevel` each: rooms and corridors, caverns, a winding passage (48 x 32 cells, seed 1) |
| `assets/scripts/dungeon.luau` | walk to the Exit; the next seed builds the next level; ten levels clear a scene and the next generator's scene loads; the autopilot follows the level's Path markers |
| `tests/clears_*.mctest` | per generator: the autopilot clears ten seeds (1 to 10) and every seed of the generator has a reachable exit (the engine's ctest `Dungeon_mctests` runs them) |
| `tests/autopilot.input` | an input script that switches the autopilot on; the determinism stage plays it (Debug = Release hash) |

Controls: Move (WASD, arrows, stick); Jump (Space) hands the walk to the autopilot and takes it back.
A script's memory lives in `ScriptVars`; the level itself is data (`ProcLevel`: kind, size, seed), so a saved
game holds a seed, not a map.
