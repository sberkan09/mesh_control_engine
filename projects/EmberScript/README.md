# Ember Step (Luau)

A small side-view platformer written only in Luau scripts and data: no C++, no compiler. It is the
demo of the script template (user guide chapter 27, section 27.8).

| Path | What |
|---|---|
| `assets/scenes/main.mcscene` | one level: ground, a ledge, the player, a walker enemy, the goal flag |
| `assets/scripts/player.luau` | run, jump with coyote time and a jump buffer, short hop, respawn |
| `assets/scripts/enemy.luau` | patrol / chase / return state machine; stomp or hurt on touch |
| `assets/scripts/goal.luau` | reaching the flag ends the level |
| `tests/*.mctest` | scenarios `test_run` plays (the engine's ctest `EmberScript_mctests` runs them) |
| `tests/run_right.input` | an input script; the determinism stage plays it (Debug = Release hash) |

Controls: Move (A/D, arrows, stick) and Jump (Space). A script's memory lives in `ScriptVars`.
