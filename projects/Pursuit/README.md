# Pursuit (Luau, 3D)

A 3D chase written only in Luau scripts and data: no C++, no compiler. Collect the five coins before the grunt catches you. A wall
with a four metre gap splits the arena, so everything that walks has to plan around it. It is the demo of the 3D side of the
script API (`mce.overlap_sphere3d`, `mce.find_path3d`, `mce.teleport3d`, `mce.shake`) and of a script-only 3D project.

| Path | What |
|---|---|
| `assets/scenes/main.mcscene` | the arena: floor, two walls with a gap, the hero and the grunt (kit `CharacterController3D`), five coins, a follow camera, a `NavMeshSurface3D` baked when the scene starts |
| `assets/scripts/arena.luau` | the hero's pickup and autopilot (Jump hands the walk to it), the grunt's chase, both planned with `mce.find_path3d`; being caught sends the grunt back to its post |
| `assets/models/` | generated boxes and a humanoid, from `samples/07_gameplay3d` (see `CREDITS.md`) |
| `tests/collects.mctest` | the autopilot collects all five coins around the wall while the grunt chases, with no script errors (the engine's ctest `Pursuit_mctests` runs it) |
| `tests/autopilot.input` | an input script that switches the autopilot on; the determinism stage plays it (Debug = Release hash) |

Controls: Move (WASD, arrows, stick), Jump (Space) hands the walk to the autopilot and takes it back.
A script's memory lives in `ScriptVars`; the walking itself is the kit's own `CharacterController3D`, which the script steers by
setting its `move`.

Things this project taught the API (M-SCRIPT2 step 5): `mce.find_path3d` listed the walker's own position first while `mce.find_path`
did not, which stalled the autopilot at the first corner; both now list the corners after `from` only. A character walking a plan
needs a "reached" distance (`CORNER` in the script), or it keeps aiming at a corner it is standing on.
