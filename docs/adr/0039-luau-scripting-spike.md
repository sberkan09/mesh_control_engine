# ADR-0039: Luau scripting spike (`mce_script`)

**Status:** Proposed (2026-10-15), spike built. The user chose "spike first" on 2026-10-15; whether a M-SCRIPT
milestone follows is the user's decision after reading the results below. **Requirements:** touches N2 (no
scripting VM in v1), N10 (code mods), FR-ENT-130 (no coroutines), FR-RUN-040 (determinism). N2 stays in force:
nothing outside the spike's own test and sample links `mce_script`.
**Reference:** the assessment in the project's shared files (`assessments/script-dili-degerlendirmesi.md`).

**Context.** N2's re-entry trigger is "after Ashen Court, if measured recompile iterations dominate editor time".
Measured on 2026-10-15 it does not hold: in the M-AGENT demo sessions `module_build` took at most 6 % of a
session, and the M5b stopwatch puts a human's save-rebuild-reload at 2.7 s. A scripting language would instead
buy games without a C++ toolchain (one stock player runs every game: SDK without a compiler, web, phones
without a per-game build), sandboxed code mods (N10) and in-session hot reload. Those gains are only worth
having if a VM keeps FR-RUN-040, saves and replays intact. The spike tests exactly that, with the smallest
embedding that can answer it.

## Decisions

1. **Luau, interpreter only.** Luau (MIT, `luau-lang/luau` tag `0.742`) over Lua 5.5, Wren, AngelScript and
   WebAssembly: a built-in sandbox (read-only globals and libraries, `luaL_sandbox`), an unseeded string hash, a
   type checker that can give agents compile-style diagnostics, and an interpreter fast enough for gameplay rules.
   Codegen (Luau's JIT) is never linked: it picks its own instructions, and the determinism contract is about
   one binary doing the same float operations in the same order.
2. **Fetched like every dependency** (ADR-0017), `MCE_WITH_SCRIPT` (ON on desktop, OFF in the mobile presets).
   Only `Luau.VM`, `Luau.Compiler` and their parts build (`EXCLUDE_FROM_ALL`). They compile with the engine's
   precise floating point (`-ffp-contract=off`, `/fp:precise`; the compiler folds constants in double) and
   `LUA_USE_LONGJMP=1`, so VM errors unwind by longjmp and the `-fno-exceptions` engine can call the VM. The
   compiler still uses exceptions internally (`luau_compile` catches them); in a real milestone it moves to the
   cook and the player loads bytecode only.
3. **`mce_script` is opt-in** like `mce_net`: not in `MCE_ENGINE_LIBS`; walls in `scripts/ci/check_walls.py`
   keep Luau's headers in `src/engine/script/src/`, Luau's targets linked by `mce_script` alone, and
   `mce_script` linked only by `tests/script` and `samples/script_headless`.
4. **Scripts are systems over reflected components.** A `ScriptHost` owns one VM; each script runs its top level
   once in its own environment (`luaL_sandboxthread`) and must define `update()`. `install(World&, Phase)`
   registers one `script.update` system that calls every script in the order they were added. The API is the
   `mce` table: `each("A", "B", fn)` in table-then-row order (the order a Query visits), `get` / `set` of any
   scalar, enum, `Vec2`/`Vec3` (as Luau vectors) or 4-float field by name through `mce_refl` (a write calls
   `World::touch`, so change detection sees it), `dt`, `tick()`, `random()` and `log()`.
5. **No state in the VM between ticks.** Everything a script remembers lives in components, so `state_hash`,
   saves and replays cover it with no new format, and a script can be replaced between two ticks. The globals
   are frozen after the top level (`lua_setreadonly`); `mce.random()` is a hash of (seed, tick, script, call
   index), so it carries no hidden generator state.
6. **Sandbox.** Only the base, table, string, math, utf8, bit32, buffer and vector libraries are opened; `os`,
   `io`, `debug`, `coroutine`, `print`, `collectgarbage`, `gcinfo`, `getfenv`, `setfenv`, `math.random` and
   `math.randomseed` are absent. Each script gets a step budget per tick, counted in Luau's interrupt callback
   (it fires at calls, returns and loop back edges, so the count is deterministic); the whole VM gets a memory
   cap through its allocator. A script that errors, exceeds a budget or writes a read-only table is disabled,
   its error kept as `name:line: message`, and the other scripts keep running.

## Results (2026-10-15)

| Question | Answer | Evidence |
|---|---|---|
| Same hash across runs, Debug = Release? | Yes | `local_ci.sh` determinism stage, `stage_script_determinism` (3 runs × 2 builds) |
| Does the VM add float drift? | No: the Luau system and its C++ twin give the same `state_hash` | `sample_script_headless -mode lua` vs `-mode cpp`, same stage |
| Sandbox | `os`/`io`/clock/unseeded random absent; writes to globals, libraries and `mce` fail | `test_script` "sandbox", "read-only" |
| Runaway and allocating scripts | Disabled by the step budget / memory cap; other scripts unaffected | `test_script` "step budget", "memory cap" |
| Error quality | `name:line: message` for compile errors, runtime errors, unknown components and fields | `test_script` "errors" |
| Cost | 2,000 entities: Release 0.82 ms/tick (Luau) vs 0.028 ms (C++), about 29×; Debug 3.31 vs 0.058 ms | `sample_script_headless` |

Release timing (linux-gcc-release, 1,200 ticks): 0.82 ms/tick (Luau) vs 0.028 ms (C++), about 29×, with the VM at
392 KiB. A 60 Hz tick has 16.7 ms, so rules over a few thousand entities fit; hot loops (particles, physics)
stay in C++. Most of the cost is the per-call name lookup in `get`/`set` (open question 5).

## Open questions (for a M-SCRIPT milestone, if the user wants one)

1. **Bytecode at cook.** The player should load bytecode compiled by `mce_cook` (the "runtime parses no text"
   rule); code mods would need the compiler in the player or a verified bytecode loader. Luau does not
   promise that loading untrusted bytecode is safe, so mods would ship source and compile on load.
2. **State in upvalues.** `local count = 0` at a script's top level, changed by `update()`, survives between
   ticks and is invisible to `state_hash`. Determinism from the start of a run holds (the spike's hashes do not
   change), but a save or a reload loses it. Options: reject assignments to outer locals with a check over
   Luau's AST at compile time, or freeze top-level tables recursively after the top level runs.
3. **Iteration order over object keys.** `pairs` over a table keyed by tables or functions follows addresses,
   which differ between runs. Scripts that only walk entities through `mce.each` and arrays are safe; the
   checker in question 2 could also flag `pairs` over non-string keys.
4. **Structural changes and events.** The spike has no `spawn`, `destroy`, `add`, `remove` or events; they go
   through `Commands` and `EventQueue` by reflection the same way fields do.
5. **Per-call cost.** Every `get`/`set` resolves the component and field by name. Caching handles (for example
   `local pos = mce.field("Ball", "position")`) is the first optimization to measure.
6. **Tooling.** Type definitions for the `mce` API and the reflected components, `luau-analyze` behind an MCP
   `script_check` tool, script stacks in Tracy zones and crash reports, hot reload in play-in-editor.
