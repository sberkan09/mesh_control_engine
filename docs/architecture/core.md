# Architecture: Core & Platform Layer (`mce_core`, `mce_platform`, `mce_refl`)

**Project:** Mesh Control Engine (MCE) · **Document class:** Architecture — subsystem
**Subsystem:** `core` · **Date:** 2026-10-06 · **Revision:** 2 — applies the binding errata from `docs/ARCHITECTURE.md` §5.1 (fixes F1, F2, F3, F4, F7, F8, F10, F11, F12). Where this doc and ARCHITECTURE.md disagree, ARCHITECTURE.md wins.
**Inputs:** `docs/REQUIREMENTS.md` (authoritative), `docs/research/platform-engineering.md`, `docs/ARCHITECTURE.md`, binding stack decision (C++20 / MSVC v143, GL 4.5 Core behind a thin RHI, SDL3, fixed 60 Hz sim, no exceptions/RTTI in the runtime core).
**Scope note:** "Core" here is everything *beneath* the other subsystems: memory, containers, math, handles, `Result`, logging/asserts/crash handling, convars, jobs, time, the application/main-loop shell, the platform seam (window, filesystem, threads, clocks), and the reflection registry (`mce_refl` — homed in this doc per ARCHITECTURE fix F4 because no other subsystem doc owns it; it is a separate L1 CMake target, not part of `mce_core`). Every other subsystem (rhi-renderer, ecs-gameplay, physics, assets, editor-tools, audio-input-ui, scripting) sits above core and may not be included by it.

---

## 1. Responsibilities & non-responsibilities

### 1.1 Responsibilities (what this layer owns)

| Area | Owned by | Notes |
|---|---|---|
| **Memory management** | `mce_core` | Allocator interface, frame-linear allocator + `MCE_FRAMES_IN_FLIGHT = 3` slot ring (F1), pools, LIFO cook arena, tracking allocator, `Handle<Tag>`/generational tables. Policy: no hidden allocation. |
| **Containers** | `mce_core` | `Array<T>`, `SmallVector<T,N>`, `FixedArray<T>`, `HashMap`, `FlatMap`, `BitSet`, `RingBuffer` (SPSC), **`MpScQueue<T>` (F10)**, `StringId`, `FixedString<N>`. Allocator-aware, exception-free. |
| **Math library** | `mce_core` | `mce::math` — scalar-first `vec2/3/4`, `mat4`, `quat`, `aabb2/3`, `Plane`, `Rect`, `Color`; conventions fixed here (RH, Y-up, meters, radians, column-vector/column-major, GL NDC). SIMD only at measured hotspots. DirectXMath = test oracle only. |
| **Error handling** | `mce_core` | `Result<T, Error>`, `Error` (code + stage + path + detail), `MCE_TRY`, `MCE_ASSERT`/`MCE_VERIFY`, crash handler (SEH + `MiniDumpWriteDump`), breadcrumb ring, build ID. |
| **Logging** | `mce_core` | spdlog-based multi-sink facade: console, `OutputDebugStringA`, rotating file in `%LOCALAPPDATA%/MCE/logs`, in-memory ring feeding the editor console panel. |
| **Configuration** | `mce_core` | Convar/confunc registry, command-line parsing, per-build flags. No runtime file-format config parsing in the shipped game (content is `.mce` only — FR-AST-010). |
| **Time & the main loop** | `mce_core` | `app::Application` shell, *Fix Your Timestep* fixed-step loop with interpolation alpha and spiral-of-death clamp, frame-slot lifecycle, headless mode, generic `StateStack<T>`. |
| **Jobs** | `mce_core` | Counter/fork-join job graph. API + *serial inline* implementation from M0; first real customer is the cook tool (ROADMAP M2, learned offline); runtime parallel enablement flag-gated at v0.5 (ROADMAP M5a, per ARCHITECTURE §10.1). |
| **Reflection registry** | `mce_refl` (L1, F4/ADR-0009) | Generic field/trait metadata + `MCE_COMPONENT` registration + property paths. The **sole** metadata source: ecs-gameplay builds component registration on it; assets (serialization), editor-tools (inspector/undo), audio-input-ui (rebinding persistence), scripting (`mce::ui` binding) all consume it. `mce_core` itself stays reflection-free. |
| **Platform abstraction** | `mce_platform` | Window + event pump (SDL3), filesystem (allocator-aware API + Windows mmap), virtual mounts, threads/TLS, dynamic library load, `SDL_Event` quarantine. |
| **Dependency-rule enforcement** | build + CI | The CMake target graph *is* the rule; include-grep walls are the second lock (§1.3). |

### 1.2 Non-responsibilities (explicitly elsewhere)

- **ECS, components, inter-system events, scene-state *semantics*** — `ecs-gameplay`. Core provides the *generic* `StateStack<T>` and the loop; the runtime defines what a scene state *is*. Core never includes `mce_ecs`.
- **The per-tick context of record is `Frame` (`mce_ecs`), not core's `TickContext`** (F3). Core's `TickContext` is only the app-shell payload (dt, tick index, input reference) from which the World builds `Frame`; `TickContext` exists for non-ECS sample apps. See §2.1.
- **GPU anything** — `rhi-renderer`. Core knows nothing about GL; the only coupling is the abstract fence callback the renderer registers to release frame slots (§4.3).
- **Asset formats, cooking, `.mce`** — `assets`. Core provides mmap primitives and CRC32/xxhash64 only.
- **Input action mapping, audio mixing, runtime UI** — `audio-input-ui` (and `mce::ui` at v1.0). `mce_platform` delivers raw timestamped device events and stops there (FR-INP-010: no `SDL_Event` outside `mce::platform`, CI-grepped).
- **Editor, ImGui** — `editor-tools`. Core has zero ImGui knowledge.
- **Scripting / game module loading** — `scripting`. Core exposes only the plain function/arena surface a DLL host could use (`platform::load_library`); the `mce_game_module` loader is scripting's.
- **Reflection *trait semantics* for components/serialization** — `mce_refl` owns the registry; *what* components and scenes mean is ecs-gameplay/assets. Core contains no reflection.

### 1.3 The module dependency rule (amended per F7, F8, F11)

Layers, strictly downward. A module may include only modules **below** it. This table matches ARCHITECTURE.md §2.1 (authoritative):

```
L0  mce_core        (STL subset, Tracy, spdlog)            — includes nothing above
L1  mce_platform    (SDL3, Win32, dbghelp)                  — includes L0
L1  mce_refl        (field/trait registry)                  — includes L0
L1  mce_ecs         (archetype tables, phases, Frame)       — includes L0, L1(mce_refl)
L2  mce_rhi         (RenderDevice/CommandBuffer interface)  — includes L0
L2  mce_rhi_gl      (the ONLY GL code — glad2)              — includes mce_rhi, platform (window handle only)
L2  mce_asset       (.mce reader, GUIDs, cook lib)          — includes L0, mce_refl, mce_ecs, mce_platform
L2  mce_input       (action maps → InputState)              — includes L0, mce_platform, mce_asset, mce_ecs
L2  mce_audio       (mixer on SDL3 callback)                — includes L0, mce_platform, mce_ecs
L2  mce_physics2d   (hand-written Catto solver)             — includes L0, mce_ecs
L2  mce_physics3d   (v0.5, Jolt 5 behind ADR-0003)          — includes L0, mce_ecs
L3  mce_render      (extract/cull/sort/batch/passes)        — includes L0..L2 (never editor, never GL outside rhi_gl)
L4  mce_gameplay_kit (starter components/systems)           — includes L0..L3
L4  mce_ui          (v1.0, retained runtime UI)             — includes L0..L3
L5  mce_engine.dll  (umbrella library aggregating L0–L4;    — "mce_runtime" is the collective
                     hosts the game-module loader)             NAME of this ImGui-free set (F8)
L5  mce_editor_core (headless: SceneDocument, UndoStack,    — includes mce_engine import lib
                     PlaySession, picking — no ImGui)
L5  mce_editor_layer (the ONLY ImGui consumer: panels,      — includes mce_editor_core, ImGui
                     viewport, gizmos; implements DrawCtx)
L6  game.dll + game.exe · mce_editor.exe · mce_cook.exe ·  — link mce_engine (exes) /
    mce_devhost.exe                                          mce_editor_layer (editor) only
```

Rules and enforcement:

1. **The CMake target graph is the rule.** `target_link_libraries` is the only include-path mechanism; no target lists a header path of a non-linked module (`target_include_directories` is per-target, never global).
2. **Target walls that carry requirement teeth** (all CI-enforced, ARCHITECTURE §1.2):
   - **ImGui wall:** nothing linked by `mce_engine.dll` may see ImGui; the shipped `game.exe`/`mce_engine.dll` are symbol-scanned (FR-UI-048). The wall is a *link error*, not an `#ifdef` — there is no `MCE_EDITOR` preprocessor branch anywhere in runtime code.
   - **GL wall:** `gl[A-Z]` symbols appear only inside `mce_rhi_gl` sources (binding RHI seam).
   - **SDL wall:** `SDL_Event` appears only in `mce_platform` sources (FR-INP-010). The single sanctioned leak is `Window::native_handle()` returning `SDL_Window*` to `mce_rhi_gl` and `mce_editor_layer` — allowlisted in the grep.
   - **JPH wall (v0.5):** `JPH::` symbols only inside `mce_physics3d` (ADR-0003).
   - **Gameplay module wall:** game code includes exactly one umbrella, `mce/gameplay.h` (FR-DX-010's 12-type budget), and links only the `mce_module_api` interface target.
   - **Core dependency floor:** `mce_core` links only Tracy + spdlog, ever. Anything new anywhere needs an ADR first — core is where dependency rot spreads from.
3. **Header hygiene:** modules expose one `mce_<mod>.h` umbrella; cross-module use goes through it; internals live in `mce::detail`. `#pragma once`; no C++20 modules (MSVC modules + vcpkg + unity builds do not compose — research §1.2).
4. **Unity-build discipline:** every `.cpp` self-contained (no anonymous-namespace symbol reuse across files); each module is its own unity target so collisions cannot cross modules.

### 1.4 Build & repository organization (binding BUILD policy, made concrete)

- **CMake 3.28+, `CMakePresets.json` as the only entry point.** Presets are load-bearing documentation: `msvc-debug` (Ninja, dev), `msvc-release`, `msvc-determinism` (same flags as release — it exists as a named alias for the FR-RUN-040 CI build, per finding 26 below), `msvc-asan` (`/fsanitize=address`), `msvc-vs` (VS 2022 generator, debugging only), `ci` (everything + clang-tidy). No MSVC project files checked in; no submodules without an ADR.
- **vcpkg manifest mode**, pinned baseline in `vcpkg.json`. Runtime deps (**SDL3, glad2, imgui-docking, Tracy, doctest, spdlog, directxmath *test-only***) vs **cook-time-only deps (nlohmann/json, meshoptimizer, stb_image/stb_truetype, glslangValidator, ispc_texcomp, msdf-atlas-gen — tools/cook and test targets only, never linked into `mce_engine.dll` or `mce_runtime`, FR-AST-010 symbol scan — finding F53)** are separated at the target level: cook-time deps link only into `mce_cook.exe`/editor cook host, never `mce_engine.dll`. *(Dependency mechanism superseded by ADR-0017: CMake FetchContent with pinned tags in `cmake/Dependencies.cmake`; there is no `vcpkg.json`.)*
- **Base flags:** `/std:c++20 /W4 /WX /permissive- /Zc:preprocessor /Zc:__cplusplus /EHs-c- /D_HAS_EXCEPTIONS=0`, RTTI off in runtime targets; **`/fp:precise` in every configuration — `/fp:fast` is banned engine-wide** (determinism must not depend on which preset a contributor built; no per-file pragmas, no drift surface, FR-RUN-040/NFR-F-04); `/LTCG` + `/DEBUG:FASTLINK`-vs-full split per preset.
- **PCH for STL + `mce/core.h`; unity builds on** (`CMAKE_UNITY_BUILD_BATCH_SIZE` tunable per preset for debugging).
- **CI (GitHub Actions, windows-latest):** all presets build, unit tests run, clang-tidy on changed files, ASan smoke per PR, clang-cl UBSan weekly, PDB artifacts uploaded per build ID, include-grep walls + symbol scans + cook determinism (from M2) + sim determinism (from M1). No green pipeline, no milestone merge (binding).
- Repository layout is ARCHITECTURE.md §8 (authoritative); `src/engine/core|platform|refl` are this doc's targets.

---

## 2. Public interface (API sketches)

These are the types gameplay and engine authors actually touch. FR-DX-010 caps the 2D-game-facing surface at ~12 `mce::` types; everything else lives under `mce::detail`.

### 2.1 Application shell and the main loop

```cpp
// mce/app.h  (L0)
namespace mce::app {

struct AppDesc {
    const char* name;
    uint32_t    window_width  = 1600;
    uint32_t    window_height = 900;
    bool        headless      = false;   // FR-RUN-025: no window, no GL, no audio device
    uint32_t    sim_hz        = 60;      // fixed step; dt == 1/sim_hz exactly
    uint32_t    max_catchup   = 5;       // spiral-of-death clamp (FR-RUN-020)
    bool        vsync         = true;    // FR-RUN-045; convar overridable
    Span<const char* const> args;        // argv, parsed into convars before init
};

// The one base class a game or the editor implements. Handmade Hero split:
// the Application IS the "game side"; platform is beneath it.
class Application {
public:
    virtual ~Application() = default;

    // Called once, after platform/services init. Register systems, load boot scene.
    virtual Result<void, Error> on_init(app::Context&) = 0;
    // One FIXED sim tick. dt is exactly 1/sim_hz; do not sample wall time inside.
    virtual void on_tick(const TickContext&) = 0;
    // Render with interpolation alpha ∈ [0,1). Runs 0..N times per sim tick.
    virtual void on_render(const RenderContext&) = 0;
    virtual void on_shutdown() = 0;

    // Scene-state stack (FR-RUN-030): menu/gameplay/pause are pushed states.
    StateStack& states() { return states_; }
    void        quit(int exit_code = 0);            // flushes saves (runtime-owned) then exits
    bool        want_quit() const;

private:
    StateStack states_;
};

int run(Span<char* const> args, Application& app);   // never returns; owns everything
} // namespace mce::app
```

The loop itself (the load-bearing ~30 lines of the engine — *Fix Your Timestep*, Gaffer On Games):

```cpp
// mce::app::run() — fixed-timestep with interpolation (FR-RUN-020)
double previous = platform::now_seconds();
double accumulator = 0.0;
constexpr double MAX_FRAME = double(max_catchup) / sim_hz;   // 5/60 s clamp
while (!app.want_quit()) {
    FrameMark;                                    // Tracy frame mark (binding: M0)
    const double now   = platform::now_seconds();
    double frame_time  = now - previous;  previous = now;
    frame_time = std::min(frame_time, MAX_FRAME); // spiral-of-death clamp
    accumulator += frame_time * time_scale;       // FR-RUN-050: scale the accumulator
                                                  // inflow, NOT dt — ticks stay fixed-size
    platform::pump_events();                      // -> event queue; mce_input maps to actions
    while (accumulator >= fixed_dt) {             // 0..max_catchup iterations
        sim.tick(TickContext{ .dt = fixed_dt, .tick_index = ++tick });
        accumulator -= fixed_dt;
    }
    const float alpha = float(accumulator / fixed_dt);  // render interpolation
    app.on_render({ .alpha = alpha });
    frame_slot_end();   // signal fence, release this frame's slot (§4.3)
    platform::present(vsync);
}
```

Why accumulator-alpha and not semi-fixed or free-running: the determinism requirement (FR-RUN-040) is only satisfiable if sim dt is a compile-time constant; interpolation alpha is the render-side compensation. `time_scale` multiplies the *accumulator inflow*, never dt — so slow-mo is determinism-preserving by construction (FR-RUN-050's hash test passes for free).

**The per-tick context (F2/F3 amended):** the ECS `Frame` (`mce_ecs`) is the per-tick context of record for all systems — every consumer doc's `Frame&` is the gameplay-facing type. Core's context types shrink to the app-shell payload and exist for non-ECS sample apps (`01_triangle`):

```cpp
// mce/app.h (L0) — app-shell payload only; the World builds mce::Frame from it
struct TickContext {
    double   dt;            // exactly 1/sim_hz, never wall time
    uint64_t tick_index;
    const input::InputState& actions;   // F2: canonical type is mce::input::InputState
};                                      // core fwd-declares it; input → core, never core → input
using ActionSnapshot = input::InputState;   // F2 alias — kept so core-internal code reads naturally
struct RenderContext { float alpha; uint64_t frame_index; };
```

The `InputState&` is delivered as an opaque, forward-declared reference: `mce_input` registers the resource pointer at boot (`app::set_input_source(const input::InputState*)`). Core holds the reference; it cannot touch a field of it. This keeps the include direction input → core while still giving the shell type-level identity (ARCHITECTURE contract table, F2 row).

**Boot failure path (FR-SHP-010/040 acceptance):** when initialization fails before any window or console exists — missing GPU/GL context, SDL init failure, corrupt install — `run()` reports through `SDL_ShowSimpleMessageBox` (the one sanctioned pre-window SDL call, inside `mce_platform`), writes the rotating log, and returns a nonzero exit code to the shell. A shipped build never dies silently; the host double-click test (artifact B) asserts the message box + log pair on a deliberately broken install.

### 2.2 State stack (FR-RUN-030)

```cpp
// mce/state_stack.h  (L0 — generic; ecs-gameplay instantiates it with a World handle)
template <class StateT>
class StateStack {
public:
    void push(UniquePtr<StateT> s);    // previous state paused (on_pause), new gets on_enter
    void pop();                        // deferred to end of tick — never pops mid-on_tick
    void replace(UniquePtr<StateT> s); // pop + push, one transition event
    StateT* top();
private:
    SmallVector<UniquePtr<StateT>, 4> stack_;
    // deferred transitions applied at the frame boundary; the entity-leak assert
    // (FR-RUN-030 acceptance) lives in the ecs-gameplay wrapper, not here.
};
```

States are `struct BootState : app::SceneState { Result<void,Error> on_enter(...) override; ... }`. Core defines the mechanism; the runtime defines the states.

### 2.3 Allocators

```cpp
// mce/memory/allocator.h  (L0)
namespace mce::mem {

using usize = size_t;
struct Allocation { void* ptr; usize size; };  // tagged so free() needs no header read

class IAllocator {   // the ONLY allocation interface inside engine code
public:
    virtual Allocation allocate(usize size, usize align = 16) = 0;
    virtual void        free(void* ptr) = 0;
    virtual void        reset() {}                   // arena-only
    // debug/tracking:
    virtual usize       used() const = 0;
    virtual const char* name() const = 0;
protected: ~IAllocator() = default;
};

IAllocator& heap();                  // CRT malloc wrapper — the ONLY path to malloc in engine code
IAllocator& frame();                 // thread-local current frame-slot arena (§3.1)

// RAII scope for mid-frame scratch that must not outlive the statement block:
class ArenaScope {                   // marks, resets on destruction — MCE_ASSERT(used==mark)
public: explicit ArenaScope(IAllocator& a = frame()); ~ArenaScope();
};

template <class T> class Pool {      // fixed-capacity free-list pool (voices, packets, chunks)
public: explicit Pool(IAllocator& backing, usize capacity);
    T*     acquire();                // O(1), intrusive free list
    void   release(T* p);
    usize  live() const;
};

// cook tool LIFO arena (ROADMAP M2) — bump + scoped pop via LifoMarker; TLSF deferred
// behind a flag to M5a (ARCHITECTURE §10.1 core row).
class LifoArena : public IAllocator { /* ... */ };
}
```

Rules that have the force of review+CI:

- Engine APIs **never** allocate internally; containers and APIs that need memory take `IAllocator&` (Bitsquid "no hidden allocations" rule). A clang-tidy custom check flags `new`/`malloc` inside `mce_*` sources outside `mce::mem`.
- `std::vector`/`std::string`/iostreams are confined to tools (`mce_cook` CLI, editor glue) — CI include-grep on engine targets.
- Hot-path rule (NFR-F-03): per-frame allocations come only from `mem::frame()`; debug builds assert no `heap()` call inside a tick/render zone (a scoped `mem::NoHeap` guard that swaps in an always-fail allocator).

```cpp
// mce/memory/handle.h  (L0) — generational-index handles (Bitsquid ID lookup table)
template <class Tag> struct Handle { u32 index = 0; u32 generation = 0; };

template <class T> class HandleTable {          // sparse + dense + generations
public:
    Handle<T>        insert(T value);           // O(1) amortized
    bool             is_alive(Handle<T> h) const;          // generation check
    T*               try_get(Handle<T> h);      // nullptr on stale — never aliases
    const T*         try_get(Handle<T> h) const;
    void             erase(Handle<T> h);        // bump generation, dense swap-remove
    Span<const T>    dense() const;             // iteration is cache-linear
};
```

Used by `assets` (GPU/asset refs), `audio-input-ui` (voice handles), and as the *only* legal cross-frame reference type (binding). Entity handles are `ecs-gameplay`'s own instantiation of the same pattern.

### 2.3.1 Result & Error

```cpp
// mce/result.h  (L0)
namespace mce {

enum class Errc : uint16_t {      // per-module ranges, one enum, u16
    // 0x0000–0x00FF core       // 0x0100–0x01FF platform/fs
    Unknown=0, OutOfMemory, AssertFailed,
    FileNotFound, AccessDenied, MmapFailed,
    // 0x0200 assets: BadMagic, CrcMismatch, VersionTooNew, Truncated, MissingRef...
    // 0x0300 rhi:  ShaderCompile, PipelineCreate, ...
    // 0x0400 audio / 0x0500 physics / 0x0600 scene ...
};

struct Error {
    Errc        code;        // stable, serialized into crash/save diagnostics
    const char* stage;       // "cook.mesh.import", "rhi.pipeline.create", ...
    FixedString<256> path;   // asset path / file — FR-DX-050: code + path + stage
    uint32_t    detail = 0;  // line/offset, GL error, OS error code
};

template <class T>
class [[nodiscard]] Result {    // C++20: no std::expected; ~80 lines, union + tag
public:
    static Result ok(T v);  static Result err(Error e);
    explicit operator bool() const;
    T&       value();       // MCE_ASSERT(has_value()) — programmer error path
    Error&   error();
    T        value_or(T def);
};
using ResultV = Result<void, Error>;   // void specialization: carries only Error

#define MCE_TRY(expr)      auto _r = (expr); if (!_r) return Err(_r.error());
#define MCE_TRYVA(...dest, expr) /* assign-and-propagate variant */
}
```

Policy (binding): expected failure (missing file, bad magic, stale handle) ⇒ `Result`. Programmer error (null deref, broken invariant) ⇒ `MCE_ASSERT` + minidump. Never exceptions (`/EHs-c-`, `_HAS_EXCEPTIONS=0`). Every user-visible failure is one of these two categories — FR-DX-050's milestone audit greps for bare `return false;` in fallible engine paths.

### 2.4 Logging, asserts, convars

```cpp
// mce/log.h — spdlog facade (spdlog is boilerplate, not architecture: not on the from-scratch list)
#define MCE_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define MCE_LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define MCE_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)   // + editor console ring + crash breadcrumb
// Hot-path rule: no MCE_LOG_* inside per-entity/per-draw loops; per-frame summary instead.
// Shipped builds: SPDLOG_ACTIVE_LEVEL=warn, sinks = file only.

// mce/assert.h
#define MCE_ASSERT(cond)   /* debug: log+break+minidump+__fastfail; release: compiled out */
#define MCE_VERIFY(expr)   (expr)  /* always evaluated; breaks only in debug */
#define MCE_DEBUG_BREAK()  __debugbreak()

// mce/convar.h
struct ConVar {
    const char* name; const char* help; uint32_t flags; // Cheat|Saved|ReadOnly|RequiresRestart
    // typed accessors over a small variant; change notification via a fixed callback slot
    static ConVar* find(StringId name);
    static Span<ConVar* const> all();     // editor console + shipped debug console (FR-UI-045)
};
#define MCE_CVAR(name, default, help, flags) /* static registrar */
#define MCE_CONFUNC(name, fn, help)          /* callable console function */
```

Convars are registered by static registrar objects (no init-order games: the registry is a `FlatMap` populated during static init before `main` runs user code). `time_scale`, `r.vsync`, `sim.max_catchup`, `headless`, `audio.device_latency_ms` are convars from day one.

### 2.5 Platform seam (`mce_platform`)

```cpp
// mce_platform/window.h (L1) — the only SDL_Window/SDL_Event touchpoint
namespace mce::platform {

struct WindowDesc { const char* title; u32 width, height; bool resizable, high_dpi; };

enum class WindowMode : u8 { Windowed, BorderlessFullscreen, ExclusiveFullscreen };

struct DisplayMode { u32 width, height; u32 refresh_hz; };  // sorted by (pixels, refresh)

class Window {
public:
    static Result<UniquePtr<Window>, Error> create(const WindowDesc&);
    void*  native_handle() const;              // SDL_Window* — used ONLY by mce_rhi_gl + editor_layer
    uvec2  framebuffer_size() const;           // in pixels, DPI-correct (FR-R2D-080)
    void   set_title(const char*);  void request_close();
    // Window placement / display / cursor (FR-RUN-030 persistence; F-scope gap fix):
    void   set_mode(WindowMode);  WindowMode mode() const;
    void   set_size(u32 w, u32 h);             // windowed only; no-op fullscreen
    void   set_display(u32 index);             // move fullscreen to another monitor
    Span<const DisplayMode> display_modes(u32 display_index) const; // per-display sorted size@refresh list
    void   set_cursor_visible(bool);  void set_cursor_relative(bool); // FPS-cam mouse look
    void   set_clip_rect(const Rect2Di*);      // null = release capture (menu close)
    // Events are drained by app::run() via pump_events(); consumers register callbacks.
    using EventFn = void(const Event&);
    void   set_event_handler(FunctionRef<EventFn>);
};

enum class EventType { Close, FocusIn, FocusOut, Minimized, Resized, KeyDown, KeyUp,
                       MouseMove, MouseButton, MouseWheel, TextInput, GamepadButton,
                       DisplayAdded, DisplayRemoved, ScaleChanged, ... };
struct Event { EventType type; u32 device_id; u32 code; f32 x, y; u64 timestamp_us; };

int  pump_events();          // dispatches through the handler; returns quit-requested
double now_seconds();        // std::chrono::steady_clock — the ONLY wall clock in the engine
u64   now_us();
void  sleep_ms(u32);

// filesystem (L1)
Result<Buffer, Error>      read_file(StringView path);        // whole-file, allocator arg
Result<void, Error>        write_file(StringView path, Span<const byte>);
MmapResult                 map_file_readonly(StringView path); // .mce load path (FR-AST-020)
// Virtual mounts — content roots are data, not hardcoded drive letters:
//   "content://" → cooked pack dir (dev: <project>/cooked/, per assets §4.4 + ARCHITECTURE CF17
//                 where the project root is <project>.mceproj + assets/)
//   "source://" → <project>/assets/ (editor/cook only)
//   "save://" → %LOCALAPPDATA%/MCE/<game>/saves   "log://" / "crash://" similarly
Result<UniquePtr<Thread>, Error> spawn_thread(const char* name, ThreadFn, void* ctx);
Result<void*, Error>       load_library(StringView path);     // game module, RenderDoc/Tracy hooks
}
```

Design notes: `Window::native_handle()` returning `SDL_Window*` is a deliberate, single, documented leak — GL context creation lives in `mce_rhi_gl` and needs it; it is the one sanctioned `SDL_` symbol outside `mce_platform` and the CI grep has an explicit allowlist entry for it. Focus-loss/minimize events surface as `Event`s; the runtime's pause-on-focus-loss policy (FR-RUN-060) subscribes, it is not hardcoded in the pump. The hot-reload file watcher is **editor-owned** (a 500 ms poll running on this layer's IO worker, per F12/ADR-0015) — the shipped runtime contains no watcher at all, which is what keeps FR-RUN-010 true.

**Display policy (FR-RUN-030, completes the window-management surface):** the settings menu drives fullscreen/resolution through two Saved convars — `video.mode` (windowed/borderless/exclusive) and `video.resolution` (WxH@Hz, validated against `display_modes()` on apply) — applied by the runtime's `apply_settings()` path calling `set_mode`/`set_size`/`set_display`; the convars are the persistence layer, the Window API is the only mutator, and there is no second settings pathway. **Display-removal fallback:** on `DisplayRemoved`, if the window's display is gone, platform moves it to the primary display and drops to `Windowed` at its last windowed size (never leaves a game stranded on a dead monitor); on `ScaleChanged` the DPI event forwards to rhi-renderer's resize path (FR-R2D-080). Unplugging during exclusive fullscreen is the acceptance case for the FR-RUN-030 persistence tests.

### 2.6 Jobs (L0 — serial inline from M0, parallel behind a flag at v0.5/M5a)

```cpp
namespace mce::jobs {
    using JobFn = void(void* ctx);                 // no captures, no exceptions
    struct Counter { std::atomic<u32> v{0}; };
    void init(u32 workers);  void shutdown();
    Counter* run(JobFn* fn, void* ctx, Counter* depends = nullptr);
    void     wait_for(Counter* c);                 // workers help while waiting
}
```

Counter/fork-join per the Destiny GDC 2015 job-system model; a mutex-guarded deque is explicitly fine at MCE's projected job count (research §3.4). Until the runtime enablement flag turns on (v0.5, ROADMAP M5a, gated by the determinism CI test), `run` executes inline — call sites are written once, parallelism is a swap, not a migration. No fibers, ever, absent a profiling surprise.

### 2.7 Math (L0) — conventions and surface

```cpp
namespace mce::math {
    // BINDING CONVENTIONS (single source of truth, asserted by the oracle tests):
    // right-handed · Y-up · meters · radians · column vectors · column-major storage
    // GL clip space: x,y ∈ [-1,1], z ∈ [-1,1]  (glDepthRange 0..1 mapping lives in mce_rhi)
    struct vec2 { f32 x, y;   /* + constexpr ops, no hidden malloc, trivially copyable */ };
    struct vec3 { f32 x, y, z; };
    struct vec4 { f32 x, y, z, w; };
    struct quat { f32 x, y, z, w; };          // w-last storage, Hamilton, normalized invariants
    struct mat4 { f32 c[4][4]; };             // c[column][row] — column-major, GL-ready layout
    mat4  mat4_identity();  mat4 mul(mat4 a, mat4 b);          // scalar reference impl
    mat4  perspective_rh_zo(f32 fov_y_rad, f32 aspect, f32 zn, f32 zf);
    mat4  look_at_rh(vec3 eye, vec3 at, vec3 up);
    mat4  trs(vec3 t, quat r, vec3 s);   mat4 inverse(mat4);   // full inverse + fast rigid inverse
    f32   dot(vec3, vec3);  vec3 cross(vec3, vec3);  quat slerp(quat a, quat b, f32 t);
    struct aabb2 { vec2 min, max; };  struct aabb3 { vec3 min, max; };  // min<=max invariant
    // Scalar everywhere. SIMD lives only in clearly-marked hotspot TUs (§3.4), and
    // each intrinsic function keeps its scalar twin which oracle-tests it (FR-PHY-070).
}
```

`vec2/3/4`, `mat4`, `quat`, `aabb2`, `Color` are on the FR-DX-010 12-type gameplay budget; everything else in `mce::math::detail`.

### 2.8 `mce_refl` (L1) — the sole metadata source (F4/ADR-0009)

```cpp
// mce_refl/type.h (L1)
namespace mce::refl {

enum class FieldKind : u8 { F32, F64, I32, U32, U64, Bool, String, Enum,
                            Vec2, Vec3, Vec4, Quat, Color,
                            AssetRef, EntityRef, Struct, FixedArray };

struct FieldRecord {
    StringId   name;  FieldKind kind;  u32 offset;  u32 count = 1; // count>1 = fixed array
    StringId   type_id;                    // Struct/Enum/AssetRef: resolved via find_type
    const char* help;  u32 flags;          // ReadOnly | Transient | EditorRange...
    f32 editor_min, editor_max;            // drag limits; optional
};

struct TypeRecord {
    StringId id;  const char* name;  u32 size, align;
    Span<const FieldRecord> fields;
    const TypeRecord* base = nullptr;      // flattened plain structs only — no inheritance
    // trait slots, registered alongside: serialize_fn, inspect_fn, hash_fn, display_fn
};

const TypeRecord*            find_type(StringId id);
Span<const TypeRecord* const> all();
// Property paths ("sprite.tint", "body.velocity.x") resolve against a TypeRecord —
// the addressing scheme shared by the inspector, undo diffs, saves, and mce::ui binding.
}

// Registration — reflected plain structs, zero per-type editor/serializer code (FR-DX-020):
#define MCE_COMPONENT(T, ...) \
    /* static registrar: builds the FieldRecord table via offsetof at compile time, \
       registers into the global registry before main, and exposes mce_refl traits */
```

Design intent: a *generic* registry with no knowledge of entities, scenes, or the inspector. `MCE_COMPONENT` folds field descriptors from a `constexpr`-friendly macro expansion (one `FieldRecord` per declared field) into a static registrar object — the same pattern as convars, so there are no init-order games. Consumers attach behavior via trait registrations: `ecs-gameplay` registers components as reflected types, `assets` uses `serialize_fn` for scene/save chunks, `editor-tools` builds the inspector and undo diffs on `FieldRecord` + property paths, `audio-input-ui` persists rebinding through the same path. The full trait vocabulary is designed with its first consumer (ECS registration, ROADMAP M1); this doc fixes only the module boundary, the registration pattern, and the invariant that **no other metadata mechanism may exist** (no ad-hoc field lists, no hand-written serializers — review rule).

**Consumer acceptance matrix** (each consumer's PR gate — what "reflection works" means per consumer, so no consumer ships its own private dialect):

| Consumer | Acceptance test (their test suite) | Trait surface consumed | Tier |
|---|---|---|---|
| `ecs-gameplay` | 3-field sample component registers; `try_get`/`set` round-trip through reflection with zero editor code (FR-DX-020) | registration, `TypeRecord`/`FieldRecord` | M1/M2 |
| `assets` (scene cook) | reflection-driven scene schema test: new reflected component → cook + inspector handle it, zero serializer changes (assets §10) | `serialize_fn`, field table | M2 |
| `editor-tools` | inspector renders + round-trips edits; undo diffs are property-path deltas (FR-EDT-020) | field table, `EditorRange` flags, display trait | M2–M3 |
| `audio-input-ui` | rebind persistence round-trips through the same save path (§2.1 there) | `serialize_fn` | M3 |
| save system (§2.9) | `[Save]`-flagged field dump/load round-trip; unknown-field tolerance across `module_version` | field table, `Transient` flag inverse | M6 |

A consumer failing its row blocks the milestone the same way the shared tests do — reflection is one mechanism, so its acceptance is per-consumer but never per-consumer-reimplemented.

### 2.9 Save/persistence (`SaveWriter`/`SaveReader`, FR-RUN-065)

Core owns the file *mechanics* of saving; `ecs-gameplay` owns what a save *is* ([Save]-flagged reflection dump, its §2.13). This fixes the contract so no two subsystems invent file formats:

```cpp
// mce/save.h (L0) — mechanics only; payloads are opaque byte spans
namespace mce::save {

Result<void, Error> write(const char* save_name, Span<const byte> payload);  // save://<name>.mcesave
Result<Span<const byte>, Error> read(const char* save_name);                 // mmap, read-only

}
```

- **Atomicity:** write to `save://<name>.mcesave.tmp`, flush, then `MoveFileEx(REPLACE_EXISTING)` rename — a crash mid-save leaves the previous save intact. One `.bak` of the prior save is kept (rotate on successful write).
- **Mount:** `save://` resolves to `%LOCALAPPDATA%/MCE/<game>/saves` (§2.5); per-slot subdirectories are the caller's naming convention, not engine policy.
- **Payload contract:** a single versioned byte span — `ecs-gameplay`'s header (`save_format_version, module_version, scene GUID, tick`) plus its `[Save]` reflection dump. Core never parses it.
- **Settings vs saves:** convar/settings persistence (options menus) uses the same writer with a well-known `settings` name — one code path, no INI reimplementation.
- **Tier:** the writer ships at M6 with the first game save (v0.5); the mount and function signature exist from M1 so call sites are written once.

---

## 3. Internal design: data structures & algorithms

### 3.1 Memory map of a frame (the load-bearing diagram)

```
 ┌────────────────────────── per process ──────────────────────────┐
 │  CRT heap (mem::heap) ── setup-time only: registries, container  │
 │  growth during init, editor session state, cook file tables      │
 │                                                                  │
 │  Persistent arenas ── one per lifetime class:                    │
 │    level arena (reset on scene change) · audio arena ·           │
 │    asset arena (mmap-backed, owned by mce_asset)                 │
 │                                                                  │
 │  Pools: Pool<AudioVoice> · Pool<RenderPacket> · chunk pool ←─────┼── owned by
 │  (consumers: audio, render, cook)                                │   subsystems
 └──────────────────────────────────────────────────────────────────┘
 ┌──────────────────── per frame slot (× MCE_FRAMES_IN_FLIGHT = 3) ─────────────┐
 │  frame arena: 16 MiB bump allocator, thread-local,                            │
 │  reset after the GPU fence for that slot signals (§4.3)                       │
 │  holds: sim command buffers, render extract lists, ImGui frame data,          │
 │  job contexts, sort keys, debug overlay strings                               │
 └───────────────────────────────────────────────────────────────────────────────┘
```

`MCE_FRAMES_IN_FLIGHT = 3` is **one engine-wide constant defined in `mce_core`** (fix F1/ADR-0005), consumed by core's frame arenas *and* the RHI's staging rings/fences. Lowering it to 2 is a measured convar change requiring a one-page ADR and both docs updated. The frame-slot ring is the same ring discipline as the RHI's persistent-mapped staging buffers — slot N's CPU memory is only reusable once frame N−3's fence has signaled. Core owns the slots' CPU half (the arena); the renderer owns the GPU half and signals release. One mental model for "when is it safe to overwrite last frame's memory" across the whole engine (Gregory ch. 6 frame allocator; the `glFenceSync`/`glClientWaitSync` half lives in rhi-renderer).

Frame allocator internals: bump pointer + alignment mask + canary; `allocate` is ~4 instructions; overflow trips `MCE_ASSERT` and logs the largest request; debug builds fill reset memory with a `0xCD`-style pattern to surface use-after-reset. Tracy hooks (`TracyAllocN`/`TracyFreeN`) wrap every arena so the Memory track shows per-slot real behavior from M0 (FR-DBG-010).

### 3.2 Containers — why not STL

Rule of thumb (Our Machinery corpus): the engine's own containers exist for *allocation control and deterministic behavior*, not cleverness.

| Container | Backing | Policy |
|---|---|---|
| `Array<T>` | growable, allocator-aware, pow2 or 1.5× growth, `noexcept` moves required | the default sequence type in engine code |
| `SmallVector<T, N>` | inline cap N then spills | small per-entity/system scratch; frame-alloc backed |
| `FixedArray<T>` | non-growing, arena-backed | hot paths, SoA columns |
| `HashMap<K,V>` | open addressing, linear probe, power-of-two, robin-hood displacement, 1-byte pseudoslot | the engine map; `std::unordered_map` never appears in engine code (CI grep) |
| `FlatMap<K,V>` | sorted `Array` | small maps, serialization-friendly, **deterministic iteration** (FR-RUN-040) |
| `BitSet<N>` | word array | ECS component masks live in mce_ecs's own bitset, not this one |
| `RingBuffer<T>` | SPSC, power-of-two, acquire/release atomics | the audio command FIFO (FR-AUD-010), log fan-out |
| `MpScQueue<T>` (F10) | bounded: **Vyukov MPMC ring** (per-cell sequence numbers, CAS enqueue/dequeue) used as MPSC; unbounded variant: intrusive singly-linked list with an atomic head + dummy node, nodes from the producer's allocator | the many-producer→one-consumer seams: editor event pump, log fan-out, asset commit queue (F10) |
| `StringId` | u64 FNV-1a hash; optional intern table on the heap | all named lookups (convars, events, asset aliases) |
| `FixedString<N>` | inline char array, length-prefixed | `Error::path`, component names — no heap in diagnostics |

All containers: no exceptions (`_HAS_EXCEPTIONS=0`), no throw paths; out-of-memory in a hot container is an assert, in a fallible path is a `Result`. Iteration order of every core container is deterministic given the same operation sequence — an inherited requirement of the ECS determinism test, so `HashMap` iteration order must never leak into sim-visible behavior (caches yes, update order no).

`MpScQueue` design notes (bounded Vyukov form): `N`-power-of-two array of `{ std::atomic<u64> seq; T data; }`; enqueue loads `seq`, computes the cell's expected position, CASes `seq` from `pos` to `pos+1` to claim; dequeue likewise consumes. SPSC-fast path degrades gracefully to MPSC; the consumer is always the main thread. The unbounded intrusive variant never frees nodes on the consumer side — nodes are frame-arena-allocated per batch and retired wholesale, which is why it is only legal for "drain-every-frame" patterns. F10 fixed the container gap where `editor-tools` posts events from ≥2 threads against a core list that only had SPSC.

### 3.3 Handle tables

Implementation = Bitsquid's ID lookup table: three arrays — `sparse[]` (maps handle index → dense slot, sized to max handles), `dense[]` (live entries, packed), `generation[]` (bumped on erase). Lookup: `sparse[h.index] → dense slot; dense_slot.generation == h.generation ? &data[...] : nullptr`. Stale handles fail, never alias (FR-ENT-010's acceptance test is also a core unit test for the generic table). u32 generation wraparound is asserted-and-renumbered (at 60 Hz that is ~4,000 years; the assert is documentation).

### 3.4 Math internals

- **Scalar-first, `constexpr` everywhere.** Small structs, value semantics, no virtual, no heap. All functions inlined; MSVC autovectorizes the scalar loop bodies that matter (matrix streams, SoA physics loops).
- **Storage beats instruction selection** (Agner Fog; Lemire): SoA `FixedArray<f32>` blocks in physics broadphase and audio mix loops; AoS `vec3` only at API boundaries.
- **SIMD hotspots, written only when Tracy shows the cost** (research §4.2): batched 4×4 matmul (skin/bone palettes, M2+), sprite-batch transform blocks, broadphase AABB sweeps, audio mix loop. Each hotspot file pairs `*_simd.cpp` with a scalar `*_ref.cpp` and the test suite compares them across randomized sweeps — the scalar path is the oracle for the SIMD path, and `/fp:precise` builds both. SSE4.2 baseline; no `/arch:AVX` global flag; AVX2, if ever, behind a `__cpuid` check.
- **Floating-point policy:** `/fp:precise` in **every** configuration — there is no `/fp:fast` build and no per-file pragma to maintain (finding 26: the pragma-per-file scheme was one forgotten pragma away from a determinism bug; uniform precision removes the failure class). NFR-F-04 is satisfied by construction, and the determinism preset is now just a named CI alias for release.
- **Conventions are asserted, not documented:** the oracle suite constructs GL-NDC projections and compares against DirectXMath's RH variants with tolerance *in meters/radians*, catching the z-range and handedness bugs at their single point of definition.

### 3.5 Logging, breadcrumbs, crash plumbing

- spdlog with 4 sinks: `OutputDebugStringA` (dev), stdout console (dev), rotating file under `%LOCALAPPDATA%/MCE/logs` (all builds), and a bounded SPSC ring (1024 entries) that the editor console panel drains each frame (FR-DBG-040).
- Every `MCE_LOG_ERROR` appends to a **breadcrumb ring** (last 64 log lines, thread-safe) held in core; the crash handler flushes it into the minidump's user stream. A crash without context is a half-debugged crash.
- Crash handler (research §6.3): `SetUnhandledExceptionFilter` + terminate/purecall/invalid-parameter handlers → `MiniDumpWriteDump` (`MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData`) to `%LOCALAPPDATA%/MCE/crashes/<exe>_<buildid>.dmp`, build ID written to a comment stream; PDBs archived per CI build keyed by the same ID (FR-DBG-030). WER suppressed. In shipped games, save-flush runs *before* the fastfail (FR-RUN-060: no silent corrupt-save paths).

### 3.6 Configuration

Convar registry is the single configuration surface for the engine (a shipped game's *user settings* go through reflection-serialized saves in ecs-gameplay/audio-input-ui land — core does not parse config files at runtime). Command line is parsed into convars before `Application::on_init` (`-cvar value`, `+confunc args`). Editor-side persistence (layout, recent files) is editor-tools' business.

---

## 4. Threading & lifecycle

### 4.1 The thread roster (matches ARCHITECTURE §6.1 — five steady-state threads, ever)

| Thread | Owner | Lifetime | Rules |
|---|---|---|---|
| **Main** | core | process | Owns window, GL context, ECS, editor, document/undo. Sim + render both run here through v1.0. The only thread that mutates registries, asset tables, or scene structure. |
| **Audio (SDL driver cb)** | SDL3 → `audio-input-ui` | device open→close | RT rules (Doumler): no locks, no alloc, no syscalls; SPSC FIFO in, mix out. Core provides the `RingBuffer` and thread priorities; the mixer is audio-input-ui's. |
| **IO worker** | `mce_platform` | app | One thread; completion queue (SPSC); runs cook steps (M2+), music page prefetch, and the editor's 500 ms hot-reload poll (F12/ADR-0015). No GL, no ECS. |
| **Job workers ×N−1** | core `jobs` | flag-gated, v0.5 (M5a) | Fork-join with counters only; no locks across job boundaries; frame-arena contexts. **Serial inline implementation until enabled** so call sites are written once. |
| **Cook worker** | `assets` (via job system) | during cook | One thread at MVP; posts progress/errors to the main thread over `MpScQueue` (F10). |

Total steady-state thread count ≤ 5. Everything else is main-thread; every non-main thread's frame-allocator slot is separate (thread-local arenas make "which arena am I on" a non-question). Cross-thread communication is exclusively: SPSC `RingBuffer`, `MpScQueue`, atomics (audio snapshots, stream `filled_upto`), and frame fences. No mutex is ever held across a subsystem boundary. Module code (`game.dll`) never leaves the main thread (the auto-parallel experiment was not merged, REQUIREMENTS N17).

### 4.2 Frame lifecycle (single frame, step by step)

1. `FrameMark` (Tracy). Acquire frame slot `N mod MCE_FRAMES_IN_FLIGHT`; if its fence hasn't signaled, wait on it (renderer-provided callback; core knows only "wait until releasable").
2. Reset slot arena (16 MiB budget; debug: fill + canary check).
3. `pump_events()` → event queue → `mce_input` action mapping update.
4. Fixed-step loop: `input::sample_actions(tick)` → `on_tick` × catch-up steps (each tick's scratch from the frame arena; ECS command buffers flushed at tick end by mce_ecs).
5. `on_render(alpha)`: extract → RHI translate → submit (rhi-renderer).
6. Editor (if present) drains its queues: console ring, reload requests, Tracy panels.
7. Signal slot fence; present; frame summary log (1 line, per FR-DBG-040's no-hot-path rule).

### 4.3 Frame-slot release protocol across the RHI seam

Core defines:

```cpp
// mce/frame_slots.h
struct FrameSlotFence { virtual ~FrameSlotFence() = default;
    virtual bool is_signaled() = 0; virtual void wait_blocking() = 0; };
mem::IAllocator& acquire_frame_slot(UniquePtr<FrameSlotFence> fence); // fence owned by rhi-renderer
```

The renderer registers a GL-fence-backed implementation at startup. Core depends on the *abstract* fence only — this is the exact seam that keeps `mce_core` free of GL while still being the allocator of record. In headless mode the fence is a no-op always-signaled impl, and the determinism harness (FR-RUN-025) runs the identical loop with zero GL symbols loaded (CI-verified by GL-leak grep).

### 4.4 Application lifecycle order (hosts differ only in steps 5–7)

```
main() [game.exe / mce_editor.exe / mce_cook.exe / mce_devhost.exe]
 ├─ crash handler install (first line, before anything can crash usefully)
 ├─ convar registration (static init already ran), command-line parse
 ├─ mem::init (frame slots, pools) · jobs::init (serial) · log sinks · platform::init (SDL3)
 ├─ subsystem inits in reverse dependency order:
 │    assets → audio device → rhi/rhi_gl → ecs+world → (editor_core → editor_layer)
 ├─ Application::on_init → push boot scene state
 ├─ main loop (§2.1)
 └─ shutdown in forward order: editor → world → rhi (flush fences) → audio device → assets
    → platform → save-flush (FR-RUN-060) → mem::verify_no_leaks (debug) → exit code
```

All four hosts link `mce_engine.dll` (the L0–L4 umbrella, F8): one CRT, one engine instance, no duplicated globals. Init is deliberately *explicit and linear*, not a service-locator: MCE has exactly one composition point per host, and the ordering above is the documentation. This is the Handmade Hero platform/game split kept literal — each host `main()` is ~150 lines and is the only place subsystems meet.

### 4.5 Editor-specific lifecycle notes

The editor is a `mce::app::Application` whose `on_tick` advances the *preview world* only in play mode (REQUIREMENTS A11); its canonical scene document lives in editor-tools (`mce_editor_core`). Core's obligations: the editor runs the same loop, same frame slots, same allocators — no second loop, no editor-specific scheduling (P6: one path per concern).

---

## 5. Upstream/downstream dependencies

### 5.1 This layer provides (consumed by every subsystem above L0/L1)

- Memory: `mem::IAllocator`, `mem::frame()`, `mem::heap()`, `Pool<T>`, `LifoArena`, `ArenaScope`, `HandleTable<T>`/`Handle<Tag>`, the `MCE_FRAMES_IN_FLIGHT` slot protocol.
- Containers: `Array`, `SmallVector`, `FixedArray`, `HashMap`, `FlatMap`, `BitSet`, `RingBuffer` (SPSC), `MpScQueue` (F10), `StringId`, `FixedString`.
- Math: `mce::math` (all types + ops), float-point policy, hotspot SIMD pair pattern.
- Errors/diagnostics: `Result<T,Error>`, `Errc`, `MCE_ASSERT`/`MCE_VERIFY`, `MCE_LOG_*`, breadcrumbs, crash handler + minidump, build ID.
- App shell: `app::Application`, `run()` main loop, `StateStack<T>`, `TickContext`/`RenderContext`, headless mode, frame-slot fences.
- Config: convar/confunc registry.
- Reflection: `mce_refl` — `TypeRecord`/`FieldRecord`, `MCE_COMPONENT`, property paths (sole metadata source, F4).
- Jobs: `jobs::run/wait_for` (serial inline; cook is the first customer at M2, runtime enablement v0.5).
- CRC32/xxhash64 (used by `assets` determinism and `.mce` CRC), endian-safe read/write helpers.

### 5.2 Platform provides (consumed by the listed subsystems)

- `platform::Window`, event pump → rhi-renderer (GL surface + resize events), editor-tools (viewport widgets), audio-input-ui (raw device events only).
- `platform::filesystem` (`read_file`, mmap, mounts, save/log/crash dir resolution) → assets (cook + runtime load), audio-input-ui (streamed music mmap), ecs-gameplay (saves via reflection serialization).
- Threads/TLS/`load_library` → audio-input-ui (device thread), editor-tools (watch poll on the IO worker), scripting (`game.dll` copy-and-load), Tracy/RenderDoc payload hooks.
- SDL3 exclusively behind `mce_platform`; the grep wall (FR-INP-010) is the contract.

### 5.3 This layer requires (upstream — deliberately tiny)

- **Nothing from other MCE subsystems.** Core/platform/refl are the root of the DAG.
- External: C++20 STL subset (no exceptions, no iostreams in engine code), spdlog, Tracy client, SDL3 (platform only), dbghelp (crash handler), DirectXMath (test project only). The approved-list constraint binds hardest here: anything new in `mce_core`'s CMakeLists needs an ADR first.

### 5.4 Downstream dependency summary (who includes this layer, and for what)

| Subsystem | Includes core for | Includes platform for |
|---|---|---|
| rhi-renderer | allocators, handles, math, Result, frame-slot fence | window native handle (GL surface only) |
| ecs-gameplay | containers, arenas, handles, Result, StateStack, refl (component registration) | filesystem (scene save via assets) |
| physics | math (SoA blocks), frame allocator, FixedArray | — |
| assets | Result, CRC, mmap, LifoArena, StringId, jobs (cook), refl (scene/save serialization) | filesystem, threads |
| editor-tools | everything L0 (convars, logs, frame slots for ImGui, refl for inspector/undo, MpScQueue) | window, event pump, IO worker |
| audio-input-ui | RingBuffer, Pool, convars, refl (rebinding persistence) | SDL audio device, threads, clock |
| scripting | jobs, arenas, Result, `load_library` (surface only) | threads |

---

## 6. Test strategy

Core is the cheapest place to be rigorous, so this layer carries the heaviest unit coverage (doctest, per research §7):

1. **Math oracle (M0, the load-bearing test).** Every `mce::math` function vs DirectXMath over randomized sweeps; tolerances in meters/radians; GL-NDC projection construction asserted here. Runs in Debug *and* the `/fp:precise` determinism build.
2. **Allocator tests (M0).** Frame arena: alignment, overflow assert, canary detection, reset-to-mark (`ArenaScope`), multi-slot ring with a mock fence; thread-local slots hammered from 4 threads (TSan via the weekly clang-cl job). Pool: exhaustion, O(1) acquire/release, Tracy hook no-throw.
3. **Container tests (M0).** `HashMap` probe/storm/iteration-determinism (same seed ⇒ same order), `RingBuffer` SPSC with a writer/reader thread pair and sequence checking, **`MpScQueue` multi-producer hammer test** (N producers, one consumer, no lost/duplicated events, bounded-variant backpressure semantics), `HandleTable` staleness/aliasing/wraparound, `FlatMap` round-trip through serialization (feeds the `.mce` tests in assets).
4. **Result/Error tests (M0).** Every `Errc` round-trips through the editor console formatting; nested `MCE_TRY` propagation preserves the *first* (root) error's code+path.
5. **Main-loop tests (M1).** A synthetic-clock `Application` (no SDL): tick grid is exactly 1/60 s under simulated 30 fps and 240 fps render rates (FR-RUN-020 acceptance); catch-up clamp never exceeds 5; alpha sequence monotonic; `time_scale=0.25` preserves tick count ratio (FR-RUN-050); headless mode runs 10k ticks with a GL-symbol-leak grep over the linked binary (FR-RUN-025).
6. **StateStack tests (M1).** push/pop/replace transitions, deferred-pop semantics, pause/resume, entity-leak assert exercised via the ecs-gameplay wrapper.
7. **`mce_refl` tests (M2 — mce_refl lands with the reflection milestone, finding F4).** `MCE_COMPONENT` registration produces the expected field table (offsets via `offsetof` verified against the struct); property-path resolution on nested/fixed-array fields; two independently-registered types cannot collide on `StringId` (hash-collision assert). Round-trip *serialization* itself is owned by assets, which tests through this registry.
8. **Crash handler test (M1, manual + CI-adjacent).** A `--crash-test` flag in `mce_devhost` writes a real minidump; CI asserts the dump file exists and embeds the build ID (symbolication itself is a documented manual step with the archived PDBs).
9. **Sanitizers per binding.** ASan smoke suite per PR over the allocator/pool/queue tests (the highest-UAF-risk code in the engine); clang-cl UBSan weekly.

Golden-image and determinism tests belong to rhi-renderer and ecs-gameplay respectively; this layer's contribution to determinism is the fixed-dt loop, the uniform `/fp:precise` configuration, and deterministic container ordering — all unit-tested here so upstream tests inherit the guarantee.

---

## 7. Risks & mitigations

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | **SDL3 vcpkg port maturity** — SDL3 is young on vcpkg; a pin may be missing/broken | Medium | High (blocks M0) | Pin the exact port version in `vcpkg.json`; fallback ADR pre-drafted: vendor SDL3 under `third_party/` (one CMake target). The window/event seam is thin (~500 lines), so a mid-project SDL patch release is absorbed, not migrated. |
| R2 | **Unity-build collisions** across core translation units (static names, macros) | Medium | Low | Per-file self-containment rule; per-module unity targets isolate blast radius; unity build disable-able per preset (`CMAKE_UNITY_BUILD_BATCH_SIZE`). |
| R3 | **Exceptions leak from spdlog/STL paths** — `_HAS_EXCEPTIONS=0` breaks some dep headers | Medium | Medium | spdlog configured `SPDLOG_NO_EXCEPTIONS`; fmt no-throw mode; a dep that hard-requires exceptions links into tools only. |
| R4 | **Frame-slot / staging-ring race** — misordered fence signaling corrupts per-frame data | Low | High (heisenbug) | Slot acquisition is single-owner (main thread only, asserted); mock-fence unit test drives wait/reset order; Tracy frame-slot track makes waits visible; ASan + canary fill catches use-after-reset. |
| R5 | **`/fp:fast` silently applied to sim files** → determinism failure discovered late | ~~Medium~~ Eliminated by design | Medium | `/fp:fast` is banned engine-wide (§1.4): every preset is `/fp:precise`, so no pragma discipline or per-file check can regress. The `msvc-determinism` CI preset remains as a named alias that also runs the FR-RUN-040 hash test (§6.5) from M1. |
| R6 | **Convar / registrar sprawl, static-init order bugs** (now three registrar families: convars, confuncs, refl types) | Medium | Low | One registrar pattern shared by all three; explicit registry objects; no cross-registrar init dependencies; registry sorted by registration for determinism; `StringId` collision assert at registration. |
| R7 | **Minidumps without matching PDBs** — crash artifacts useless | High (if unmanaged) | Medium | Build ID embedded in dump + PDB archive per CI build from M0 (research §6.3); devhost `--crash-test` exercises the whole chain monthly. |
| R8 | **Core becomes a dumping ground** (the "engine swamp" failure of solo engines — Hazel lesson) | High | Medium | The dependency table (§1.3) is enforced by CMake targets, and any PR adding a type to `mce_core` that mentions entities, textures, or audio is rejected in review by rule. Subsystem-specific code goes to the subsystem. Reflection lives in `mce_refl`, not core, precisely to keep core's surface inside the FR-DX-010 budget. |
| R9 | **Job system introduced too early** — parallel ECS/render bugs at M0–M1 | — | — | Serial inline implementation until the v0.5 enablement flag (ARCHITECTURE §10.1 core row: M5a); the API exists from M0 so call sites are written once; parallelism is a swap, and the determinism CI test gates the flip. |
| R10 | **Windows-only platform code accretes into the runtime core** (SDL calls sprinkled) | Medium | Medium | CI grep walls (SDL_Event, GL, JPH, ImGui) from M0; `mce_platform` is the only SDL3 consumer; even `SDL_GetPrefPath` is wrapped. Portability stays theoretical but the *seam* stays real (N6 rationale). |
| R11 | **`TickContext`/`Frame` confusion** — two per-tick context types drifting apart again | Low | Medium | F3 is the ruling: `Frame` (ecs) is the context of record; `TickContext` is documented as app-shell-only and used only by non-ECS samples. A CI grep asserts no engine subsystem consumes `TickContext` outside `mce::app` and the sample ladder. |

---

## 8. MVP scope summary (what this doc fully specifies vs sketches)

Tier mapping per ARCHITECTURE §10.1: **MVP tier = core + platform + loop (ROADMAP M0–M1)**; **v0.5 = jobs runtime enablement, TLSF (M5a)**; **v1.0 = shipped console plumbing (M8, via the convar registry + FR-UI-045)**.

**Fully specified (MVP):** allocator family minus TLSF, containers (including `MpScQueue`), math conventions + oracle, `Result`/`Error`, asserts/logs/crash handler, convars, app loop + `StateStack`, `MCE_FRAMES_IN_FLIGHT` frame slots + fences, platform seam, `mce_refl` registry boundary + registration pattern, module dependency rule + CMake target walls + presets, the M0/M1 test list.
**Sketched (later tiers):** job graph parallel enablement (v0.5/M5a, serial-until-then), TLSF (v0.5, flag-gated), SIMD hotspots (profiling-gated), `time_scale` polish (v0.5), `mce_refl` trait semantics deepened with its consumers (ECS registration at M1, serialization at M2, inspector/undo at M2–M6), save writer mechanics (§2.9, M6), shipped debug console plumbing (v1.0 via FR-UI-045).
