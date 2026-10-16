# M-SCRIPT — Luau scripting for games without a compiler (milestone record)

**Status:** active (added 2026-10-16; step 1 done, step 2 next). Plan: ROADMAP §M-SCRIPT; design of record
ADR-0041; Linear SBA-147 … SBA-152.

**Why:** today every game needs a C++ toolchain and, on phones and the web, its own build. A script game is
data that one stock player runs. The spike (ADR-0041, PR #262) showed a sandboxed Luau system keeps
FR-RUN-040 at about 29× the C++ cost.

**Not in scope:** visual scripting, code mods (N10 stays a separate decision), Luau's JIT.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Scripts as assets: `.luau` → `CHNK_SCRIPT`, `ScriptSystem` in scenes, the stock hosts run bytecode only | SBA-147 | done |
| 2 | A complete API: field handles, `Commands`, events, input, resources, physics queries; bench ≤ 0.4 ms | SBA-148 | not started |
| 3 | No hidden state: an AST check for outer-local writes and `pairs` over object keys | SBA-149 | not started |
| 4 | Tooling: `mce.d.luau`, type checking, MCP `script_check`, editor errors, hot reload, Tracy, crash stacks | SBA-150 | not started |
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
