# core — L0 foundation: memory, containers, math, errors, logging, config and the main loop

`mce_core` is layer L0 (`src/engine/core/CMakeLists.txt`). It includes nothing above itself and links only
`spdlog::spdlog` and `Tracy::TracyClient` publicly, plus `${CMAKE_DL_LIBS}` privately on non-Windows for the
crash handler. It also reads the configure-time engine stamp from `MCE_GENERATED_INCLUDE_DIR` for
`mce/version.h`. Every engine library links it, directly (`mce_refl`, `mce_ecs`, `mce_asset`, `mce_platform`,
`mce_rhi` and others) or through them, and it is linked whole into the `mce_engine` umbrella shared library
(`src/module/CMakeLists.txt`). Headers live in `src/engine/core/include/mce/`; `mce/core.h` is the umbrella.

## Public surface

Fundamentals
- `mce/types.h`: fixed-width aliases (`u32`, `f32`, `usize`), `Span`, `StringView`, `KiB`/`MiB`,
  `MCE_FRAMES_IN_FLIGHT = 3`, `align_up`, `next_pow2`, `MCE_FORCEINLINE`.
- `mce/hash.h`: `fnv1a64`, `crc32` (IEEE, used by `.mce` containers), `mix64`, `hash_combine`, the streaming
  `Hasher64` used by `World::state_hash`, and the `Hash<K>` trait for `HashMap`.
- `mce/string_id.h`: `StringId`, a 64-bit FNV-1a name hash for convars, events and asset aliases;
  `MCE_SID("name")` forces compile-time evaluation.
- `mce/type_name.h`: `type_name<T>()` and `kTypeHash<T>`, RTTI-free type identity from the compiler's pretty
  function name. This is the ECS component id.
- `mce/fixed_string.h`: `FixedString<N>`, inline, NUL-terminated, truncates silently and reports it through
  `truncated()`.
- `mce/version.h`: `engine_describe()`, the `git describe` + build type + compiler + sanitizer stamp.

Memory
- `mce/memory/allocator.h`: `IAllocator`; `heap()` (the only path to malloc, thread-safe, tracks live
  allocations); `make`/`destroy`/`UniquePtr`/`make_unique` over any allocator; `LinearArena` (bump, markers,
  `rewind`) and `ArenaScope`; `NoHeapScope`, which makes any `heap()` traffic on its thread assert and bump
  `heap_rt_violations()`.
- `mce/memory/frame_slots.h`: `FrameSlots` (three arenas, 16 MiB default each), `FrameSlotFence`, and the
  thread's `frame()` / `frame_arena()` allocator.
- `mce/memory/pool.h`: `Pool<T>`, fixed-capacity pool with an intrusive free list.
- `mce/memory/handle.h`: `Handle<Tag>` and `HandleTable<T>`, generational handles; a stale handle fails and
  never aliases.

Containers (`mce/containers/`)
- `array.h`: `Array<T>` (allocator-aware, 1.5x growth) and `SmallVector<T, N>`.
- `hash_map.h`: `HashMap<K, V>`, Robin Hood open addressing.
- `flat_map.h`: `FlatMap<K, V>`, sorted array with key-ordered iteration; the map for anything sim-visible.
- `bitset.h`: `BitSet<N>`.
- `ring_buffer.h`: `RingBuffer<T>`, lock-free SPSC FIFO for trivially copyable payloads (audio commands).
- `mpsc_queue.h`: `MpScQueue<T>`, bounded multi-producer / single-consumer queue with the main thread as
  consumer.

Math
- `mce/math/math.h`: `vec2/3/4`, `quat` (Hamilton, `{x, y, z, w}`), `mat4` (column-major, `c[column][row]`),
  `aabb2`/`aabb3`, `trs`, `inverse`, `look_at_rh`, `perspective_rh_no`, `perspective_rh_zo`, `ortho_rh_no`,
  `slerp`. Conventions: right-handed, Y-up, meters, radians, column vectors.

Errors, logging and asserts
- `mce/result.h`: `Errc` (ranges per subsystem: 0x0200 assets, 0x0300 rhi, 0x0700 module loader), `Error`
  (code + static stage string + path + detail), `Result<T>`, `ResultV`, `ok()`, `MCE_TRY`, `MCE_TRY_ASSIGN`,
  `format_error`.
- `mce/assert.h`: `MCE_ASSERT`, `MCE_ASSERT_MSG`, `MCE_VERIFY`, `MCE_UNREACHABLE`; `set_assert_handler` (tests
  install one that fails the current test instead of trapping).
- `mce/log.h`: `MCE_LOG_*` macros over spdlog; `log::init`/`set_level`/`flush`; the console ring
  (`read_console`, `console_last_seq`, 1024 lines) the editor drains; the 64-entry breadcrumb ring
  (`visit_breadcrumbs`, `try_visit_breadcrumbs`).
- `mce/crash.h`: `crash::install(CrashDesc)` hooks fatal signals (POSIX: SEGV, BUS, FPE, ILL, ABRT on an
  alternate stack) or the unhandled-exception filter (Windows) and writes a plain-text `.crash` report
  (header, signal, pc, module+offset backtrace, loaded modules with build ids, breadcrumbs); Windows adds a
  `.dmp`. `default_dump_dir` is per user; `trigger_test_crash()` backs `-crash_test 1`.

Configuration
- `mce/convar.h`: `ConVar` (bool/int/float/string, flags `Cheat`, `Saved`, `ReadOnly`, `RequiresRestart`),
  `ConFunc`, `MCE_CVAR`, `MCE_CONFUNC`; `console::execute`, `console::parse_command_line` (`-name value`,
  `+func args`; `-help` / `--help` / `-h` sets the `help` convar, `console::help_requested()`, and
  `console::print_help` writes the reference every app prints before exiting 0), and the cheat lock
  `console::set_cheats_allowed` / `cheats_allowed`. Core itself registers `time_scale` (Cheat),
  `sim.max_catchup`, `headless`, `r.vsync` and `help`.

Runtime shell
- `mce/app.h`: `app::run(Application&, AppDesc, RunStats*)`, the only main loop in the engine: a fixed-step
  loop with `TickContext` (dt is exactly `1/sim_hz`) and `RenderContext` (interpolation alpha). Seams:
  `IClock`, `IPlatformHooks`, `IFrameFenceSource`, `IInputSource`. With no platform hooks it runs headless.
- `mce/state_stack.h`: `StateStack<StateT>`, push/pop/replace with transitions deferred to `apply()` at the
  frame boundary.
- `mce/jobs.h`: `jobs::init`, `run`, `wait_for`, `Counter`, a counter/fork-join API.

## Rules and invariants

- Dependency floor: `mce_core` links only spdlog and Tracy, ever. A new dependency needs an ADR first
  (`core.md` §1.3, ARCHITECTURE §7).
- Engine APIs do not allocate behind the caller's back. Anything that needs memory takes an `IAllocator&`;
  `heap()` is the only route to malloc; per-frame scratch comes from `mem::frame()`, which is reset when its
  slot comes round again.
- Frame-slot slot `N mod 3` is reused only after the fence registered for frame `N-3` signals (ADR-0005).
  Headless runs register no fence, so slots are always reusable.
- Engine container ban (`scripts/ci/check_walls.py`): no `std::vector`, `std::unordered_map`,
  `std::unordered_set`, `std::string`, `std::list`, `std::map` or `<iostream>/<sstream>/<fstream>` anywhere
  under `src/engine/`. Use `Array`, `HashMap`, `FlatMap`, `FixedString`.
- DirectXMath wall: DirectXMath appears only in `tests/core/test_math_oracle*`.
- Determinism: `HashMap` iteration order must never drive sim-visible ordering; use `FlatMap`.
  `Hasher64::f32v` hashes bit patterns, so `-0.0f` and `+0.0f` differ.
- Expected failures return `Result`; programmer errors assert. No exceptions (`SPDLOG_NO_EXCEPTIONS` is a
  public definition).
- No `MCE_LOG_*` inside per-entity or per-draw loops; log a per-frame summary.
- Convars and confuncs register through static objects into an intrusive list (no heap) and are main-thread
  only. While cheats are locked, `console::execute` and the command line refuse `Cheat` convars, and locking
  resets them to defaults. Code can still set them.
- `on_tick` must not read wall time. `time_scale` scales accumulator inflow, never dt; catch-up is clamped at
  `sim.max_catchup` ticks per frame.
- No function-local statics in engine header templates (finding F11): type identity is the value
  `kTypeHash<T>`, and registries map it to dense ids in non-template code, so identity holds across the
  engine/game-module boundary.
- The crash handler only uses what `install()` prepared: paths and header text are formatted up front, the
  report is written with raw file calls, and the breadcrumb ring is read with a try-lock.
- Jobs currently run inline on the caller (`src/jobs.cpp`): `run` asserts its dependency is already complete,
  and `wait_for` asserts the counter is zero. Call sites are written once; parallelism is meant to be a swap.

## How it is tested

ctest `test_core` (`tests/core/CMakeLists.txt`) links only `mce_core`:
- `test_app.cpp`: "headless run ticks exactly max_ticks with a constant dt (FR-RUN-025)", "tick grid is
  independent of render rate (FR-RUN-020)", "spiral-of-death clamp limits catch-up ticks per frame",
  "time_scale scales accumulator inflow, never dt (FR-RUN-050)", "input source gets exact rational tick window
  ends (FR-INP-030)", "failing on_init returns a nonzero exit code".
- `test_containers.cpp`: `Array` growth and aliasing, `SmallVector` spill, `HashMap` with `StringId` keys and
  deterministic iteration, `FlatMap` ordering, `RingBuffer` SPSC, `MpScQueue` with several producers,
  "HandleTable: stale handles fail, never alias", `FixedString` truncation.
- `test_convar.cpp`: registration and lookup, typed get/set and parsing, read-only rejection, change
  callbacks, `console execute`, command line parsing, "cheat lock (shipped builds)".
- `test_crash.cpp`: "crash: default dump dir is per user", "crash: a fatal signal writes a report and still
  dies of it" (a forked child dies of SIGSEGV).
- `test_hash.cpp`, `test_math.cpp`, `test_memory.cpp` ("FrameSlots reuse a slot only after its fence signals",
  "NoHeapScope flags heap traffic on the guarded thread only"), `test_result.cpp` (`MCE_TRY` propagation,
  "format_error is diagnosable"), `test_state_stack.cpp` ("StateStack defers transitions to apply()").

ctest `test_math_oracle` (`test_math_oracle.cpp`) checks the math library against DirectXMath: layout bridge,
rotation sense, quaternions, slerp, inverse, `look_at_rh`, both perspective variants, `ortho_rh_no` and
`aabb3`. It is a separate executable so DirectXMath reaches no other target, and it builds with
`-fno-strict-aliasing` because of DirectXMath's type punning.

ctest `player_crash_report` (`src/player/CMakeLists.txt`, Unix, non-ASan) runs `mce_player -crash_test 1` and
checks with `tests/crash_report_check.py` that `scripts/symbolicate.py` resolves the report.

## Design records

- [../architecture/core.md](../architecture/core.md): responsibilities and the layer table (§1), API sketches
  for the app loop, state stack, allocators, `Result`, logging/convars, jobs and math (§2), frame memory map
  and containers (§3), the thread roster and frame lifecycle (§4), test strategy (§6).
- [../ARCHITECTURE.md](../ARCHITECTURE.md) §1.2 (walls), §6 (threading and memory model), §7 (dependency
  rule), §9 (ADR-0005 frame slots, ADR-0007 determinism, ADR-0009 reflection).
- `docs/adr/0017-fetchcontent-dependencies.md` (how spdlog and Tracy
  are fetched) and `docs/adr/0018-portable-build.md` (Linux presets beside MSVC).
- `docs/adr/0021-game-module-loader-m3.md`: one engine instance (one
  convar registry, one log, one allocator state) shared by host and game module.
- `docs/milestones/M8-packaging.md`: the crash handler, symbolication, version
  metadata and the cheat guard.

## Known gaps

- Jobs: the worker pool is not implemented; `jobs::run` executes inline and `worker_count()` only stores the
  value passed to `init` (`src/jobs.cpp`, `mce/jobs.h`).
- Crash reports (from `M8-packaging.md`): the Windows path (minidump + text report) compiles only on Windows
  and has not been run; `abort()` on Windows does not pass through the exception filter, so an assert in a
  Windows release build leaves no report; on POSIX only the crashing thread is unwound; there is no upload, so
  reporting is manual.
- Under ASan the crash hooks stay off and ASan's own report wins, so `player_crash_report` is not registered
  in ASan builds.
