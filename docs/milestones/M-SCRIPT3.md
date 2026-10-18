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
| 4 | A real blind test: a fresh agent with only the guide builds a small script-only game; friction goes to `report_friction` | Sonnet | done (PR for the guide fixes); open: see "Step 4 as built" |
| 5 | Gaps the blind test 3 friction notes found (2026-10-16): an entity lookup by name in `mce` (screens and markers are found by comparing a document guid today); a way for Luau to raise a perceivable noise (`kit::Noise` is not a reflected event, so a trap script needed a C++ component); whether `ScriptSystem` runs while a cutscene holds gameplay (it does not, and chapter 13.6 does not say); `mce.get` / `mce.set` over several components in one call | Sonnet | name lookup is M-WORKFLOW 7 PR 10 (`mce.find`, Rutin 3); `during_cutscenes` and multi-component `mce.get` / `mce.set` already shipped with M-WORKFLOW 7; `mce.make_noise` merged in #492 (open work: a running perceiver hearing a script noise is not tested end to end) |
| 6 | Docs the same notes asked for: the Luau limits (no state between ticks, one component per `("Comp", "field")` call, `Perceiver.sees` is a number) in the guide itself, and `mce.d.luau` as a reference page in the SDK docs | Sonnet | done: `docs/engine/script.md` (surface, `mce.d.luau`, limits, gaps) and a limits note in guide 27.4; open: none |
| 7 | The perf gate's `--seed` self-check catches nothing on Dungeon (so its baseline cannot be trusted yet): find why three textured sprites added to `walk.mcscene` do not change `sprites` / `sprite_batches`, fix it, and make `Dungeon_perf_counters` run the seeded variant too | Sonnet | done |
| 8 | Bytecode trust (M-FUZZ decision, coordinator 2026-10-17): bytecode loads only from the game's own cooked pack; mods, base packs, fetched config and user files are data-only. Pin it so a new mount site or raw-bytecode caller cannot slip in | Sonnet | done in PR (see "Step 8 as built") |
| 9 | Blind-test gap 1: `mce test <project> [path]` plays `.mctest` scenarios without an agent (the MCP server over stdio) | Sonnet | done in PR (this step) |
| 10 | Blind-test gap 2: named `ScriptVars` slots instead of `v[16]` | Sonnet (touches the field-handle layer in `script.cpp`) | done in PR (this step) |
| 11 | Blind-test gap 3: a distance / overlap query without physics (`mce.near`) | Sonnet | done in PR (this step) |
| 12 | Blind-test gap 4: a headless entity and component dump | Sonnet | not started |

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

## Step 5 as built (PR #492)

- `mce.make_noise(position, loudness?, source?, team?)` in `api_world.cpp`: a thin binding that pushes a `kit::Noise` (loudness in
  metres for a hearing-1 perceiver, default 10; perceivers of `team` ignore it). A negative loudness, or a world without
  perception, is a named error. Declared in `mce.d.luau`, row in guide chapter 27, case in `test_script_api.cpp`.
- Already covered, so not built here: name lookup is M-WORKFLOW 7 PR 10 (`mce.find`, merged); `ScriptSystem.during_cutscenes`
  (M-WORKFLOW 7 PR 8) and multi-component `mce.get` / `mce.set` (`test_script.cpp`, M-WORKFLOW 7) are in `dev_claude`.
- Open: an end-to-end check of a perceiver hearing a script noise; step 6 should mention `mce.find` and `mce.make_noise`.
- Linear: this milestone has no cards (the workspace is at its free limit), so this file is the tracker.

## Step 8 as built (bytecode trust)

- The rule was already in place since M-SCRIPT2 step 0 (`MountOptions::allow_code`, default false; Script entries of such a root are dropped at mount).
  What was missing was a pin: `scripts/ci/check_walls.py` now has two walls. `allow_code = true` may appear only in the player's pack mount, the
  editor project's cook, the headless run's pack and `tests/`; `add_bytecode(` may be called only inside `src/engine/script/` and `tests/`. A new
  mount site or loader fails the walls stage until it is named there on purpose.
- Test: `test_scene_scripts` "a pack mounted without allow_code never runs its scripts": a pack mounted with default options has no Script
  entries and a `ScriptSystem` naming one does nothing.
- Not built: accepting mod or network scripts as `.luau` source and compiling them at load. The shipped player has no compiler by design
  (`player_no_luau_compiler`), so that is a separate decision; until then those sources cannot carry code at all.
- No raw-bytecode API takes an external path today: `ScriptHost::add_bytecode` is reached only from scene scripts (mounted chunks) and `compile()`.

## Step 4 as built (the blind test)

A fresh Sonnet agent that could read only the user guide, `docs/engine/script.md` and its own cook's `mce.d.luau` built a 2D script-only
game (a player, three coins, a chasing enemy, a following camera) with `mce_cook` and `mce_player` and got it cooked and running (the
coin pickup logged, the state hash changed with input). It could not run its `.mctest`. Findings, most costly first:

1. No way to run a `.mctest` without MCP (open: a command that plays a scenario through the player alone; the guide now says where scenarios run).
2. Guide 27.1 said the cook writes the script's sidecar itself; the bare cook needs `--adopt` (fixed).
3. Bootstrap order and a hand-written `.mceproj` for `"games": []` were undocumented (fixed: "By hand, without `mce new`" in 27.7).
4. `ScriptVars` rows must list all 16 numbers in a scene (now stated); named slots instead of `v[16]` are a wish (open).
5. No single script-only 2D scene example, the `$<index>` camera target, the +y-up `Move` axis and `mce.destroy` inside `mce.each` were unstated (fixed in the same section).
6. Wished for and open: `mce.distance` / `mce.overlap_circle2d` without physics, a headless entity or component dump, scene-editable per-script constants.

Not a true independent test of everything: the agent was handed the task, not asked to find its own, and it did not run the editor.

## Step 9 as built (`mce test`)

- `mce test <project> [path]` in `tools/mce_cli/mce.py`: spawns the project's `mce_mcp` (with `--module` for a game project), calls `test_run` once
  (`path` is a `.mctest` file or a folder) and prints each scenario as `pass` or `FAIL <checks>` with script errors; exit 0 only when every
  scenario passed and there was at least one. Run by hand against `projects/Pursuit` with the real server (`tests/collects.mctest` passes).
- `tests/import/test_cli_test.py` (`cli_test`) pins the client, report and exit codes against a fake server; documented in guides 14.3.1, 16 and 27.7.
- Not done: the ctest target of a script project still calls `demo_gate.py`; it could call `mce test`, left alone to keep this PR small.

## Step 10 as built (named `ScriptVars` slots)

- `mce.field("ScriptVars", "v", 3)`: an optional 1-based `element` makes a handle to one slot of an array field, so `mce.get(e, COINS)` is a number and
  `mce.set(e, COINS, n)` writes it, mixed freely with whole-field handles and `("Comp", "field")` pairs. No new component: the scene still lists the
  16 numbers. `FieldHandle` gets an `element` (0 = the whole field); `get` / `set` view the element as a one-element field, so reads, writes and the
  state guard take the existing paths. A bad `element` or a non-array field is a named error; `mce.d.luau` declares the third argument.
- The compile-time name check needs no change (it reads the first two arguments). Test: "mce.field names one element of an array field".
- Not done: moving Dungeon and the other demos onto named slots (it would not change behaviour, only readability); a per-script named-constants table (separate wish).

## Step 11 as built (`mce.near`)

- `mce.near(center, radius, component?, exclude?)` in `api_world.cpp`: the entities with a `LocalTransform` within `radius` of `center` (3D distance; a 2D
  game has z = 0), optionally also holding `component`, nearest first with ties in world storage order (a stable sort), minus `exclude`. A negative radius is
  a named error; an unregistered component matches nothing. It reads `LocalTransform.position`, so a parented entity answers with its local position.
- Chosen over a bare `mce.distance(a, b)`: `vector.magnitude(a - b)` already is that, and the real gap was "who is near this point" without a physics world.
  Linear in the entities with a `LocalTransform`; use `mce.overlap2d` / the casts when a physics world exists and the set is large.
- Test: "mce.near is a distance test without physics"; declared in `mce.d.luau`; guide 27.3 and 27.7.
