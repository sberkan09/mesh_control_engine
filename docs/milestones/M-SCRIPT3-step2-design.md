# M-SCRIPT3 step 2: hidden-state checks at run time (design note)

**Status:** proposal for review (docs only). Step 2 of [M-SCRIPT3](M-SCRIPT3.md). Model: Opus (touches `ScriptHost`).

**The rule:** scripts hold no state between ticks; everything lives in reflected components so `World::state_hash`,
saves and replays cover it (`script.h:11-14`, user guide 27.4).

## What the code does today (verified)

- The compiler check (`compiler/state_check.cpp`) flags a write only when its root expression is a local with
  `functionDepth == 0` (`state_check.cpp:118-119`); writes at the top level are free (`:105`); mutating calls are
  checked by their first and fifth argument only (`:72-74`). Roots that are globals (`AstExprGlobal`) are never flagged.
- Run time: `luaL_sandbox` makes the shared globals and libraries read-only (`script.cpp:595`); each script gets a
  thread whose global table proxies them (`:666-667`, Luau `linit.cpp:113-128`), frozen after the top level (`:696`).
  `lua_setreadonly` is shallow: the *contents* of tables held in globals or upvalues stay writable.
- `update()` is held by `lua_ref` and called on the main thread `L_` (`:693`, `:733`); the host writes `mce.dt`
  each tick by toggling `mce` read-only (`:720-725`). `newproxy` is not removed (`:560`), so a script can own a
  userdata with a writable metatable.
- Bytecode is built with `debugLevel = 1` (`compiler/compiler.cpp:88`): Luau then stores no upvalue names
  (`Compiler.cpp:578` needs level 2), so `lua_getupvalue` returns `""` (`lapi.cpp:1754`).
- Every hot reload or scene change rebuilds the whole `ScriptHost` (`scene_scripts.cpp:61-62, 71, 96-108`); the
  "fresh VM" test does the same (`test_script_state.cpp:147-182`). A baseline taken in `add_bytecode` is always fresh.
- `mce.random` keeps its counter in C++ (`random_calls_`, reset per update at `:732`); field handles are tagged light
  userdata indexing `handles_` (`:383-398`). Neither is VM state the check could see or confuse.
- `mce_player` does not report a `Failed` script in its exit code (only `headless_run.cpp:229` and
  `samples/script_headless/main.cpp:125` look); the determinism stage (`local_ci.sh:530-552`) compares hashes, so a
  script disabled the same way in Debug and Release passes it today.

**Gaps the static check misses**, all confirmed by reading `check_root`:
(a) alias, `local a = t; a.n += 1` in `update` (root `a` has depth 1); (b) a helper that writes its parameter,
`fill(t)` (the test at `test_script_state.cpp:103` documents it); (c) a closure factory, `local inc = counter()`
whose upvalue `n` has depth 1; (d) `for k, v in t` over object keys (documented, guide line 183); and two more:
(e) a global table, `STATE = {n = 0}` then `STATE.n += 1` (global root, and the frozen env is shallow);
(f) `getmetatable(newproxy(true))` written from `update`.

## Options

| | Catches | Per-tick cost | Error says | Risk |
|---|---|---|---|---|
| A. Deep fingerprint probe (author's proposal): hash everything reachable from `update` after load, compare after every `update` | a b c e f (not d) | O(reachable graph) per script per tick, so it must be off by default | the root (`global 'STATE'`), no line | traversal + hash code, false positives from hash design |
| B. Recursive freeze after load (`lua_setreadonly` on every reachable table and userdata metatable) | a b e f at the write | none | Luau's `s:5: attempt to modify a readonly table` (line-precise) | misses c (upvalue reassignment) and buffers |
| C. Fresh VM state each tick | everything, by construction | top-level re-run per tick (handles, constant tables) | nothing: state is silently lost | hides bugs instead of naming them; slow |
| D. Static only, with alias tracking | a, e cheaply; b needs interprocedural flow; c needs escape analysis | none | `file:line:col` in the cook | never complete in a dynamic language |
| **E. Hybrid: B + a watch list** | a b c e f | O(watched upvalues + watched buffer bytes) | line for table writes; root + closure line for upvalues | small, see below |

**E in detail.** After the top level ran (end of `add_bytecode`, `script.cpp:693-697`), one walk from `update` and the
script's global table: tables (`lua_rawiter`), Lua closures (`lua_getupvalue` 1..n, `lua_getfenv`), metatables
(`lua_getmetatable`, raw, no metamethods run). Every table reached is frozen; every Lua closure's upvalue slots and
every buffer reached go on a watch list with their baseline (values kept alive with `lua_ref`, so no address reuse;
buffers as a byte copy). After each `update()` (`:735-743`) the host compares the watch list: upvalue by
`lua_rawequal` (numbers by bit pattern, so a NaN does not read as a change), buffer by `memcmp`. Once tables are frozen, the reachable graph can only change through an upvalue or
a buffer, so this is complete for everything except iteration order (d), which no state check can see.
A is then only a slower way to catch the same set; C is rejected; D is a cheap cook-time improvement (2c).

## Decisions proposed

- **API (Luau 0.742, `Dependencies.cmake:489`, all present in `VM/include/lua.h`):** `lua_rawiter` (364),
  `lua_getupvalue` (559), `lua_getfenv` / `lua_getmetatable` (240-241), `lua_setreadonly` / `lua_getreadonly`
  (236-237), `lua_topointer` (193, visited set key), `lua_tobuffer` (192), `lua_rawequal` (170), `lua_ref`,
  `lua_cpcall` (261). `lua_clonefunction` is not needed. The walk runs inside `lua_cpcall` so a stack-growth
  "not enough memory" (`lua_checkstack`) becomes a load error instead of a longjmp out of the host.
- **What is skipped:** the shared globals table and the library tables in it, pre-seeded into the visited set (they
  are already read-only, and `mce` must stay writable by the host for `mce.dt`); C functions (opaque); strings,
  numbers, booleans, vectors, light userdata (immutable values).
- **Determinism:** the walk is a set operation (freeze is idempotent, the watch list is compared slot by slot), so
  its order does not change any outcome. Only the error message picks "the first changed slot"; slots are ordered
  by root (update's upvalues by index, then globals by name, Luau's string hash has no seed) and walk order under it.
  Nothing enters `state_hash`.
- **Bounds:** the walk is O(reachable objects) once per load and is bounded by the VM memory cap (32 MiB,
  `script.h:54`); the visited set lives in host memory, not the VM's. Per tick: one `lua_rawequal` per watched slot.
  Buffers: total watched bytes capped (proposal 256 KiB); above it the load fails with a named error, since no
  current script creates a buffer (grep of `projects/*/assets/scripts`).
- **Legitimately mutable things:** RNG and field handles live in C++ (allowed, nothing to do); `mce.dt` lives in the
  skipped `mce` table. **Lazily filled caches and top-level scratch tables are flagged**, the same as the static check
  already does for `cache[k] = v` and `table.clear(t)`: build them at the top level, or per call.
- **Reload:** each `ScriptHost` walks its own scripts at `add_bytecode`; hot reload, scene reload and the fresh-VM swap
  all build a new host, so baselines never outlive a VM. No interaction with the step budget (no Lua code runs).
- **Switch:** `ScriptBudget::check_state` (default `true`) and a convar `script.state_check` (bool, default 1),
  declared in `scene_scripts.cpp` (not a header) and copied into the budget in `install_scene_scripts` on the main
  thread (convars are main-thread only, `convar.h:3`). Same default in Debug and Release: a check that fails a script
  in one build only would make the Debug=Release comparison of `stage_script_determinism` diverge. With E's cost
  there is no reason to make it Debug-only, which also answers the assessment's "Debug assert after every tick" (section 4)
  with something stronger: the same check in every build.
- **Messages** (through `fail()`, `script.cpp:653`, which keeps `<name>:<line>:`):
  - table write: `arena:57: attempt to modify a readonly table (tables reachable from update() are frozen after the
    top level; keep state in a component)`: `fail()` appends the hint when the Luau message ends in `readonly table`.
  - upvalue: `counter:3: update() kept state between ticks: upvalue 'n' of the function defined at line 3 changed
    (reached from upvalue 'inc' of update()); keep it in a component`. Names need `debugLevel = 2` (open question 1);
    at level 1 it says `upvalue #1` and `upvalue #2`.
  - buffer: `s:1: update() kept state between ticks: a buffer reached from global 'B' changed; keep it in a component`.

## Tests (`tests/script/test_script_state.cpp`, all in-process, under 1 s)

Each of the first five asserts **both** that `script::compile` accepts the source (the static check misses it) and
that after `w.tick` the script is `Failed` with the expected text: (a) alias, (b) helper parameter, (c) closure
factory, after tick 1 not at load, (e) global table, (f) `newproxy` metatable; plus a buffer write.
Then: what stays allowed (locals and closures inside `update`, constant top-level tables read, `mce.random`, field
handles, `mce.dt` changing) runs 100 ticks `Running`; `check_state = false` leaves case (a) `Running` (the switch
works); the reload test (`:147`) passes unchanged with the check on; the shared `mce` table is not frozen by a walk.
The existing `test_script` suite, EmberScript, Dungeon and Pursuit must stay green with the check on (they run in
`stage_script_determinism`).

## Steps

| # | Step | Size | Review |
|---|---|---|---|
| 2a | Walk, freeze, watch list, compare, messages in `script.cpp`; `check_state` in `ScriptBudget` and private arrays in `script.h` (6 includers, no seam PR needed); the tests above | ~200 lines + ~150 test | Opus/coordinator: the walk under `lua_cpcall`, skipping the shared globals, `lua_ref` lifetime of baselines (unref in `~ScriptHost` is implicit via `lua_close`), no Lua code run from the walk |
| 2b | Convar in `scene_scripts.cpp`; `mce_player -headless` exits non-zero when a script is `Failed` (player module, own PR) so the determinism stage notices; user guide 27.4, ADR-0041 amendment, M-SCRIPT3 record, `changelog.d/` | ~40 lines + docs | Sonnet; player owner for the exit code |
| 2c | Optional: static check follows `local a = <top-level local>` aliases and flags writes rooted at a global table, so the common cases fail in the cook at `file:line:col` | ~60 lines + tests | Sonnet |

**Risk.** Medium-low. The freeze changes what a running script may do, but only in ways the rule already forbids;
the four shipped scripts build no table that `update` writes (only constants like `NEXT`, `dungeon.luau:25`), to be
confirmed by 2a's run of the determinism stage. The new code is host-side and runs no Lua, so it cannot spend steps
or reenter the VM. The one real cost to users: no top-level scratch tables (one allocation per tick instead).

## Open questions for the coordinator

1. Compile with `debugLevel = 2` so errors name upvalues (`'inc'`, `'n'`)? It grows bytecode with local names and
   needs the `script` importer version bumped (cooked scripts change). Recommended: yes.
2. Default on in shipped games too (recommended, it costs a comparison per upvalue), or `script.state_check 0` in
   release players with a non-zero exit only in dev hosts?
3. Gap (d) stays a documented gap; acceptable for M-SCRIPT3, or should 2c reject `for k, v in t` over any table the
   walk found with object keys at load?
4. Is the player exit-code change (2b) in scope here, or does the determinism stage read the log for `script failed:`?

**Recommendation:** option E (freeze + upvalue/buffer watch list), on by default in every build, as 2a then 2b;
2c only if the blind test (step 4) shows agents hitting the run-time errors often. The deep fingerprint probe (A) is
not needed once tables are frozen: it would catch the same cases at a per-tick cost and without a line number.
