# M-SCRIPT — Luau scripting for games without a compiler (milestone record)

**Status:** active (added 2026-10-16; steps 1–3 and 4 done, 5 next). Plan: ROADMAP §M-SCRIPT; design of record
ADR-0041; Linear SBA-147 … SBA-152.

**Why:** today every game needs a C++ toolchain and, on phones and the web, its own build. A script game is
data that one stock player runs. The spike (ADR-0041, PR #262) showed a sandboxed Luau system keeps
FR-RUN-040 at about 29× the C++ cost.

**Not in scope:** visual scripting, code mods (N10 stays a separate decision), Luau's JIT.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Scripts as assets: `.luau` → `CHNK_SCRIPT`, `ScriptSystem` in scenes, the stock hosts run bytecode only | SBA-147 | done |
| 2 | A complete API: field handles, `Commands`, events, input, resources, physics queries; bench ≤ 0.4 ms | SBA-148 | done |
| 3 | No hidden state: an AST check for outer-local writes and `pairs` over object keys | SBA-149 | done |
| 4a | Checks: `mce.d.luau`, names with "did you mean", type checking, MCP `script_check`, script errors in `run_headless` | SBA-150 | done |
| 4b | The editor: errors that open the script, hot reload in play-in-editor, a Tracy zone per script, script stacks in crash reports | SBA-150 | done |
| 5 | The compiler-free path: `mce new --script`, `mce ship`, phones and web | SBA-151 | not started |
| 6 | Docs and the demo: user guide, kit recipes, Ember Step in Luau, script-only blind test | SBA-152 | not started |

## Step 1 as built

- `mce_script` (the VM host and the scene-script runner) and `mce_script_compiler` (Luau's parser and
  compiler) are two libraries. The cook, the editor and tests link the compiler; `mce_player` links only the
  runner. Walls: `script compiler wall` and `Luau compiler wall` in `scripts/ci/check_walls.py`, and the ctest
  `player_no_luau_compiler` checks the player and `libmce_engine` binaries for `luau_compile`.
- `.luau` cooks through the `script` importer to `CHNK_SCRIPT` (`asset::ScriptView`: the source path and the
  bytecode). Syntax errors are cook errors at `file:line:col`. **Type checking moves to step 4**, where the
  type definitions it needs (`mce.d.luau`) are generated; step 1 checks syntax and compiles.
- `kit::ScriptSystem {script, order, phase, enabled}` names one script in a scene. `script::install_scene_scripts`
  adds `script.pre` (FixedUpdate_Pre) and `script.post` (FixedUpdate_Post); `mce_player`, play-in-editor and
  MCP `run_headless` call it where `MCE_WITH_SCRIPT` is on. When the enabled set changes, a fresh `ScriptHost`
  loads it between two ticks. A world that never had a ScriptSystem is untouched (the component is not even
  registered), so scenes without scripts hash as before.
- `ScriptHost::add(source)` became `script::add_source(host, ...)` in the compiler library; the host takes
  bytecode only and runs scripts in groups (one per phase).
- Tests: `test_script` (cook round trip, `file:line:col`, phase and order, swaps, a missing script, the same hash
  twice), ctest `player_scripts` (cook a project, `mce_player -headless` runs its scripts, a failing script is
  reported with its line, two runs hash alike, a broken source fails the cook).

## Gates

- The determinism stage covers script games (3 runs, Debug = Release) and the Luau-vs-C++ twin.
- Sandbox, step-budget and memory-cap tests stay green; the walls keep Luau's headers in `mce_script` and the
  compiler out of the player.
- A project without scripts builds and hashes exactly as today.
- `local_ci.sh` green on every step.

## Step 2 as built

- The `mce` table covers what a game needs without C++ (user guide 27.3): field handles (`mce.field`, opaque
  tagged light userdata) and multi-field `mce.get` / `mce.set`; `spawn`, `add`, `remove`, `destroy`,
  `instantiate` (a prefab, through the host's `AssetSystem`), `has`, `alive`; `events` / `emit` for any reflected
  event; `signal` / `signals` for the kit's named signals; `play_sound`; input actions; `load_scene` and `quit`;
  2D raycasts, overlaps, `move_and_collide`, velocities and impulses; 3D raycasts.
- Structural changes go through `Commands` (`spawn_raw` on a reserved handle, `set_raw`, the new `remove_raw`),
  so a script behaves like a C++ system: handles are alive at once, components appear at the sync point.
  `mce.spawn` records components in ComponentId order, never the table's key order.
- Events by name: `MCE_EVENT(T, fields...)` reflects an event type (`refl::TypeFlags::Event`); the 2D trigger
  and contact events, the 3D trigger events and `JointBreak3D` carry it. `World::event_queue_raw(type_hash)`
  and `EventQueueBase::push_raw` / `front_data` / `elem_size` give the type-erased access.
- Every C function may longjmp (`LUA_USE_LONGJMP`), so nothing with a destructor lives across a raising call:
  component bytes go through the host's scratch buffer, query and instantiate results through a host-owned
  entity array.
- Bench (`samples/script_headless`, 2,000 entities, Release, this container): 0.77 ms/tick with
  `("Comp", "field")` names, 0.34–0.37 ms with handles and one `get` / `set` per entity (target ≤ 0.4 ms);
  C++ does the same work in 0.013 ms; the state hash is the same in both modes.
- Tests: `test_script` (`test_script_api.cpp`: handles, multi-field calls, the Commands round trip, deterministic
  spawning, events and emit, a real 2D contact read by name, signals, input, scene requests, physics queries,
  prefab instantiation, error messages).

## Step 3 as built

- The compiler (`mce_script_compiler`, so the cook, the editor and tests alike) runs a check over Luau's AST after
  parsing (`compiler/state_check.cpp`). Inside any function it rejects assigning a top-level local, writing into a
  table rooted at one (index writes, `function t.f()`), and passing one to the mutating library calls
  (`table.insert/remove/clear/sort/move`, `rawset`, `setmetatable`, `buffer.write*/fill/copy`). The top level
  itself is free (it runs once at load), and so are `update()`'s own locals and the closures over them.
  Errors are compile errors at `file:line:col`, so a cook fails like on a syntax error (ADR-0041 open question 2:
  the check was chosen over freezing top-level tables, which would not catch upvalue assignment).
- Object keys (open question 3): the check rejects table and function keys the source shows (constructors,
  index writes, locals bound to a table or a function). At run time `pairs` and `next` are the host's and raise on
  a table, function, buffer, thread or userdata key; `pairs` starts its loop with a sentinel key so the VM's
  built-in traversal does not bypass the check. Not caught: a `for k, v in t` loop over such keys (Luau iterates
  it inside the VM). Step 4's type check does not close it either (it does not track key types), so it stays a
  documented gap (user guide 27.4).
- Tests: `test_script_state.cpp` (positions, every rejected form, what stays allowed, the run-time `pairs` error,
  and a fresh VM swapped in mid-run giving the same state hash as an uninterrupted run); the `player_scripts` ctest
  checks that the cook fails on hidden state at `file:line:col`.

## Step 4a as built

Step 4 was split: 4a is the checks every host shares, 4b the editor work.

- `script::check(name, source, out)` (`mce_script_compiler`) returns every problem, sorted by position: a syntax
  error alone, else step 3's state check, the name check and Luau's type check. `compile()` fails on the first;
  the cook's `script` importer (version 2, so cached scripts are checked again) reports them all.
- Names (`compiler/names_check.cpp`): string literals in `mce.field/get/set/each/has/add/remove/spawn/events/emit`
  and the record keys of the tables passed to them are looked up in the reflection registry; a miss gets the
  closest name by case-insensitive edit distance (reach 1–3 with the name's length). Globals the sandbox removes
  (`print`, `os`, `io`, `debug`, `coroutine`, `require`, `collectgarbage`, `getfenv`/`setfenv`, `math.random`)
  name what to use instead.
- Types (`compiler/typecheck.cpp`): `Luau::Frontend` with the old solver, nonstrict by default (`--!strict` opts
  in), against `mce.d.luau` loaded as a definition file. Luau.Analysis throws, so this one file builds with
  exceptions and catches everything at its edge; the rest of the library stays `-fno-exceptions`.
- `mce.d.luau` (`compiler/definitions.cpp`, `script::write_definitions`): the fixed API signatures, then
  `ComponentName`/`EventName` unions and a field list per type from the registry, sorted, byte-identical between
  runs. Function parameters take `string`, not the unions, so a name built at run time is not a false positive;
  the engine's own name check catches literal typos with a suggestion. The cook writes it to
  `cooked/mce.d.luau` when the project has scripts.
- MCP `script_check` (`tools_script.cpp`): a file under `assets/` or source text, every problem with line and
  col, `definitions` returns the file. `run_headless` and `test_run` return `script_errors` for scene scripts
  that failed; a failed script fails a test.
- Tests: `test_script_check.cpp` (positions and suggestions, removed globals, type errors, a clean script,
  `--!strict`, syntax errors first, the compile message, `mce.d.luau` content and determinism),
  `test_mcp_script.cpp` (`script_check`, `script_errors`, `cooked/mce.d.luau`), and the `player_scripts` ctest
  (the cook writes `mce.d.luau` and reports two name problems in one file).

## Step 4b as built

- Hot reload: the editor already swaps a recooked asset in place (`commit_reload` bumps its generation) and keeps
  playing. `scene_scripts.cpp` now mixes each script's asset generation into the signature of the loaded list, so
  the next tick rebuilds the `ScriptHost` from the new bytecode. Scripts keep no state, so a rebuild changes none.
  Test: `scene scripts: a script recooked while the scene runs takes effect on the next tick`.
- Tracy: `ScriptHost::run_scripts` opens a `ZoneTransientN` named after the script (compiled out without `TRACY_ENABLE`).
- Crash reports: `crash::add_context(fn, user)` (`mce/crash.h`) lets a subsystem add text to the report's
  `context:` section; the host registers one that, when a script's `update()` is on the stack, writes the script
  name and up to 12 Luau frames (`lua_getinfo` only reads). Test: `test_script_crash.cpp` aborts a child process
  spinning in a script and reads the report.
- Editor: `find_script_location` (`console_model.h`) reads `path.luau:line` out of a log or cook line. The Console
  draws such lines as selectable rows: double-click opens the script in the system's default application
  (`SDL_OpenURL`) and copies `scripts/x.luau:3`; the Cook panel's context menu has "Open script". The editor
  has no text editor of its own, so there is no jump to the line itself. The UI part is not covered by a test
  beyond the parser (`test_console_model.cpp`).

