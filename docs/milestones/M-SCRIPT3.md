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
| 1 | Rotation from Luau: Euler degrees as a vector (no quaternion type), `mce.rotation3d`, `set_rotation3d`, `look_rotation3d`, `teleport3d` with a rotation | Sonnet | done |
| 2 | Hidden-state checks: freeze reachable tables after load and watch upvalues and buffers per tick ([design note](M-SCRIPT3-step2-design.md), awaiting review) | Opus (touches the VM and the compiler check; design note first) | not started |
| 3 | Dungeon `perf_gate` counter baseline | Sonnet | done |
| 4 | A real blind test: a fresh agent with only the guide builds a small script-only game; friction goes to `report_friction` | Sonnet | not started |
| 5 | Gaps the blind test 3 friction notes found (2026-10-16): an entity lookup by name in `mce` (screens and markers are found by comparing a document guid today); a way for Luau to raise a perceivable noise (`kit::Noise` is not a reflected event, so a trap script needed a C++ component); whether `ScriptSystem` runs while a cutscene holds gameplay (it does not, and chapter 13.6 does not say); `mce.get` / `mce.set` over several components in one call | Sonnet | not started |
| 6 | Docs the same notes asked for: the Luau limits (no state between ticks, one component per `("Comp", "field")` call, `Perceiver.sees` is a number) in the guide itself, and `mce.d.luau` as a reference page in the SDK docs | Sonnet | not started |
| 7 | The perf gate's `--seed` self-check catches nothing on Dungeon (so its baseline cannot be trusted yet): find why three textured sprites added to `walk.mcscene` do not change `sprites` / `sprite_batches`, fix it, and make `Dungeon_perf_counters` run the seeded variant too | Sonnet | done |

## Step 3 as built

- `projects/Dungeon/perf/counters.json` holds the walk and caves scenes' render counters; `Dungeon_perf_counters` (in `cmake/ScriptProject`, for any
  script project that has the file) runs `scripts/ci/perf_gate.py` with the project's own host as the player (`perf_gate.py --player`).
  It carries the `gpu` label, so `--task` skips it and the gpu stage runs it. Counters are small by design (2 draws, 9 and 14 sprites): a script that
  starts spawning visible things shows up here; script cost in milliseconds is printed, never judged.
- Step 7 (the `--seed` self-check): the cause was the camera. Dungeon's camera follows the autopilot player, so three 1 m sprites placed in the scene are in
  view for a few frames at most (the player walks away before the 10 warmup frames end). `perf_gate.py --seed` now plants 400 m wide sprites, which are in view
  wherever the camera goes: the seeded run fails on draws, sprites and batches, and `Dungeon_perf_counters_seeded` (gpu label) keeps it so. Not rerun: the
  Glowmoth and MarbleGarden seeded tests (module builds, gpu stage); their scenes only gain larger sprites. `main.mcscene` is not measured: the script leaves it
  within the warmup.

**Gates:** every new function has a `tests/script` case and a `mce.d.luau` entry; `local_ci.sh --task` green on every step.
