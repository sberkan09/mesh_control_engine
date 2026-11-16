# M-SCRIPTPROF — Seeing what scripts do: profile, budget errors, log source, traces (milestone record)

**Status:** done 2026-10-18 (steps 1-4: PRs #603, #605, #606, #612) (added 2026-10-18; owner: the M-SCRIPT thread; device-free; no Linear issues: free limit, this record is the tracker).

**Why.** A script that is slow, runs out of its step budget or prints into a crowded log is hard to diagnose today: `ScriptInfo.last_steps` is the only number, the budget
error says "step budget exceeded (10000 per tick)" with no line, `mce.log` lines do not say which script wrote them, and `mce inspect` shows values after a run but
not how they moved. A game author (or an agent) writing Luau needs the same tools a C++ author has: a profile, a located error, a traceable log and a time series.

**Not in scope:** an interactive debugger (breakpoints, stepping: it needs VM hooks and a device UI), a profiler for the engine's own systems (M-PERF covers it).

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | Per-script step profile: `ScriptInfo` keeps peak, total and ticks run; `run_headless` returns it as `scripts`; `mce profile <project> [--ticks N]` prints a table with the budget share | Sonnet (Opus review: touches the update loop in `script.cpp`) | done in PR #603 |
| 2 | A step-budget failure names the script line it stopped at (`file:line`), like any other script error (memory failures cannot: the allocator has no Lua frame) | Sonnet (Opus review: VM interrupt) | done in PR #605 |
| 3 | `mce.log` lines carry the script name (`[<name>] script: ...`) so a console filter and `run_headless`'s log tail show the source | Sonnet | done in PR #606 |
| 4 | `mce inspect --trace`: sample entity fields every N ticks through `run_headless`'s `trace`, printed as series | Sonnet | done in PR #612 |

Each step ends with a "Step N as built" section here, guide 27 / `docs/engine/script.md` updated, and a `changelog.d/SCRIPTPROF-<n>-<slug>.md` fragment.

## Step 1 as built (script profile)

- `ScriptInfo` gets `peak_steps`, `total_steps` and `updates`, filled where `last_steps` was (`ScriptHost::run_scripts`, three lines); `ScriptHost::budget()` exposes the limit.
  `HeadlessRun::scripts` carries them (`ScriptProfile`), `run_headless` returns `scripts` (script, failed, updates, avg_steps, peak_steps, budget_steps), and `mce profile
  <project> [--ticks N]` prints the table (exit 3 on a failed script). Steps are the VM's deterministic interrupt count, not time.
- Tests: "the step profile counts updates, peak and total steps" in `test_script.cpp`; `cli_test` covers the table and the exit code (fake server).
- Not done: wall-clock time per script (non-deterministic; the Tracy zone `MCE_SCRIPT_ZONE` already exists for that); `mce profile` and `mce inspect` were run by hand against the real `mce_mcp` on a copy of `projects/Pursuit` (Pursuit's `arena.luau`: 59 steps on average, peak 62 of 200000).
- Also fixed here: `run_headless` wants a scene path under assets/, so `mce profile` and `mce inspect` (M-SCRIPT3 step 12, which had only been checked against a fake server) now default to the scene the project's `start_scene` guid names, found by scanning `assets/**/*.mcscene`; without it the real tool said "the scene was never saved".

## Step 2 as built (located budget errors)

- `ScriptHost::interrupt_fn` builds the error from `lua_getinfo(L, 0, "sl")`: `spin:1: step budget exceeded (500 per tick)`, the line inside the function that spent the budget (a
  nested call reports the inner loop, not its caller). It uses `lua_pushfstring` + `lua_error` because `luaL_error` would add the caller's position as well ("deep:5: deep:2: ...").
  Without line info the old text is used. The profile's over-budget detection still matches "step budget exceeded".
- Tests: in "the step profile counts updates, peak and total steps" (a one-line loop and a loop in a called function).
- Not done: "not enough memory" stays unlocated (the allocator callback has no Lua frame to ask).

## Step 3 as built (log source)

- `mce.log(msg)` writes `[<name>] script: <msg>` (it was `script: <msg>`), the name of the running script first (`?` if none is running). The name leads and the old
  `script: <msg>` text stays intact on purpose: the SDK, weather, missions and day/night sample tests (other threads') match on it, and changing it would break them. Test: "mce.log lines name the script that wrote them" reads the console ring with two scripts.
- Not done: a log level argument (`mce.log` stays info) and a per-script mute.

## Step 4 as built (inspect --trace)

- `mce inspect <project> --trace <entity>:<Component.path>` (repeatable, at most 16; `--every N`, default 10) passes `run_headless`'s existing `trace` (`{every, fields}`) and prints
  the series as a table, one row per sampled tick and one column per field (vectors as `(x, y, z)`). `--trace` alone needs no `--component`/`--entity` and prints no entity dump.
  No engine change: the tool already sampled and returned `trace`; the CLI only exposed it.
- Test: `test_cli_test.py` (fake server returns a two-row trace; checks the table, that no entity dump is printed, and the `--trace Hero` error). Not run by hand against the real `mce_mcp`.
- Not done: plotting, CSV export, or tracing a field of an entity the run spawns (the tool takes scene entities only).
