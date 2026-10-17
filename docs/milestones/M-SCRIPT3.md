# M-SCRIPT3 — Scripts that rotate, tick on events and stay honest (milestone record)

**Status:** claimed 2026-10-17 by the M-SCRIPT2 thread (no Linear issues yet: the workspace is at its free limit, this record is the tracker).

**Why:** M-SCRIPT2 gave a script the whole 2D and 3D physics, navigation, save and feel API, and two script-only
demos (Dungeon, Pursuit) run on it. The gaps those demos and the milestone records left open are all in what a script
cannot yet say or check:

- **Rotation.** `teleport3d` keeps the rotation and Luau has no quaternion; a 3D script cannot face a chaser at the hero
  or turn a door. (M-SCRIPT2 step 4 "Not covered".)
- **State outside components.** The compiler catches top-level locals changed from `update()` but not every route
  (a generalized `for k, v in t` over object keys, upvalues of nested closures); a hidden state breaks saves and replays.
- **Guarding the demos' budgets.** Dungeon has no `perf_gate` counter baseline, so a script that gets slower is not caught.
- **A blind test.** Both demos were written by the API's author; no fresh agent has built a script-only game from the guide.

**Not in scope:** new VM features, coroutines, source compilation in the player, networking from scripts.

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | Rotation from Luau: a quaternion value, `mce.look_at3d`, `mce.rotate3d`, `teleport3d` with a rotation | Sonnet | not started |
| 2 | Hidden-state checks: close the loop and closure routes, a run-time `state_hash` probe in the script determinism stage | Opus (touches the VM and the compiler check; design note first) | not started |
| 3 | Dungeon `perf_gate` counter baseline | Sonnet | not started |
| 4 | A real blind test: a fresh agent with only the guide builds a small script-only game; friction goes to `report_friction` | Sonnet | not started |

**Gates:** every new function has a `tests/script` case and a `mce.d.luau` entry; `local_ci.sh --task` green on every step.
