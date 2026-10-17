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
| 2 | Hidden-state checks: freeze reachable tables after load and watch upvalues and buffers per tick ([design note](M-SCRIPT3-step2-design.md)) | Opus (touches the VM; design note approved) | 2a and 2b done; 2c (static check widening) not started |
| 3 | Dungeon `perf_gate` counter baseline | Sonnet | done |
| 4 | A real blind test: a fresh agent with only the guide builds a small script-only game; friction goes to `report_friction` | Sonnet | not started |
| 5 | Gaps the blind test 3 friction notes found (2026-10-16): an entity lookup by name in `mce` (screens and markers are found by comparing a document guid today); a way for Luau to raise a perceivable noise (`kit::Noise` is not a reflected event, so a trap script needed a C++ component); whether `ScriptSystem` runs while a cutscene holds gameplay (it does not, and chapter 13.6 does not say); `mce.get` / `mce.set` over several components in one call | Sonnet | name lookup is M-WORKFLOW 7 PR 10 (`mce.find`, Rutin 3); `during_cutscenes` and multi-component `mce.get` / `mce.set` already shipped with M-WORKFLOW 7; `mce.make_noise` in progress |
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

## Step 2a as built

- `ScriptHost::watch_state` (end of `add_bytecode`) walks from `update` and the script's globals under `lua_cpcall`: tables are made read-only, upvalue slots, C-closure upvalues (`string.gmatch` keeps its position in one) and buffers go on a watch list held with `lua_ref`; the shared globals and library tables are skipped (the host writes `mce.dt`).
  `state_unchanged` compares after each `update()` (`lua_rawequal`, numbers and vectors by bit pattern, buffers by `memcmp`). A change fails the script with `update() kept state between ticks: ...`; a write to a frozen table fails at its line.
- Tests (`test_script_state.cpp`): alias, helper, closure factory, global table, `newproxy`, buffer and `gmatch` each compile and then fail at run time; `check_state = false` lets them run; a stateless script with constant tables, NaN and vector upvalues, `mce.random` and field handles runs 100 ticks with the same `state_hash` on or off.
- Default is on in the field; step 2b makes it off in shipped games and on in editor, `mce run`, headless and CI (coordinator decision), and adds the headless exit code and `debugLevel = 2`.
- Kept `newproxy` (its metatable is frozen). Watched buffers are capped at 256 KiB per script.

## Step 2b as built

- `script.state_check` (-1 auto, 0, 1), read by `install_scene_scripts`. Auto: on in the editor's play mode, `mce_mcp` runs, `mce_player -headless` and any run with `-pack` or the build tree's pack;
  off only in a windowed game whose pack sits beside the executable (the shipped layout, `PlayerDesc::pack` found there). Not covered by a test: the shipped-window default needs a window.
- `mce_player -headless` exits 3 when a script ended `Failed` (the state hash is still printed, the errors go to stderr), so the determinism stage and CI notice; `player_scripts` pins it and the convar.
- Scripts compile with `debugLevel = 2` (local and upvalue names) and the script importer is version 3, so cooked scripts rebuild; the state error names `upvalue 'n'`.
  Pack size: +704 B on Dungeon's 12 KB pack (+6.0%) and +512 B on Pursuit's 24 KB pack (+2.2%); about 0.5 to 0.7 KB per script, so the relative cost only shows on toy packs. The coordinator's rule
  was level 1 in the ship cook if the cost passes 5%; it is kept at level 2 everywhere for now (a ship-only cook switch would be a new cook option), to be decided at review.

**Gates:** every new function has a `tests/script` case and a `mce.d.luau` entry; `local_ci.sh --task` green on every step.
