# Architecture: Scripting & the Gameplay Authoring Model (`scripting`)

**Project:** Mesh Control Engine (MCE) · **Document class:** Architecture — subsystem
**Subsystem:** `scripting` · **Date:** 2026-10-06
**Inputs:** `docs/REQUIREMENTS.md` (authoritative), `docs/research/ecs-gameplay.md` (§11.2 sequences, §12 model comparison), `docs/research/existing-engines.md` (§2 Unity VM tax, Handmade Hero split), `docs/research/platform-engineering.md` (§1.2 compile engineering), sibling docs `core.md` (§2.5 `load_library`, §2.1 app shell), `ecs-gameplay.md` (§2.12 the committed programming model), `editor-tools.md` (§2.8 PlaySession).
**Binding context honored:** no scripting VM, no visual scripting, no coroutines in v1 (N2, FR-EDT-150); no engine-level live code hot-reload (N13 — the Handmade Hero DLL split is adopted as *architecture*, not as a live-reload promise, FR-DX-040); native C++20 components + systems are the only behavior model (committed in `ecs-gameplay.md` §2.12); no exceptions/RTTI; zero heap in hot paths; `Result<T,Error>` for fallible paths; ImGui never in the runtime; editor owns the canonical scene.

**Scope note on tiers:** MVP-tier (module contract, loader, compile/reload workflow, FSM helper `fsm.h` — FR-ENT-090, and **SequenceRunner-lite** — the ARCHITECTURE §10.1 tier table requires a minimal runner at M3 so Artifact A's scripted beats work before the full action-list) is designed to implementation depth. v0.5 (full action-list SequenceRunner with save/resume — FR-ENT-100; play-in-editor module load) gets committed shapes with lighter detail. v1.0 (TimelinePlayer — FR-ENT-110) gets a sketch only.

---

## 0. The committed model in one paragraph

A game programmer on MCE writes **native C++20 in a "game module"**: reflected plain-struct components (`MCE_COMPONENT`), plain functions registered into named phases (`MCE_SYSTEM`), and data-driven behavior tools (FSM transition tables, action-list sequences, timelines) — this is the model `ecs-gameplay.md` §2.12 already committed, and this doc owns its *packaging*. The game module is compiled as a **separate DLL target** (`game.dll`) against one umbrella engine DLL (`mce_engine.dll`), so that gameplay changes rebuild only the small module and the editor can (re)load user code at the Play boundary without relinking the editor. v1 reload semantics are deliberately modest: **recompile → press Play**; the World is rebuilt from cooked content at every Play, so no stale-state problem exists and none is engineered around. There is **no Lua, no C#, no visual scripting, no C++20 coroutines in v1** — the designer-facing surface is the reflection inspector plus the FSM/sequence/timeline data tools, and a scripting VM stays behind N2's recorded trigger (after Ashen Court, if measured recompile-iterations dominate editor time).

---

## 1. Responsibilities & non-responsibilities

### 1.1 This subsystem owns

1. **The game-module contract** — the manifest, the single exported entry point, the build-target shape (which CMake target a game project creates, which headers it may include, which flags it inherits), and the engine/version stamping that makes a stale or mismatched module a diagnosable error rather than a mystery (FR-DX-050).
2. **Module loading and unloading** — the loader used identically by the shipped runtime host (`game.exe`), by play-in-editor (`PlaySession::enter`, v0.5), and by the devhost samples; the copy-and-load scheme that sidesteps Windows file locks during recompiles.
3. **The compile/reload workflow** — the documented loop (edit → incremental DLL build → Play) and its budgets (NFR-B-02: gameplay change ≤ 10 s to next Play), including what is *explicitly not* live (N13).
4. **The FSM helper (`fsm.h`)** — FR-ENT-090: transition tables as data, timed states, entry/exit actions, registration-time validation, editor-visible live state. States remain ordinary components; the helper adds no second object model.
5. **The data-driven behavior runtimes** — `SequenceRunner` advancing closed action lists (MVP `SequenceRunner-lite` subset `{Wait, PlayAnim, PlaySound, Emit, End}`; full `wait / move_to / play_anim / emit / call / spawn` + save/resume at v0.5, FR-ENT-100); `TimelinePlayer` (FR-ENT-110, v1.0) playing cooked track/clip assets. Both are ordinary `FixedUpdate_Post` systems over ECS primitives.
6. **The authoring developer experience surface** — the module template (shipped as a v1.0-tier deliverable — ROADMAP M8 project template + new-project generator, finding F17), the sample-ladder modules' structure (FR-DX-070), the API-surface census discipline (FR-DX-010), and the "what goes in code vs. data vs. the editor" rulebook.
7. **Editor integration for user code** — the registration records (systems/phases, FSM tables, sequence defs) that the editor's pipeline view and live FSM/sequence inspection read; the guarantee that user code runs **only** inside a play session, never in the editor's preview/authoring world.

### 1.2 This subsystem explicitly does NOT own

| Concern | Owner | Boundary rule |
|---|---|---|
| The ECS object model, phases, commands, events, change ticks | `ecs-gameplay` | This subsystem's runtimes are *clients* of World/Frame; it registers no new execution mechanism and no observers (rejected for v1). |
| Component reflection mechanics (macro + constexpr traits), scene/save serialization | `ecs-gameplay` (reflection) | `MCE_COMPONENT` is defined there; this doc only specifies how a *module* uses it. Sequences/FSM state are ordinary reflected components — they get save/serialize for free, no bespoke format. |
| The scene document, play-in-editor orchestration, inspector | `editor-tools` | The editor *calls* this subsystem's loader through `PlaySession::enter`; it never links the game module statically and never registers module systems into the preview world. |
| Cook pipeline, `.mce`, timeline chunk format | `assets` | Timeline assets are cooked by `assets`; this subsystem defines only the *player* semantics over the chunk. |
| Input mapping, audio mixer, the event→FIFO bridge | `audio-input-ui` | Modules consume the `InputState` resource and emit gameplay events; they never see SDL3 or the audio thread. |
| Character/physics behavior | `physics` | `move_and_collide` and queries are `physics`' public API, consumed by module systems as plain calls (§2.7). |
| Rendering anything | `rhi-renderer` | Module code never touches the RHI; rendering reads components in `Render_Pre`. |
| Mods, plugins, shipping untrusted content | nothing (N10, N8) | The module is first-party compiled content, not a plugin API; there is no third-party module contract, ever (N8). |

### 1.3 What "scripting" means here — stated once, precisely

The subsystem name is historical; the honest name is **the gameplay authoring model**. It has exactly three authoring surfaces, tiered:

1. **Code surface (MVP):** components + systems in a game module (§2). This is where *all* behavior lives in v1.
2. **Data-with-code-hooks surface (v0.5):** FSM tables and sequences — data structures authored in C++ (or by the inspector on their components) whose *steps and transitions* are data, so they are inspectable, save-game-able, and editable without recompiling when they only reference existing hooks.
3. **Pure-data surface (v1.0):** timelines, cooked to `.mce` and editable in the editor's timeline view with zero code.

Each tier trades programmer expressiveness for author-iteration speed, and each is a *subset of the same ECS substrate*, not a parallel runtime. That is the whole design thesis: MCE gets the "scripting feels" benefits Unity gets from Mono (inspectable live state, data-authored behavior) without a VM, by making state components and making behavior tables data.

---

## 2. Public interface (API sketches)

### 2.1 The module contract — one exported symbol

```cpp
// mce/module.h — the module-facing contract (part of the ONE module umbrella header, §2.3)
namespace mce {

using EngineId = uint64_t;   // FNV-1a over git-describe + build config, baked by CMake

struct ModuleInfo {
    const char* name;            // "ember_step" — log/save tags, editor title
    EngineId    engine_id;       // must equal the host's MCE_ENGINE_ID (§2.8)
    uint32_t    module_version;  // save-format versioning (FR-RUN-065)
    void      (*install)(World& w);   // register components/systems/events/resources
};

} // namespace mce

// The ONLY symbol a game module exports. No factories, no base classes, no
// plugin registry — one pure function of nothing, describing the module.
extern "C" __declspec(dllexport)
const mce::ModuleInfo* mce_game_module(void);
```

`install(World&)` is the module's entire footprint: it registers components (binding them to reflection), events (typed queues with producer phases), systems (explicit order), singleton resources the module owns, FSM tables, and sequence defs. Everything registered is a record the editor can render (pipeline view, event-flow view — FR-DX-030, FR-ENT-030). Registration is **explicit and ordered** — no cross-TU static-init registration (the ecs-gameplay static-init rule, its risk #6), so DLL load order is a non-issue and `install` doubles as readable documentation of the game.

```cpp
// game/module.cpp — the whole surface the host sees of Ember Step
#include <mce/gameplay.h>                       // the ONE allowed include (§2.3)

static void install(mce::World& w) {
    w.register_components<PlayerMove, GruntBrain, Health, ...>();
    w.register_event<PlayerLanded>(mce::Phase::FixedUpdate_Physics);
    w.register_event<DamageDealt>(mce::Phase::FixedUpdate_Physics);
    w.register_system<player_input>();          // registration order == execution order
    w.register_system<player_move>();
    w.register_system<player_react>();
    w.register_system<grunt_fsm_run>();
    w.register_system<grunt_act>();
    w.sequences().define("intro_cinematic", kIntroSteps);   // §2.6 — lite step set at MVP, full at v0.5
}
extern "C" __declspec(dllexport)
const mce::ModuleInfo* mce_game_module(void) {
    static const mce::ModuleInfo info{
        .name = "ember_step", .engine_id = MCE_ENGINE_ID, .module_version = 1,
        .install = &install };
    return &info;
}
```

### 2.2 Host-side loader API

```cpp
// mce/module_host.h — engine side; used by game.exe's main and PlaySession::enter
namespace mce {

struct LoadedModule {
    const ModuleInfo* info;
    void*             handle;        // platform library handle (opaque)
};

// Loads, version-checks, and returns the module. Fallible by contract (FR-DX-050):
// every failure below is a Result error with code + stage + path, never a crash.
//   - Errc::ModuleNotFound      DLL missing / bad path
//   - Errc::ModuleMissingEntry  export symbol absent
//   - Errc::EngineIdMismatch    stale build against a newer engine (names both ids)
//   - Errc::ModuleInstallFailed install() returned an error (duplicate system name, etc.)
Result<LoadedModule, Error> load_game_module(StringView dll_path, World& world);

// Play-boundary unload: World destroyed first (rows freed by the ECS arena),
// then FreeLibrary; debug builds assert zero outstanding module-tagged allocations.
Result<void, Error> unload_game_module(LoadedModule& m);
}
```

Copy-and-load detail (the only Windows-specific wart, and it is load-bearing): the loader copies `game.dll` → `game_loaded.dll` (or a `game_loaded_N.dll` ring) and loads the copy, so the compiler can overwrite `game.dll` while a previous play session's copy is still mapped. `FreeLibrary` on Stop releases the copy; a debug counter asserts the ring never needs more than 2 slots (recompile between Plays).

### 2.3 The one include: `mce/gameplay.h`

Module code may include exactly one engine umbrella header, which aggregates the FR-DX-010 budget surface and nothing else:

```cpp
// mce/gameplay.h — the total visible engine for a game module:
#include <mce/core_types.h>      // Vec2/3, Quat, Mat4, Result, Handle, StringId, log macros
#include <mce/ecs.h>             // World, Entity, Frame, Commands, EventQueue<T>,
                                 // MCE_COMPONENT / MCE_SYSTEM / Phase, fsm.h (§2.5)
#include <mce/starter_kit.h>     // Sprite, LocalTransform, RigidBody2D, AudioSource, ...
#include <mce/module_host.h>     // ModuleInfo, MCE_ENGINE_ID
// NOT reachable from a module (CI include-grep on game*/sources):
//   anything SDL_, anything GL, imgui.h, nlohmann/, the renderer, the cook tool,
//   mce::detail::*, the ECS table internals.
```

Rule with teeth: `gameplay.h` is a *deny-by-default* boundary, not an allow-by-convention one — the module CMake target links only `mce_engine` (import lib) + `mce_module_api` (the flags/version interface target, §2.8), so the banned headers are not on its include path at all. Leakage is a compile error, not a review finding (same mechanism as the ImGui/GL walls, `core.md` §1.3).

### 2.4 Module rules (checked where possible, documented where not)

1. **No state outside the ECS.** No mutable statics/globals holding cross-tick state, no singletons in module code; module state lives in components or registered resources. This is what makes reload-at-Play trivially correct and save/resume possible. (Constexpr/const tables — FSM tables, sequence step arrays — are fine.)
2. **No allocation in systems.** Hot-path scratch comes from `f.scratch()`; module code never calls `new`/`malloc` (the `mem::NoHeap` debug guard from `core.md` §2.3 is active inside ticks).
3. **Handles only.** `Entity` is the only cross-frame reference; asset references are GUID/`AssetId` handles.
4. **No threads, no locks.** Module systems run on the main thread inside phases (§4).
5. **Failures are data.** Missing asset → component/event (`FailedToLoad`), not a crash; programmer errors assert with minidump. No exceptions in module builds (`/EHs-c-` inherited from the interface target).

### 2.5 FSM helper (`fsm.h`) — FR-ENT-090

State is data (an enum field inside a reflected component); the transition table is data; per-state behavior stays in systems filtered by state. The helper adds only what the enum-cascade pattern gets wrong at the 20% mark: timed transitions, entry/exit actions, ordered guard evaluation, and validation.

```cpp
// mce/fsm.h (~200 lines, header-only)
namespace mce::fsm {

// Guard/Action = plain function pointers over Frame — registered, never serialized
// (guards are code; state + stateTime are data and DO serialize).
template<class S> using Guard  = bool (*)(Frame&, Entity);
template<class S> using Action = void (*)(Frame&, Entity);

template<class S>
struct Trans {
    S        from, to;
    Guard<S> when  = nullptr;   // evaluated in table order; first true wins
    f32      after = -1.0f;     // timed transition: stateTime >= after
    Action<S> entry = nullptr;  // run on entering `to` (after stateTime reset)
    Action<S> exit  = nullptr;  // run on leaving `from` (before entry)
};

template<class S>
class Table {
public:  constexpr Trans<S>* begin(); constexpr Trans<S>* end();
         // builder: t.from(Idle).to(Patrol).after(1.5f) ... chainable, constinit-friendly
};

// The one runtime entry point. For each entity in the query: advance stateTime,
// evaluate transitions of the current state in registered order; on the first
// taken transition: exit(old) -> state = to (bumps change tick) -> stateTime = 0
// -> entry(new); optionally emit StateChanged<S>{e, from, to} if registered.
template<class S, class C> void run(Frame&, const Table<S>&, Query<S, ...>);
}
```

Registration-time validation (runs in `install()`, errors surface in the editor console, FR-DX-050): unreachable states (warning), duplicate `from+when` pairs (error — ambiguous evaluation order), `after < 0` with `when != nullptr` (error — one or the other), states referenced but never declared.

### 2.6 Sequences — action-list scripting (MVP-lite / full at v0.5, FR-ENT-100)

Steps are data; the vocabulary is **closed** (adding a step kind is a design event with a use case, not a habit — same trigger discipline as relationship pairs in `ecs-gameplay.md`):

```cpp
// mce/sequence.h
namespace mce::seq {

enum class Kind : uint8_t {
    Wait, WaitForEvent, MoveTo, PlayAnim,
    PlaySound, StopSound,            // audio-input-ui contract row: sequencing reaches audio
                                     // ONLY through the same command API (AudioDevice::play/stop,
                                     // resolved to an AudioHandle) — no second audio path
    Emit, Call, Spawn, Destroy, End
};

struct Step {
    Kind    kind;
    StringId id;            // target event name / clip name / sfx guid / registered Call id / prefab guid
    f32     a = 0, b = 0;   // Wait seconds; MoveTo speed; PlaySound gain; ease params
    Vec3    pos;            // MoveTo target (entity-relative or world, per `space`)
    Entity  entity;         // Spawn/Destroy target (placeholder remapped at define-time)
};

using CallFn = void (*)(Frame&, Entity);   // the ONLY code hook in a sequence

class Registry {                            // world-owned, populated at install()
public:
    void define(StringId name, Span<const Step> steps);  // validates: End-terminated,
                                                         // Call ids resolvable, loops bounded
    const Step* steps(StringId name) const;
};
}

// Runtime state is a small reflected component — this is what makes sequences
// save/resume-able and editor-scrubbable (FR-ENT-100 acceptance):
MCE_COMPONENT(SequencePlayer, StringId def; uint16_t step = 0;
                              f32  elapsed = 0; bool active = false; bool looping = false;)
```

`SequenceRunner` is one `FixedUpdate_Post` system: for each active `SequencePlayer`, advance the current step until it blocks (Wait elapses, WaitForEvent satisfied, MoveTo arrives), then advance; per-entity step budget per tick (assert against infinite loops — `Call` may enqueue commands but not re-enter the runner). `Call` targets a registered function table (module-registered, stable `StringId`s, part of the module contract) — sequences can trigger arbitrary module code but only through named, versioned hooks.

**Tier split (binding per ARCHITECTURE §10.1):** the **MVP `SequenceRunner-lite`** executes only the closed subset `{Wait, PlayAnim, PlaySound, Emit, End}` — enough for Artifact A's scripted beats — with no save/resume and no `MoveTo`/`Spawn`/`Call`. The full vocabulary and the save/resume machinery are the v0.5 deliverable (FR-ENT-100); the runner's state shape (`StringId def + u16 step + f32 elapsed`) does not change between tiers, so MVP sequences upgrade in place. `PlaySound`/`StopSound` resolve a GUID to an `AudioHandle` through the `AudioDevice` command API at run time — the same path every other audio consumer uses (audio-input-ui contract table), never a second audio path.

**v1.0 sketch — TimelinePlayer (FR-ENT-110):** a cooked `TimelineAsset` chunk (tracks of keyframes/clips targeting template entity slots) + a `TimelinePlayer` component binding asset→instantiation; evaluation is one `FixedUpdate_Post` system sampling tracks at `tick * dt` (deterministic, scrubbable by changing the playhead — the editor's timeline view drives the same player in a paused world). Reuses `Sequence`'s "index + elapsed is the whole state" lesson: playback position is data, so scrub = write the position.

### 2.7 The two required code sketches

> These sketches are **illustrative, non-normative** — where any name or signature here disagrees with the owning subsystem doc (`physics` §2.1–§2.2, `audio-input-ui` §2.1, `ecs-gameplay` §2), the subsystem doc wins. Field names below follow those committed contracts and are re-checked each doc revision; input consumption uses the committed `InputState` surface (fixed fields as fast paths + `StringId` readers, audio-input-ui §2.1).

**A player controller** — Ember Step's run/jump with coyote time and jump buffering (the UC-A "game feel" core), showing input consumption, the physics controller call, and event-driven reactions:

```cpp
// ==== game/player.cpp — module code ====
enum class Ground : uint8_t { Air, Ground, WallSlide };

MCE_COMPONENT(PlayerMove,
    f32  coyote        = 0.0f;     // seconds since last grounded
    f32  jump_buffer   = 0.0f;     // press-buffered jump (FR: game-feel tooling)
    Ground ground      = Ground::Air;
    int   jumps_left   = 1;
    bool  was_grounded = false;    // landing-edge tracking (MoveResult2D has no prev state)
    f32   prev_vy      = 0.0f;)    // vertical velocity going into the tick, for impact

// 1) input -> intent (buffer decay is the only per-tick state here)
MCE_SYSTEM(player_input, mce::Phase::FixedUpdate_Pre, after("input_apply"))
static void player_input(Frame& f) {
    const InputState& in = f.resource<InputState>();
    f.query<PlayerMove&>().each([&](Entity, PlayerMove& p) {
        p.jump_buffer = in.jump_pressed ? 0.12f      // fixed-field fast path (audio-input-ui §2.1)
                      : std::max(0.0f, p.jump_buffer - f.dt);
    });
}

// 2) intent -> character controller (physics owns the sweep; FR-PHY-030)
// Controller tuning (maxSlopeAngleDeg, snapDownDistance, skinWidth) are fields of
// the CharacterController2D component (physics §2.1) — authored in the inspector,
// not passed per call; gravity/max-fall live in world config. Controller calls run
// in FixedUpdate_Pre (physics §3.1): the Physics phase contains only physics_step.
MCE_SYSTEM(player_move, mce::Phase::FixedUpdate_Pre, after("player_input"))
static void player_move(Frame& f) {
    const InputState&     in   = f.resource<InputState>();
    physics::World2D&     phys = f.resource<physics::World2D>();
    f.query<PlayerMove&, CharacterController2D&, const LocalTransform&>()
     .each([&](Entity e, PlayerMove& p, CharacterController2D& cc, const LocalTransform& t) {
        const f32 run  = in.move.x * RUN_SPEED;                   // deadzone/curve: action map
        const bool jump = p.jump_buffer > 0.0f
                          && (p.ground != Ground::Air || p.coyote > 0.0f);
        if (jump) { p.jump_buffer = 0; p.coyote = 0; p.jumps_left--; }

        // one-way drop-through is data, not a controller flag (physics §3.5):
        // Down+Jump sets a DropThrough component that a per-tick gameplay system
        // decrements — physics stays generic, the mechanic stays data.
        if (jump && in.held("down"_sid))
            f.commands().add<physics::DropThrough>(e, {.ticks = 8});

        const f32 vy_in = r0_vel(t, p).y;             // vertical velocity going in
        const physics::MoveResult2D r =
            phys.move_and_collide(e, Vec2{ run, jump ? JUMP_V : vy_in });
        const bool grounded = (r.flags & physics::ControllerFlags::Ground) != 0;
        const bool on_wall  = (r.flags & (physics::ControllerFlags::WallLeft
                                        | physics::ControllerFlags::WallRight)) != 0;

        if (grounded && !p.was_grounded)              // landing edge — tracked in PlayerMove
            f.events().push(PlayerLanded{ .entity = e, .impact = -p.prev_vy });
        p.ground       = grounded ? Ground::Ground : (on_wall ? Ground::WallSlide : Ground::Air);
        p.coyote       = grounded ? COYOTE_TIME : std::max(0.f, p.coyote - f.dt);
        p.was_grounded = grounded;
        p.prev_vy      = vy_in;
        // r.position is authoritative: physics mirrors the pose into LocalTransform
        // (bumping the change tick) — gameplay never writes the pose of a bodied entity.
    });
}

// 3) reactions are systems reading event queues — never callbacks (ecs-gameplay §2.6)
MCE_SYSTEM(player_react, mce::Phase::FixedUpdate_Post, after("player_move"))
static void player_react(Frame& f) {
    for (const PlayerLanded& ev : f.events().read<PlayerLanded>()) {
        // "sfx/land"_guid / "dust"_guid: the GUID string is resolved to a cooked
        // asset handle at scene load (asset-ref fields), never by a runtime
        // string lookup — the event carries the pre-resolved handle (finding 40).
        f.events().push(PlaySfx{ .asset = "sfx/land"_guid, .at = ev.entity,
                                 .gain = clamp(ev.impact / 900.f, 0.3f, 1.0f) });
        f.events().push(SpawnParticles{ .at = ev.entity, .preset = "dust"_guid });
    }
}
```

**A simple enemy AI** — the FSM pattern of FR-ENT-090: table as data, behavior as per-state systems, events carrying transitions. This is the *complete* idiom for both anchor games' AI (FR-ENT-090 acceptance: "enemy FSM inspectable in the editor live"):

```cpp
// ==== game/grunt.cpp — module code ====
enum class Grunt : uint8_t { Idle, Patrol, Chase, WindUp, Recover, Dead };

MCE_COMPONENT(GruntBrain, Grunt state = Grunt::Idle; f32 stateTime = 0;
                          Entity target = Entity::null; Vec2 home = {};)

// guards/actions: plain functions, registered with the table (never serialized)
static bool sees_player(Frame& f, Entity e);
static bool in_reach(Frame& f, Entity e);
static void alert(Frame& f, Entity e) { f.events().push(GruntAlerted{ .enemy = e }); }
static void swing(Frame& f, Entity e) { f.events().push(MeleeHit{ .from = e }); }

static constexpr auto kGruntFsm = mce::fsm::table<Grunt>(
{   {Grunt::Idle,    Grunt::Patrol,  .after = 1.5f},
    {Grunt::Patrol,  Grunt::Chase,   .when = &sees_player, .entry = &alert},
    {Grunt::Chase,   Grunt::WindUp,  .when = &in_reach},
    {Grunt::WindUp,  Grunt::Recover, .after = 0.35f, .exit = &swing},  // hit lands on exit
    {Grunt::Recover, Grunt::Chase,   .after = 0.6f},
    {Grunt::Chase,   Grunt::Idle,    .when = &lost_player_long}, });

MCE_SYSTEM(grunt_fsm_run, mce::Phase::FixedUpdate_Pre, after("player_move"))
static void grunt_fsm_run(Frame& f) {
    mce::fsm::run(f, kGruntFsm, f.query<GruntBrain&, const LocalTransform&>());
}

MCE_SYSTEM(grunt_act, mce::Phase::FixedUpdate_Pre, after("grunt_fsm_run"))
static void grunt_act(Frame& f) {
    f.query<GruntBrain&, Velocity&, const LocalTransform&>()
     .each([&](Entity e, GruntBrain& b, Velocity& v, const LocalTransform& t) {
        switch (b.state) {                                   // per-state behavior, table-aligned
        case Grunt::Patrol: v.v = patrol_around(t.position, b.home, PATROL_SPEED); break;
        case Grunt::Chase:  v.v = seek(t.position, b.target, CHASE_SPEED);         break;
        default:            v.v = {};                    // WindUp/Recover/Dead: planted
        }
    });
}

// damage flows through the typed event, keeping combat decoupled from AI (FR-ENT-030)
MCE_SYSTEM(grunt_death, mce::Phase::FixedUpdate_Post, after("health_apply"))
static void grunt_death(Frame& f) {
    for (const DamageDealt& d : f.events().read<DamageDealt>())
        if (GruntBrain* g = f.world().try_get<GruntBrain>(d.target); g && d.amount > 0)
            { g->state = Grunt::Dead; g->stateTime = 0; }    // FSM table routes Dead nowhere
}
```

### 2.8 API-surface stability strategy

MCE promises **no third-party API stability** (N8 — the engine ships games, not an SDK). What it owes *itself* is a stable-enough surface that the sample ladder and the anchor games keep compiling. The strategy is compiler-enforced narrowness plus mechanical contract tests, not semver:

1. **Narrowness by construction.** One umbrella header (`mce/gameplay.h`), one link target (`mce_engine` + `mce_module_api`), the 12-type budget (FR-DX-010) as the design ceiling for what enters `gameplay.h`. A module physically cannot reach SDL/GL/ImGui/ECS internals — include-path starvation, not convention.
2. **Version stamping, not ABI promises.** `MCE_ENGINE_ID` (hash of git-describe + build config, generated into `mce_engine_version.h` by CMake and compiled into both sides). Loader mismatch → `Errc::EngineIdMismatch` naming both ids → editor console + Play panel (FR-DX-050). Because module and engine always rebuild from the same commit in CI and dev, this only ever fires on a stale manual build — exactly the case it should catch.
3. **The sample ladder is the compatibility test.** `01_triangle…05_showcase` (FR-DX-070) compile and run headless in CI forever; a header change that breaks a sample fails the same commit. This is the de-facto SemVer, at zero ceremony.
4. **Deprecation-by-milestone.** Renames/moves keep an old name for one milestone emitting `[[deprecated]]`, removed at the next milestone gate — coarse, honest, and appropriate for a single-consumer engine (the Unreal/UHT warning in `existing-engines.md` is why there is no code generator and no reflection-format freeze).
5. **What is *deliberately* unstable:** everything under `mce::detail` and all ECS internals. Modules that need them are wrong and the include wall says so at compile time.

---

## 3. Internal design — data structures & algorithms

### 3.1 The module boundary: one engine DLL + one game DLL (the load-bearing decision)

```
mce_engine.dll   (L0–L4 engine targets linked into ONE umbrella DLL; public headers
                  decorated MCE_API → dllexport when building it, dllimport for consumers)
game.dll         (the game module; links mce_engine's import lib; same CRT /MD)
game.exe         (thin shipped host: platform init, app loop, loads game.dll at boot —
                  per ARCHITECTURE F8/target table; "mce_runtime" survives only as the
                  collective name of the ImGui-free engine library set)
mce_editor.exe   (host: editor layer + in-process play; loads game.dll per Play session)
```

Why this shape and not the two obvious alternatives:

- **Not "static module linked into every host."** It would make every gameplay tweak relink `mce_editor.exe` (violating NFR-B-02's 10 s gameplay-change budget) and would make play-in-editor (v0.5) a relink-per-Play proposition. The DLL is what makes "recompile → press Play" possible without an editor restart.
- **Not Handmade Hero's C-ABI function-table boundary.** Hero's split is right when the game is a pure update function over opaque state; MCE's gameplay model is instead *deeply* template-coupled to the engine (query item tuples, `register_components<Ts...>`, `EventQueue<T>`, `fsm::table<S>`). Crossing a C ABI would force a parallel reflection/registration protocol — a second API to keep in sync, which is exactly the two-language-interop tax the stack decision rejected. The C++ ABI across the boundary is acceptable *because* there is only one compiler (MSVC v143), one machine, one consumer (N8: no API stability guarantees; N6: Windows-only). The RHI seam is the engine's one binary-style boundary; the module seam is a source-level boundary with a version stamp.
- **One engine instance, not one per DLL.** The engine builds as the umbrella DLL so that host and module share *one* CRT state, one convar registry, one ECS, one allocator — no duplicated globals, no cross-CRT free hazards. (`/MD` everywhere; allocations cross the boundary only through `mem::IAllocator&` per the core rule anyway.) MSVC detail that makes this workable: export decorations live in the engine headers, so the umbrella DLL exports the static engine libs' symbols without `.def` files or `/WHOLEARCHIVE`; test executables link the same import lib instead of rebuilding the engine statically, so there is exactly one engine build.
- **The MSVC template/ABI rule (finding F11 — the duplication trap this boundary otherwise hides).** Because the boundary is template-coupled C++, two failure modes would silently break the "one engine instance" promise in `game.dll`:
  1. **Template statics duplicate.** A function-local static inside a template that `game.dll` instantiates with its own arguments gets a *second copy* in the module (COMDAT folding stops at the DLL edge — the static lives in each image that instantiates it). Concretely: a `template<class T> ComponentId component_id()` implemented as `static const Id id = next_counter();` would hand a module-registered component a different id inside `game.dll` than inside `mce_engine.dll`, silently splitting the ECS registry.
  2. **The rules that prevent it (binding):**
     - Every templated engine API reachable from a module **must have explicit `__declspec(dllexport)`-decorated explicit instantiations in the engine DLL** for every type on the FR-DX-010 12-type budget — the budget is finite, so the instantiation list is enumerable and checked into one `engine_instantiations.cpp` per affected module (ECS queries/commands/events, `Handle<T>`, `EventQueue<T>`, `fsm::table<S>`, `StateStack<T>`).
     - **Engine headers must not place function-local statics inside templates.** Identity (component ids, type hashes, string interning) is routed through exported *non-template* registry functions (`mce::ecs::component_id(StringId)` and friends) whose statics live once, in `mce_engine.dll`.
     - **CI test (from ROADMAP M3 on):** a module-registered component's `ComponentId` returned inside `game.dll` must equal the engine-side id for the same type (asserted via a devhost module that registers a component, spawns it, and compares against a engine-side registration of the same type — mismatch fails CI, not play mode).

### 3.2 What the loader produces (the registration record)

`install(World&)` populates, inside the `World`:

- component ids + reflection records (already `ecs-gameplay`'s machinery — the module just calls it);
- a **system table**: `{fn ptr, name, phase, after-name}` per registration, in registration order — this array *is* the execution order and is what the editor's pipeline view renders (FR-DX-030);
- an **event registry** (type, producer phase, named consumers);
- FSM tables and sequence definitions, keyed by `StringId` — stored as data on the World so the editor can render live FSM state (FR-ENT-090 acceptance) and the current step of every `SequencePlayer` (v0.5);
- module-owned singleton resources.

Nothing is registered at DLL static-init time except `constinit` tables referenced by `install` — so load/unload ordering is trivial and no loader-lock code exists anywhere (the DllMain rule: module code does nothing in `DllMain`; static init only builds constant tables).

### 3.3 Reload semantics — why reload-at-Play is the whole feature

The World is destroyed and rebuilt from cooked content at every Play (editor-tools §2.8; ecs-gameplay §2.10). Because modules are required to keep no state outside the ECS (rule §2.4.1), a module rebuild at the Play boundary has *no stale state to migrate*: `Stop → FreeLibrary → rebuild → install(fresh World)`. This is why v1 does not promise live reload and does not need to: the only state that would need migrating is ECS data, which a fresh Play rebuilds anyway. Live mid-session reload (N13) would additionally require module-side state migration (a serialized-state shim) — deferred with its recorded trigger (measured iteration pain after Ashen Court), and the DLL split means it would be an *addition* to this design, not a rewrite.

### 3.4 SequenceRunner internals (v0.5; the MVP `SequenceRunner-lite` is the same runner minus the rows marked v0.5)

- `SequencePlayer` is 12 bytes (`StringId + u16 step + f32 elapsed + 2 bools`) — entirely serializable, which is the acceptance test (resume mid-sequence).
- Step execution: `while (budget--) { evaluate step; if blocking break; else ++step; }` with a per-tick entity budget (8) and a debug assert that a sequence without `End` cannot loop without `looping` (validation at `define()`).
- `MoveTo` writes `Velocity` (physics moves the body — sequences never teleport through walls); completion is "within epsilon or blocked" with an optional `on_blocked` fail action.
- `Call` resolves through the module's registered `CallFn` table by `StringId` at `define()` time (never per tick); save/resume serializes only the *id*, so a save made against module v1 resumes correctly under v2 unless the id was removed — diagnosed at resume with code+name (FR-DX-050).

### 3.5 How the editor sees user code (invocation contract)

- **In the preview/authoring world: user code never runs.** The editor preview runs render-extract only, with no module systems registered (editor-tools §3.1) — gizmos and inspectors operate on reflected data alone. This keeps authoring deterministic, fast, and immune to module bugs.
- **In play mode (v0.5):** `PlaySession::enter` → `load_game_module(copy_path, play_world)` → `install` → `load_scene` → fixed-step ticks. The editor reads live state (FSM state enum via reflection, `SequencePlayer.step`) through the ordinary ECS read API. The editor has no way to *call into* module code ad hoc — the only sanctioned entries are sim ticks, which is what makes play-mode deterministic (editor-tools risk R3).
- **MVP (no play-in-editor):** run-in-player launches `game.exe`, whose `main` runs the identical loader — one code path (P6), the editor is just not one of its callers yet.

---

## 4. Threading & lifecycle

### 4.1 The compile/reload workflow (the daily loop, with budgets)

```
edit game/*.cpp
 └─ cmake --build --preset dev        # incremental: game.dll only (PCH + unity, small TU set)
                                      #   budget: ≤ 10 s to next Play (NFR-B-02, FR-DX-040)
 editor: press Play
 └─ PlaySession::enter                # copy game.dll → game_loaded.dll (file-lock workaround)
                                      # → load → engine_id check → install → scene load
                                      #   budget: ≤ 1 s excluding compile (NFR-M-03)
 sim ... Stop → world destroyed → FreeLibrary (debug: module-alloc leak assert)
 (old game_loaded.dll released → next build can overwrite game.dll)
```

MVP (run-in-player only): build → cook → `game.exe`; same loader path inside the runtime, exercised earlier and more often because the editor isn't a second consumer yet. Shaders/assets/content need no compile at all (P3 — editor-first iteration is why the *code* surface is allowed to be slow-ish; content is not).

What is deliberately **not** in the loop: no mid-session code reload (N13), no script-recompile-in-place, no C#-style domain reload tax (the Unity lesson, `existing-engines.md` §2.1 — MCE's edit-compile-run stall is one small DLL, not a VM domain teardown).

### 4.2 Threads

Module code runs **only on the main thread, only inside fixed-step phases** — there is no module-visible threading surface at all (no spawn-API in `gameplay.h`; jobs are engine-internal). The audio thread never executes module code (commands cross the SPSC FIFO via the audio bridge, FR-AUD-010); the editor watch/cook threads never execute module code. The auto-parallel experiment (ecs-gameplay §4.1) was the only place module systems could have run off-main; it was run on 2026-10-10 and not merged (REQUIREMENTS N17), so this rule holds without exception.

### 4.3 Module lifecycle states

```
NotLoaded → Loading(copy+LoadLibrary) → Validating(engine_id, entry, table checks)
          → Installed(world) ──sim ticks──→ Unloading(world teardown first) → NotLoaded
Error states: LoadFailed | IdMismatch | InstallFailed  → Result error → editor console / runtime error screen
```

All transitions happen on the main thread at Play boundaries. `install()` failures are recoverable (Play is refused with the error; the editor keeps running). Debug builds tag every allocation made from inside `install`/systems with a module arena tag so `unload` can assert zero leaks (module code has no allocator access, so any tagged allocation left is an engine bug, not user error — the assert documents the invariant).

---

## 5. Upstream/downstream dependencies

### 5.1 Upstream — what scripting consumes

| Provider | What | Interface consumed |
|---|---|---|
| `core` | allocators (`mem::frame`, `NoHeap` guard), `Result<T,Error>`/`Errc` (module error codes), `StringId`, convar registry, `platform::load_library` (the DLL loader), Tracy client (module system zones are emitted by `MCE_SYSTEM`), crash handler | The module contract compiles against core types only. |
| `ecs-gameplay` | `World/Entity/Frame/Commands/EventQueue`, phase pipeline + registration records, reflection (component + enum records — FSM dropdowns and sequence field display are reflection reads), `state_hash` (sequences/FSM state participate in the determinism hash) | This subsystem is a *client* of the ECS, contributing ordinary systems; it owns no scheduling machinery. |
| `physics` | `physics::World2D`/`physics::World3D` resources: `move_and_collide` + `MoveResult2D`, raycast/overlap queries | Called from module systems as plain resource calls (§2.7); no module-visible physics types beyond the controller + query results. |
| `assets` | GUID→`AssetId` resolution, prefab `instantiate_prefab` (Spawn/Call steps), cooked `TimelineAsset` chunks (v1.0) | Sequences reference assets by GUID; failures degrade per FR-DX-050 (skip + one error), never crash. |
| `audio-input-ui` | `InputState` resource snapshot (per-tick, deterministic); `PlaySfx` event → FIFO bridge; `AudioDevice` command API + `AudioHandle` for the sequence runner's `PlaySound`/`StopSound` steps (the runner is engine code and is the only non-module caller) | Modules emit events; they never touch the mixer, SDL, or devices. |
| `rhi-renderer` | **nothing.** Module code never renders; `AnimatedSprite`/`MeshRenderer` components carry the render-visible state. | — |

### 5.2 Downstream — what consumes scripting

| Consumer | Relationship | Rule |
|---|---|---|
| `editor-tools` | `PlaySession::enter/stop` drive the loader; pipeline view reads the system/phase registry; FSM/sequence debug views read component state; the API census (FR-DX-010) reads module records | The editor loads user code *only* through this subsystem's loader, only for play sessions; it never links a game module statically. |
| runtime host (`game.exe`) | Loads `game.dll` at boot through the copy-and-load loader; the module list is its entire gameplay surface | The host ships with zero knowledge of any specific game (UC-D: a third game = new module + content, no engine changes). |
| devhost/samples | The FR-DX-070 ladder builds its "games" as modules and runs them headless | Samples are the CI contract for module API stability (§6). |

**CMake target shape (binding build policy applied):** `game.dll` is a preset-built target in the game project; both hosts locate it via a convar/path convention (`--module game_loaded.dll`); no MSVC project files; the module inherits `/W4 /WX /permissive- /EHs-c-` from the `mce_module_api` interface target so flag drift between engine and module is impossible.

---

## 6. Test strategy

1. **Loader contract tests** (MVP): a synthetic minimal module exercises the happy path; missing DLL, missing export, `engine_id` mismatch, and `install()` returning an error each produce the documented `Result` with code+stage (FR-DX-050's "every failure is one of two categories" audit item).
2. **Reload lifecycle test** (MVP): Play → 600 ticks → Stop → FreeLibrary → re-copy a *rebuilt* module → Play again; assert first-tick `state_hash` equality (NFR-M-03 through the module path) and zero module-tagged allocations outstanding after unload (debug leak assert).
3. **FSM semantics** (FR-ENT-090, MVP): timed transitions fire on the exact tick with a synthetic clock; `stateTime` resets on transition; entry/exit run in exit→entry order exactly once; guard evaluation order == table order; validation catches unreachable states and duplicate transitions at `install`.
4. **Sequence semantics** (FR-ENT-100, v0.5): a sequence with `Wait/MoveTo/PlayAnim/Emit/Call` completes at the tick-math-derived time; save mid-sequence → resume → identical completion tick and final hash (round-trip acceptance); `Call` id resolution errors are diagnosable; infinite-loop validation fires.
5. **Sample ladder as the module API contract** (FR-DX-070): each sample module builds + cooks + runs 300 headless fixed steps in CI, state-hash snapshotted; an engine header change that breaks a module fails the PR. This is also the FR-DX-010 census check (grep `mce::` usage in samples ≤ budget).
6. **Determinism integration** (FR-RUN-040): module systems are ordinary phase members, so the 10k-tick hash harness covers them by inclusion — the platformer sample's FSM + player controller are in the hash set; replay (FR-RUN-070) replays module behavior with no extra machinery.
7. **Sanitizers + hygiene:** ASan per PR over the loader/reload tests (DLL boundary is prime UAF territory — the FreeLibrary-vs-world-teardown order is the risk); clang-tidy on `src/module` (the module host: `src/module/src/module_host.cpp`) and the sample modules; CI greps module sources for banned includes (SDL/GL/ImGui/heap) and for mutable file-scope statics.
8. **Manual gate:** the 30-second loop (UC-C) timed end-to-end with a module change in the middle — the stopwatch number goes in the milestone doc (NFR-B-02/05).

---

## 7. Risks & mitigations

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **MSVC DLL/CRT pitfalls** — duplicated engine state if a module ever links static engine libs; cross-CRT free | Medium | High (subtle corruption) | The umbrella-DLL shape (§3.1) makes one-engine-instance structural, not conventional: the module target physically links only the import lib. CI links a test module against a deliberately double-built engine to prove the linker rejects it. |
| 2 | **Native-only authoring friction** — the classic pressure that turned engines into Lua engines | High (eventual) | Medium | The pressure is met with the data toolbox (§1.3), not a VM: FSM tables, sequences, and timelines cover the designer-shaped work; the P3 content rule (no recompile for content) covers the rest. VM re-entry stays gated on N2's measured trigger, and §1.3's tiering means a VM would *replace the code surface's dullest third*, not bolt on beside it. |
| 3 | **Reload-scope creep** — "just reload the DLL live" mid-session | Medium | High (fragility sink; the Hazel stall pattern) | N13 is binding; the design says *why* it's cheap to add later (state is ECS data; §3.3) and the trigger is recorded (measured pain post-Ashen Court). Play-boundary reload is the whole v1 promise. |
| 4 | **Engine header churn forces module rebuilds** — every engine edit recompling the module | Medium | Medium | NFR-B-03 budgets it (≤ 90 s); PCH + unity absorb most of it; the umbrella header means module TUs see a *small* header set, and `mce::detail` starvation (§2.3) keeps the compiled surface honest. |
| 5 | **File-lock friction / stale-DLL confusion** (Windows dev reality) | High (frequency) | Low | Copy-and-load ring (§2.2); `engine_id` surfaced in the Play panel and the runtime's boot log line so "which DLL am I running" is always visible (FR-DX-050 failure-visibility rule). |
| 6 | **Module crashes take down the editor process** (in-process play) | Medium | Medium | Accepted deliberately: the alternative (child-process play) forfeits live inspection, which is the point of play-in-editor. Minidumps + autosave make the cost a re-open, not lost work (editor-tools R9); authoring-world isolation (§3.5) means module bugs cannot corrupt unsaved scene state. |
| 7 | **FSM/sequence vocabulary creep** toward a visual scripting language inside the back door | Medium | Medium | Step/transition vocabularies are closed lists with per-addition use-case triggers (§2.6, mirroring the relationship-pair trigger rule); visual scripting stays FR-EDT-150 Won't. |
| 8 | **`Call`/guard statics leaking state across reload** | Low | Medium | Rule §2.4.1 + CI grep for mutable file-scope state in module sources; hooks receive everything through `Frame&`, so the disciplined path is also the easy one. |
| 9 | **Loader-lock / DllMain hazards** in module static init | Low | High | Documented rule (no DllMain work; constinit tables only) + a devhost module that deliberately does work in static init to smoke-test the guidance in CI. |

---

## 8. Milestone mapping

Tier names with ROADMAP milestone IDs (ARCHITECTURE §10.1 is the mapping authority).

- **MVP (ROADMAP M3):** module contract + umbrella `mce_engine.dll` split + loader with copy-and-load and `engine_id` check; the §3.1 template/export rules + ComponentId identity CI test; `fsm.h`; **SequenceRunner-lite** (closed step subset `{Wait, PlayAnim, PlaySound, Emit, End}`, no save/resume — ARCHITECTURE §10.1); the platformer sample's player controller and grunt FSM (the two sketches above) as the load-bearing proof; loader/reload/FSM tests in CI. *Artifact A's gameplay is a module.*
- **v0.5 (ROADMAP M5b for play-in-editor, M6–M7 for the rest):** play-in-editor loads the module through the same `PlaySession::enter` path (lands with M5b — finding 11); full action-list SequenceRunner (`MoveTo`/`WaitForEvent`/`Spawn`/`Destroy`/`Call`) + save/resume (FR-ENT-100); editor FSM/sequence live views (with editor-tools); reload lifecycle test through `PlaySession`.
- **v1.0 (ROADMAP M8):** TimelinePlayer over cooked timeline chunks (with `assets` + editor timeline view); the module template + new-project generator (§1.1 item 6, finding F17 — the template ships as an M8 deliverable and is the only sanctioned way to start a game project); API census doc refresh (FR-DX-010); UC-D proves the module model with ≤ 2 engine-side gameplay systems.

## References

- Casey Muratori, *Handmade Hero* — the platform/game DLL split (adopted as architecture; live reload explicitly *not* adopted, per N13).
- Timothy Ford, "Overwatch Gameplay Architecture and Netcode," GDC 2017 — state-as-data + per-state systems, the FSM pattern's provenance.
- Bob Nystrom, *Game Programming Patterns* — Update Method (sequences/FSM runtimes), Command (transition/step tables as data), Event Queue.
- Jason Gregory, *Game Engine Architecture*, 3rd ed. ch. 14 — the gameplay-object-model spectrum this doc positions against (native-first with data tools).
- Unity editor manual — domain-reload iteration tax as the VM cautionary record (`existing-engines.md` §2.1); C++ coroutines rejection rationale in `research/ecs-gameplay.md` §11.2.
- Repo-internal: `docs/architecture/ecs-gameplay.md` §2.12 (committed programming model), `docs/architecture/editor-tools.md` §2.8 (PlaySession), `docs/architecture/core.md` §2.5 (`load_library`, allocators), `docs/research/existing-engines.md` §2/§4, `docs/REQUIREMENTS.md` (FR-DX-040/070, FR-ENT-090/100/110, N2, N13).
