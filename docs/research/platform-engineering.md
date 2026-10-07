# Research: Platform Engineering for MCE

**Domain:** build systems, package management, memory, concurrency, SIMD, profiling, logging/asserts/crash handling, testing.
**Stack context (binding):** C++20 / MSVC v143, `/std:c++20 /W4 /permissive-`, warnings-as-errors, exceptions+RTTI off in runtime core, `Result<T,E>` for fallible paths; CMake 3.28+ presets + vcpkg manifest; OpenGL 4.5 Core behind a thin RHI; SDL3; solo dev, Windows 10 x64, 2–3 year horizon.
**Method:** survey → tradeoffs → committed decision. Everything below is grounded in canonical sources: *Game Engine Architecture* (Gregory), Gaffer On Games, Bitsquid/Our Machinery blogs, Box2D/Erin Catto GDC slides, Tracy docs, MSVC/ASan docs, Agner Fog's optimization manuals.

---

## 1. Build system & package management

### 1.1 State of the art in C++ land, 2026

C++ has no dominant build system; this is the single biggest platform-engineering tax on the language. Surveyed options:

| Option | Strengths | Weaknesses for MCE |
|---|---|---|
| **CMake 3.28+ with `CMakePresets.json`** | Universal ecosystem; every vcpkg port ships CMake config; IDE-native (VS opens CMake folders); presets fix the UX complaint that made CMake hated; `Ninja` generator is fast | Ceremony-heavy; generator expressions are cryptic; easy to write non-portable scripts |
| Meson | Clean syntax, fast, sane defaults | vcpkg integration is second-class; smaller ecosystem; one more thing nobody can help you with |
| xmake | Genuinely nice DSL, built-in package manager | Small ecosystem; `xmake.lua` is unreadable to everyone else; solo bus-factor |
| Bazel / Buck2 | Massive-repo scale | Massive-repo scale. Absurd overhead for one developer |
| MSBuild projects | VS-native | Not scriptable, not diffable; binding forbids them |

Package management: **vcpkg manifest mode** vs Conan 2 vs hand-rolled `FetchContent`. vcpkg is Microsoft-maintained, integrates with CMake toolchain files, pins everything in a checked-in `vcpkg.json`, and hosts every dependency on MCE's approved list (SDL3, glad2, imgui[docking], stb, meshoptimizer, Tracy, doctest, spdlog, directxmath, DirectXShaderCompiler/glslang). Conan 2 is better at multi-platform lockfiles, which MCE doesn't need. Git submodules build from source every CI run and drag in transitive noise — binding already requires an ADR for them.

### 1.2 Compile-time engineering (matters more than people think)

At 10–15 h/week, a 3-minute build steals a whole evening per week of *waiting*. Standard mitigations, in order of measured payoff (see the recurring unity-build/PCH data in *Large-Scale C++* (Sutter) and countless engine postmortems):

1. **PCH** for the STL + core headers: biggest single win on MSVC.
2. **Unity builds** (`CMAKE_UNITY_BUILD ON`) — collapses TUs; the known cost is static/anonymous-namespace collisions, which you fix by keeping files self-contained.
3. **`/MP`** with Ninja (Ninja parallelizes across targets; MSVC needs `/MP` within a target — with Ninja generators CMake handles this).
4. **Pimpl + forward declarations across module boundaries**; templates only where they pay rent (containers, ECS iteration).
5. **Compile caching exists on Windows, but plan around it anyway**: sccache (Rust, first-class MSVC support) and ccache 4.4+ (MSVC `cl` support) both work on Windows in 2026. The catch: cache granularity collapses under unity builds (one giant TU = one giant cache entry, invalidated by any included header), so caching fights mitigation #2. Commit to #1–#4; if iteration still stalls later, try sccache on the non-unity `msvc-asan` preset where it pays most, not on the main dev build.
6. **Link**: `/DEBUG:FASTLINK` for debug, `/LTCG` only for shipped release.

Language-level: skip C++20 modules deliberately. MSVC modules + vcpkg + unity builds do not compose reliably today, and module-aware tooling (clang-tidy, vcpkg toolchain files) still lags; plain headers + `#pragma once` is what shipping engines actually do on Windows in 2026.

### 1.3 Recommendation for MCE — build

**Committed: CMake 3.28+ with `CMakePresets.json` as the only entry point, Ninja for dev, VS 2022 generator for debugging, vcpkg manifest mode with a pinned baseline, unity builds + PCH, GitHub Actions CI on windows-latest.**

Concrete policy:

- Targets: `mce_core` (math/alloc/containers/jobs/log), `mce_ecs`, `mce_rhi` (interface) + `mce_rhi_gl` (backend), `mce_render`, `mce_asset` (cook), `mce_audio`, `mce_physics2d`, `mce_editor`, `mce_runtime` (game host), test executables per module. This decomposition is what makes unity builds safe and compile times survivable.
- Preset names are load-bearing documentation: `msvc-debug`, `msvc-release`, `msvc-asan`, `msvc-cook`, `ci` (everything + clang-tidy).
- Base flags: `/W4 /WX /permissive- /Zc:preprocessor /Zc:__cplusplus /EHsc-` (exceptions off via `/EHs-c-` plus `/D_HAS_EXCEPTIONS=0` where dependencies need it), `/fp:fast` in release for math-heavy code (never in the physics determinism test build — see §8).
- Dependencies only through vcpkg manifest; a dep that vcpkg lacks (e.g. a specific Tracy tag) is vendored under `third_party/` with an ADR, never added ad hoc.
- CI (GitHub Actions, windows-latest): build all presets, run tests, run clang-tidy on changed files, upload the day's PDBs as artifacts for the crash pipeline (§7.3).

---

## 2. Memory management

### 2.1 The taxonomy (Gregory ch. 6 is the canonical treatment)

Engines converge on the same zoo, because the general-purpose allocator is the wrong tool for every hot path:

- **Linear (arena) allocator** — bump pointer, reset in O(1). Perfect lifetime: frame, level-load, cook pass.
- **Stack (LIFO) allocator** — bump + scoped pop, for nested temporary lifetimes; often double-ended (front for "known size", back for "unknown size" scratch).
- **Pool allocator** — fixed-size free list; O(1) alloc/free, zero fragmentation; the natural home for entities, particles, audio voices, command buffers.
- **General fallback** — TLSF (two-level segregated fit) is the engine-industry favorite: O(1) alloc/free with bounded fragmentation (Matt Conte's original design; used in Unreal as `MallocTLSF` and in countless console engines). dlmalloc/ptmalloc/mimalloc are the "just good enough" alternatives.

The *policy* question is more important than the *implementation*: which allocator backs which lifetime, and who is allowed to call the heap at all.

### 2.2 Threads of evidence worth stealing

- **Bitsquid / Our Machinery (Niklas Gray, "Building a Data-Oriented Entity System")**: memory and ECS design are one topic; per-system temporary allocators, no hidden allocations inside API calls, everything explicit.
- **Gaffer On Games, "Fix Your Timestep" + allocator-adjacent posts**: frame stability is an engineering discipline, not an accident — the sim must not change behavior because the heap moved.
- **mimalloc/JEmalloc research line** (Leijen et al., "mimalloc: Free List Shuffling in Action"): a modern general allocator is excellent; that is precisely the argument for *not writing* one unless measurement demands it.

### 2.3 Recommendation for MCE — memory

**Committed: three hand-written allocators, one deferred, and a strict "no hidden allocation" rule.**

1. **Frame linear allocator (M0).** Per-frame bump allocator, thread-local instances, 16 MiB budget per frame slot, reset at frame end after the GPU fence (GL: after `glClientWaitSync` on the frame slot — same ring discipline as the persistent-mapped staging buffers). Every per-frame allocation in sim, render submission, and ImGui comes from here. This is the single highest-leverage allocator in the engine; write it first, use it everywhere, measure it with Tracy memory plots.
2. **Pool allocators (M0–M1).** Fixed-size free-list pools for: ECS archetype chunks (a chunk is just a 64 KiB block — the chunk pool *is* the archetype allocator), audio voices, `RenderPacket` scratch, cook-stage intermediates. No templated magic — one `Pool<T>` with intrusive free list, ~100 lines.
3. **Double-ended stack allocator for cook (M1).** Cook passes are deep, nested, and single-threaded per file; a LIFO arena per file is both faster and *simpler* than heap juggling, and makes "did cook leak?" trivial (assert `used == 0` at pass end).
4. **General fallback (M2, deferred).** TLSF written as a deliberate learning exercise, switched on behind a flag for editor allocations only; shipping runtime still uses CRT malloc as the default until profiling shows TLSF wins. *Alternative considered*: adopting mimalloc as the default — rejected for the runtime (it hides the lesson and violates the from-scratch list) but noted as the standard "real engine" answer if MCE ever needed team-scale throughput.
5. **No hidden allocations:** engine APIs never call `new`/`malloc` internally; containers take an allocator argument; `std::vector` is confined to tool/editor code. Frame rule from the binding: zero heap allocation in hot paths, frame-linear only.
6. **Generational-index handles everywhere across frames** (Bitsquid ID lookup table design; the binding already mandates this). Raw pointers to sim state are compile-time discouraged: a small `Handle<T>` type with a per-table generational lookup is the *only* legal cross-frame reference to GPU/asset/entity state.
7. **Debug tooling, not more allocators:** build one *tracking* allocator (tag, size, peak, Tracy `tracy::Memory` hooks) instead of many specialized ones. Leak detection is ASan's job (§8); the tracking allocator's job is answering "who owns the 16.6 ms frame's 9 MB?"

**VRAM note:** GL 4.5 DSA makes this simple — `glCreateTextures`/`glNamedBufferStorage` with immutable storage; the allocation "strategy" problem (D3D12MA-style placed resources) doesn't exist on GL. Budget-by-capacity tracking in the render layer, revisit if a Vulkan/D3D12 backend ever materializes.

---

## 3. Multithreading: task systems, fibers, and what a solo engine actually needs

### 3.1 The survey

- **No system (single thread).** Honest option until profiling proves otherwise. On GL 4.5 this is *more* viable than on D3D12: a GL context is bound to one thread anyway, so "multithreaded renderer" buys command *recording* parallelism only, not submission.
- **Thread pool with a job graph** (fork-join tasks with dependencies): the standard modern design — id Tech's job lists, Destiny's "Parallelizing the Rage" *GDC 2015* line of talks, Unity DOTS's scheduling model. `Counter` semantics (EnTT-style counters that jobs wait on) are the load-bearing idea: declare dependencies, don't share locks.
- **Fiber-based scheduling** (Christian Gyrling, *Parallelizing the Naughty Dog Engine Using Fibers*, GDC 2015): fibers let jobs *yield* on a dependency instead of blocking a worker thread. Spectacular at Naughty Dog scale (tens of thousands of jobs/frame, deep dependency graphs). Costs: MSVC supports fibers (`CreateFiber`/`ConvertThreadToFiber`, or `std::execution`-less hand-rolled coroutine stack switching), but they break TLS, break profilers that assume thread stacks, and their win evaporates below a few hundred concurrent blocking jobs.
- **Off-the-shelf pools** (Intel TBB, `BS::thread_pool`, `std::async`): all rejected by binding (never a framework). `std::async` is semantically wrong (futures, not graphs) and allocation-happy.

### 3.2 When does a solo 2D/3D engine actually need one?

Answer, with receipts: **later than you think, and as a job graph, never fibers.**

- M0–M1 (triangle → 2D game): single-threaded main loop. The only *other* threads are SDL3's audio callback (real-time, §3.3) and possibly a file-IO thread. A 2D sprite game at 60 Hz has ~3–5 ms of sim+render work; parallelizing 3 ms is negative-sum after sync costs.
- M2 (3D renderer, cook pipeline): the **cook tool** is the first real customer — glTF meshes, texture compression (ispc_texcomp is embarrassingly parallel), shader compilation all parallelize trivially. This is also the *safest* place to learn a job system: it's offline, deterministic, and crash-tolerant.
- M3+ (physics islands, culling, audio DSP pre-pass): runtime data-parallel jobs enter when the frame budget table (§6) shows a hot stage exceeding ~25% of frame.
- Fibers: never, on current evidence. The trigger condition would be "hundreds of jobs that block on I/O or other jobs mid-execution" — MCE's job count per frame is projected in the dozens. Gyrling's own talk frames fibers as the answer to *job graph deadlock at scale*, which is a scale MCE will not reach.

### 3.3 The one thread with hard real-time rules: audio

SDL3's audio callback runs on a driver thread with a deadline (~10 ms buffer). Rules (Timur Doumler, *C++ in the Audio Context*, CppCon 2015; also standard JUCE/FMOD practice): no locks against the main thread (SPSC ring buffer of mix commands instead), no allocation, no syscalls, no logging in the callback. This is hand-written anyway per the from-scratch list, so the discipline is free.

### 3.4 Recommendation for MCE — concurrency

**Committed: single-threaded simulation+render until M2; hand-written thread-pool job graph with counter/dependency semantics introduced by M3 (learned first inside the cook tool at M2); no fibers ever, absent a profiling surprise; SPSC ring for audio commands.**

Sketch of the committed API shape (standard counter-based fork/join, cf. Destiny's job system talk):

```cpp
namespace mce::jobs {
    using JobFn = void(void* ctx);                 // no captures, no exceptions
    struct Counter { std::atomic<uint32_t> v{0}; }; // increment per pending job

    void init(uint32_t worker_count = std::max(2u, std::thread::hardware_concurrency() - 1u));
    void shutdown();

    Counter* run(JobFn fn, void* ctx, Counter* depends_on = nullptr); // fork
    void wait_for(Counter* c);                    // join; workers help while waiting
}
// Cook usage: one counter, N mesh jobs, wait; then texture pass; then shader pass.
```

Worker threads: `std::jthread` + `std::condition_variable_any` + a lock-free-ish MPMC queue — a plain mutex-guarded deque is *fine* at this job count; do not hand-roll a Chase-Lev deque until Tracy shows contention. Job context memory comes from the frame allocator for runtime jobs, from the cook arena for cook jobs.

---

## 4. SIMD math strategy

### 4.1 The survey

- **DirectXMath** (Microsoft, shipped in Windows SDK / vcpkg `directxmath`): excellent SSE/AVX implementations, right-handed modes, but its API surface *is* the engine's math API if adopted — binding forbids that (the math library is from-scratch). Keep as **test oracle only** (binding already says so).
- **GLM**: the GL-tutorial default; header-only, huge template surface, slow to compile, and adopting it would outsource exactly the subsystem MCE exists to write. Rejected.
- **glm-like hand-rolled with raw intrinsics everywhere**: the classic over-correction — intrinsics in every function makes the scalar reference implementation disappear and bugs harder to isolate.
- **Agner Fog's VCL**: superb vector class library, but it's another dependency with an unusual license model, and it optimizes a problem MCE doesn't have (general vector math at scale) rather than the one it does (correct, cache-friendly, scalar-first math with measured SIMD hotspots).
- **Scalar-first + intrinsics at measured hotspots**: what shipping engines actually converge on (Unreal's math is largely scalar C++ with intrinsics in marked hot paths; id Tech likewise).

Key insight from Agner Fog's manuals and Lemire's benchmarking work: **memory layout beats instruction selection.** An SoA array of 4 floats processed with scalar code often beats AoS `float4` with SSE, because the win comes from bandwidth and batching, not from replacing an add with `_mm_add_ps`.

### 4.2 Recommendation for MCE — SIMD

**Committed: scalar-first math library (binding), SSE4.2 as the compile baseline, raw MSVC intrinsics in explicitly marked hot paths only, no runtime dispatch, no external SIMD dependency.**

- Baseline: x64 guarantees SSE2; committing to **SSE4.2** (2008+) is safe for Windows 10 x64 targets and enables `_mm_mullo_epi32`, blend ops, and PCMPESTRM-free string ops that occasionally matter in cook code. No `/arch:AVX` global flag — AVX2 paths, if ever needed (broadphase sweeps), get a runtime `__cpuid` check behind a function pointer, written only if profiled.
- Where SIMD is expected to pay (write intrinsics here, and only here, when Tracy shows the cost): 4×4 matrix multiply batches (skin/bone palette updates), sprite-batch transform blocks, broadphase AABB sweeps in 2D physics (SoA floats), audio mixing loop (saturating `_mm_adds_ps`-style mix), cook-time quantization.
- Everything else stays scalar with `constexpr`/inlined small structs (`Vec2/3/4`, `Mat4`, `Quat`), column-vector/column-major per binding. DirectXMath is linked into the **test project only** and compared with `EXPECT_NEAR` tolerances in units (§8).
- Compiler config: `/fp:fast` for release runtime, `/fp:precise` for the determinism test build and for physics solver code, so the sim hash test (§8.4) isn't testing compiler float reassociation.

---

## 5. Profiling & graphics debugging

### 5.1 Survey

- **Tracy** (Wolf's client/server profiler): zone-based, real-time, with locks/contention, memory, frame marks, GPU zones, and call-stack sampling. The de-facto choice for indie/AA engine profiling in 2026 (adopted across The Forge, the Rust `tracing-tracy`/Bevy ecosystem, and countless indie engines). vcpkg-portable; client overhead ~ns-per-zone when zones are compiled in, ~zero when disabled.
- **Superbly complementary but wrong fit:** Optick (dormant since 2022), ETW/WPA (great for OS-level: file IO, cold start, DWM), VS Profiler (fine, coarse), Superluminal (best-in-class sampling profiler, but paid — declined for a hobby budget).
- **GPU:** **RenderDoc** is the graphics debugger of record for GL (binding mandates it per render feature). PIX doesn't do GL; the Windows GL driver debug output (`glDebugMessageCallback`/`GL_KHR_debug`) plus RenderDoc covers GL debugging. GPU timing: `GL_TIMER_QUERY`/`glQueryCounter` for our own overlay, and Tracy's GL integration (`TracyGpuContext` + `glQueryCounter` under the hood) for the profiler timeline.

### 5.2 Recommendation for MCE — profiling

**Committed: Tracy wired at M0 as the only CPU profiler; RenderDoc as the only graphics debugger; `GL_KHR_debug` callback always on in debug builds; per-milestone frame budget table enforced.**

M0 integration checklist (binding requires this; keep it literal so it's checkable):

- `FrameMark` + named `ZoneScoped` in: main loop, sim fixed-step tick, ECS archetype iteration, render submit, RHI command translation, asset load, cook steps.
- `ZoneText`/`ZoneColor` for pass names; `FrameImage` for the in-Tracy frame thumbnail (free "what did the frame look like" tool).
- GPU zones via the GL context integration so CPU/GPU rows align in one capture.
- Memory: route the frame allocator + pools through Tracy's alloc hooks so the Memory track shows real engine behavior from day one.
- Frame budget table (16.6 ms @ 60 Hz), per milestone, e.g. M2 target: sim 3.0 / cull+sort 1.5 / RHI translate 1.5 / GL submit 2.0 / GPU 6.0 / headroom 2.6. Numbers live in the milestone doc; Tracy screenshots live next to them.
- Release/runtime builds: Tracy zones compiled out via `TRACY_ENABLE=OFF` except the `FrameMark`; a shipped game must not carry profiler overhead.

---

## 6. Logging, asserts, and crash handling

### 6.1 Logging

Options: spdlog (vcpkg, async sinks, fmt-based, compile-time level filtering), hand-rolled (~300 lines: ring buffer + formatted sink to editor console + file), or nothing. spdlog is the pragmatic industry default; the only argument against is "write it yourself," and logging is *not* on the binding's from-scratch list — the learning budget belongs to the mixer and ECS, not to `vsnprintf` plumbing.

**Committed: spdlog, pinned in vcpkg.** Sinks: debug OutputDebugStringA sink + console sink in dev; file sink in `%LOCALAPPDATA%/MCE/logs` (rotated); editor in-app console sink feeding an ImGui panel (round-trip through a locked SPSC buffer — the editor thread is just another consumer). Compile-time filter: `SPDLOG_ACTIVE_LEVEL` = debug in dev presets, warn in shipped runtime. Hot-path rule: logging never in per-entity/per-draw loops; per-frame summary instead.

### 6.2 Asserts

**Committed: one custom `MCE_ASSERT`/`MCE_VERIFY` macro family, layered with the binding's error-handling policy.**

```cpp
#define MCE_ASSERT(cond) \
    do { if (!(cond)) { MCE_LOG_CRITICAL("assert: {}", #cond); \
          MCE_DEBUG_BREAK(); __fastfail(FAST_FAIL_FATAL_APP_EXIT); } } while (0)
// Debug builds: MCE_ASSERT = assert + log + break.  Release: compiled out.
// MCE_VERIFY: evaluated in all builds, breaks only in debug — for "should never
// happen, but degrade gracefully" paths (e.g. handle generation mismatch → log + skip).
// Programmer error   -> assert/crash (fail loudly, repro locally)
// Expected failure  -> Result<T, Error>   (missing file, bad magic, stale handle)
// Never: exceptions (binding), never: silent error codes ignored.
```

`__fastfail` (Windows 8+) bypasses SEH dispatch and our own handler — it is the correct primitive for "the assert itself must not recurse." C standard `assert()` is disabled (`NDEBUG`) everywhere so nothing bypasses the policy.

### 6.3 Crash handling

Survey: **Breakpad/Crashpad** (Google) — battle-tested, but a build-system bear on MSVC and designed for fleet-scale symbol servers; overkill for one developer. **CrashRpt** — aging, Windows-only wrapper. **Hand-rolled `MiniDumpWriteDump`** — ~150 lines, uses the OS's own best-in-class dumper, gives a file Visual Studio loads with full locals when PDBs match.

**Committed: hand-rolled SEH top-level filter + `MiniDumpWriteDump` (dbghelp), PDBs archived per CI build.**

- `SetUnhandledExceptionFilter` + `_set_purecall_handler` + terminate/invalid-parameter handlers → write `minidump-with-indirectly-referenced-memory` to `%LOCALAPPDATA%/MCE/crashes`, show a one-line ImGui/MessageBox ("crash report written, path"), then `__fastfail`.
- PDBs for every CI build are uploaded as GitHub Actions artifacts and named by build ID; a crash dump is only useful with its matching PDB, so the dump embeds the build ID (write it into a `CommentStreamW` via `MiniDumpWriteDump`'s callback).
- WER dialogs suppressed per-process (`SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)`) so a crashed editor doesn't wait for a user click.
- Editor-first rule: the crash handler runs in the editor and in the cook tool (where it most often pays), and in shipped games it writes the dump and exits cleanly — no silent corrupt-save paths (close/flush saves *before* the fastfail).

---

## 7. Testing strategy for engine code

### 7.1 Framework choice

| Option | Verdict |
|---|---|
| **doctest** | Single header, ~20× faster compile than Catch2 v2, `TEST_CASE` + sections, subcases; the fastest edit-test loop for a solo dev. Maintenance cadence is slow (2.4.x line) but the library is finished, not abandoned. |
| Catch2 v3 | Better maintained, nicer matchers, but compiles as a static lib and is heavier in link time; per-test-run loop is noticeably slower to iterate. |
| GoogleTest | Industry standard, but heavyweight for a solo engine and pulls a dependency surface larger than every other MCE test dep combined. |

**Committed: doctest** (vcpkg-pinned) for unit tests, with a documented migration path (tests are plain functions registered via macros — porting to Catch2 later is mechanical if doctest ever stalls). Golden-image and determinism harnesses are plain executables, not framework tests.

### 7.2 What gets tested, and how (per binding)

1. **Math vs DirectXMath oracle (M0).** Every operation in `mce::math` compared against DirectXMath over randomized sweeps: matrix products, inverse, quat slerp, projection construction (GL NDC [-1,1] convention asserted here, catching the classic z-range bug at its source). Tolerances in *meters/radians*, not unitless epsilon.
2. **ECS invariants (M1).** Add/remove/iterate storms across archetypes; generational-handle staleness (a freed entity's old handle must fail lookup, never alias); move-semantics of component storage; iteration order stability for a fixed seed (the renderer depends on it for determinism).
3. **Serialization round-trips + `.mce` container (M1).** Write→read→byte-identical property tests with randomized payloads; negative tests: bad magic, truncated file, CRC mismatch, version-ahead-of-reader must all fail with a *diagnosable* `Result` error, not a crash. This is where `Result<T,E>` earns its keep.
4. **Fixed-step determinism (M1, continuous).** Run the sim N ticks from a fixed seed twice (and once with `-O2` vs `/fp:precise` build), hash all component arrays, require equal hashes. Catches: unordered iteration, uninitialized memory, float non-determinism. This is the test that keeps the "determinism is a test target, not an accident" binding honest.
5. **Golden-image tests (M2+).** Cook-time job renders a fixed scene to an offscreen FBO, dumps PNG, compares against reference with a perceptual threshold (small per-channel RMSE + histogram distance, not byte equality — driver versions change pixels, and byte equality CI is a false-alarm machine). Every new render feature adds a scene + reference image; RenderDoc capture path is the same code as the golden dump, so debugging and testing share plumbing.
6. **Allocator/jobs tests (M0–M3).** Frame allocator: allocation patterns, alignment, overflow assert, reset. Job system: hammer tests (10k jobs with dependency diamonds) under ASan/TSan; a stress build in CI, not per-PR.

### 7.3 CI + sanitizer policy (binding-aligned)

- **Per PR:** `msvc-debug` build + unit tests; `msvc-asan` build (MSVC `/fsanitize=address` — mature on MSVC 19.37+, catches UAF/leaks in the frame allocator and cook pipeline) + a short smoke test run; `clang-tidy` on changed files with a small checked-in check list (bugprone-*, performance-*, readability-identifier-naming for the `mce_` convention).
- **UBSan:** MSVC has no UBSan equivalent; run a **clang-cl UBSan job** on windows-latest weekly (or on milestone candidates, not per-PR — clang-cl + vcpkg + GL interop has enough friction that per-PR would be a tax with low yield; weekly catches the drift). This satisfies the binding's ASan/UBSan intent with honest tradeoffs documented.
- **Per milestone:** full cook determinism check (cook twice, hash outputs), golden images, determinism hash test, ASan cook of the full asset set, Tracy capture of the milestone scene attached to the milestone doc.
- **No green pipeline, no milestone merge** (binding). Enforce with required checks on the (single-developer) repo; the discipline is for future-you, who cannot be trusted to remember.

---

## 8. Recommendation for MCE — the committed platform, in one table

| Area | Decision | Phase |
|---|---|---|
| Build | CMake 3.28+ presets, Ninja dev / VS debug, unity + PCH, `/W4 /WX /permissive- /Zc:preprocessor`, exceptions off | M0 |
| Packages | vcpkg manifest, pinned baseline; deps: SDL3, glad2, imgui (docking), stb, meshoptimizer, Tracy, doctest, spdlog, directxmath (test-only) | M0 |
| Memory | Frame-linear allocator (thread-local, 16 MiB/slot) + fixed pools + cook LIFO arena; TLSF deferred to M2 behind a profiling trigger; handles-not-pointers | M0–M2 |
| Concurrency | Single-threaded until M2; hand-written counter/fork-join job graph from M3 (learned in cook first); no fibers; SPSC ring to audio thread | M0–M3 |
| SIMD | Scalar-first; SSE4.2 baseline; intrinsics only in measured hotspots (matmul batches, broadphase, audio mix); `/fp:fast` release, `/fp:precise` determinism build | M0, grows with profiling |
| Profiling | Tracy at M0 (frame/ECS/render/asset zones, GPU zones, memory hooks); RenderDoc mandatory per render feature; frame budget table per milestone | M0+ |
| Logging | spdlog, multi-sink (console, file, editor ImGui panel), compile-time level, no hot-path logs | M0 |
| Asserts | `MCE_ASSERT`/`MCE_VERIFY` → log + break + `__fastfail`; `Result<T,E>` for expected failures; std `assert` unused | M0 |
| Crashes | SEH filter + `MiniDumpWriteDump` + PDB archive keyed by build ID; WER suppressed; handler in editor/cook/runtime | M0 (editor), M1 (runtime) |
| Testing | doctest unit tests (math oracle, ECS, round-trips, `.mce` negatives), sim determinism hash test, cook-time golden images, ASan per-PR, clang-cl UBSan weekly, clang-tidy per-PR | M0, layered |
| CI | GitHub Actions windows-latest: `msvc-debug`, `msvc-asan`, `msvc-cook`, `ci` (tidy+tests); PDB artifacts | M0 |

**Solo-dev overhead audit:** every committed choice is either (a) vcpkg-pinned and boring (spdlog, doctest, Tracy), (b) small enough to write in a day and read forever (frame allocator, assert macro, minidump handler, job system), or (c) *deletion* of work (no fibers, no TLSF until measured, no mimalloc, no UBSan per-PR). The only genuinely novel engineering in this domain — the job graph and the allocator family — is exactly the engine substance MCE exists to learn, scheduled where it can't block the render bootstrap.

## 9. References (canonical, from knowledge)

- Jason Gregory, *Game Engine Architecture*, 3rd ed. — ch. 6 (allocators/containers), ch. 15 (rendering), ch. 13 (audio threading model).
- Glenn Fiedler, "Fix Your Timestep" — Gaffer On Games (fixed-step + interpolation; also the determinism-test philosophy).
- Erin Catto, Box2D GDC slide series (solver structure; also a model of algorithm-writing-with-tests).
- Christian Gyrling, *Parallelizing the Naughty Dog Engine Using Fibers*, GDC 2015 (fibers: the benchmark MCE deliberately does not meet).
- Destiny engine job system talks (Drew/"Destiny's Multithreaded Rendering Architecture", GDC 2015) — counter/fork-join semantics MCE adopts.
- Timur Doumler, *C++ in the Audio Context*, CppCon 2015 — real-time audio rules.
- Matt Conte, TLSF (original design); Unreal `MallocTLSF` as production reference.
- Niklas Gray / Bitsquid–Our Machinery blogs — ID lookup tables, data-oriented systems, explicit allocation discipline.
- Agner Fog, *Optimization manuals* (layout vs. instruction choice); Daniel Lemire's SIMD benchmark posts.
- Microsoft DirectXMath documentation; MSVC `/fsanitize=address` documentation; `MiniDumpWriteDump`/dbghelp docs.
- Tracy manual (wolfpld/tracy) — GL integration, memory hooks, frame images.
- vcpkg documentation (manifest mode, baselines); CMake presets documentation.
