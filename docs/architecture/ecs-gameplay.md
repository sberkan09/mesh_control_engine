# MCE Architecture — ECS & Gameplay Runtime

**Document class:** Subsystem architecture · **Subsystem:** `ecs-gameplay` · **Date:** 2026-10-06
**Authority:** `docs/REQUIREMENTS.md` (authoritative) + `docs/research/ecs-gameplay.md` (domain research). Binding constraints inherited: hand-written archetype ECS (flecs/EnTT/Bevy studied, never linked), generational-index handles only, fixed 60 Hz sim with interpolated render, per-phase command buffers over frame-linear memory, editor owns the canonical scene, no exceptions/RTTI, zero heap allocation in hot paths, reflection-driven serialization, ImGui never in the runtime.
**Scope note on tiers:** MVP-tier features (archetype core, phases, command buffers, events, scene load, starter kit, FSM, determinism harness) are designed to implementation depth. v0.5 features (change detection, hierarchy, prefabs, play-in-editor support, sequences) get committed shapes with lighter detail. v1.0 features (timelines) get sketches only.

---

## 1. Responsibilities & non-responsibilities

### 1.1 This subsystem owns

1. **The object model.** Entities as generational handles, reflected plain-struct components, archetype-table storage, queries. This is the substrate every other runtime subsystem reads and writes (FR-ENT-010).
2. **Execution structure.** The phase pipeline: named phases, explicit intra-phase system order, sync points. Declared order == execution order, renderable in the editor's pipeline view (FR-ENT/DX-030).
3. **Structural change discipline.** Per-phase command buffers over frame-linear memory; bulk application at sync points; lifecycle hooks at application time (FR-ENT-020).
4. **Communication.** Typed double-buffered event queues with registered producers/consumers (FR-ENT-030); lifecycle hooks for engine-owned invariants.
5. **Change detection (v0.5).** Per-row changed-tick columns, `is_added()/is_changed()` filters (FR-ENT-040).
6. **Hierarchy (v0.5).** `ChildOf` single-parent transform hierarchy with depth-ordered dirty propagation (FR-ENT-050).
7. **Scene & prefab runtime semantics.** Loading cooked `.mce` scene chunks into a live World, scene-state push/pop for app-shell screens (FR-RUN-030), prefab instantiation as bulk row copy + remap + overrides (FR-ENT-060/070).
8. **The gameplay programming model.** Native components + systems as the only behavior model in v1; the starter-kit component set (FR-ENT-080); the FSM helper (FR-ENT-090); the rulebook for what goes where (state-as-data, sequences-as-data).
9. **Determinism machinery for the sim.** Deterministic iteration order, recorded-order command application, the component-state hash used by the FR-RUN-040 CI harness.
10. **The `Frame` context.** The one object a system sees: tick, dt, alpha, commands, events, resources, queries. It makes the wrong thing (heap alloc, cross-frame pointers, per-render-frame gameplay) hard by omission.
11. **Home of small shared cross-subsystem interfaces** (ARCHITECTURE fix F5). Interfaces that L2 subsystems must exchange without creating horizontal include edges — currently `mce::debug::IDebugDrawSink` — are defined *here*, in `mce_ecs`, beside the other shared seams. `physics` fills the sink during its extract contribution; `rhi-renderer`'s `DebugDraw` implements it; the runtime/editor composition point registers the implementation as a World resource and a sink-pump system in `Render_Pre` drains it into the debug line batch. This keeps the include direction strictly downward (`physics ↓ ecs`, `render ↓ ecs`) with no `physics → render` edge ever.

### 1.2 This subsystem explicitly does NOT own

| Concern | Owner | Boundary rule |
|---|---|---|
| Frame timing, accumulator, vsync, app shell (window/quit) | `core` (RUN) | ECS consumes `SimClock` values via `Frame`; ecs-gameplay never calls SDL3. |
| Render extraction, interpolation-to-GPU, draw submission | `rhi-renderer` | Renderer *reads* the World in `Render.Pre` through a declared read-only extract query; it never mutates gameplay components. Interpolation *consumes* change ticks but the tick columns live here. |
| Physics stepping, broadphase, contacts | `physics` | Physics registers its systems into `FixedUpdate.Physics` phases and reads/writes components; it gets no scheduler privileges beyond explicit ordering. |
| `.mce` container format, cook, mmap, GUID→asset resolution | `assets` | ecs-gameplay consumes cooked scene/template chunks; it never parses interchange formats and never touches the cook cache. The editor-side scene *document* (text, canonical) belongs to `editor-tools`; this subsystem defines only the load-time semantics of the cooked chunk. |
| Window/input/audio devices and mixers | `audio-input-ui` | Input arrives as the `InputState` resource snapshot sampled per tick (FR-INP-030); audio receives its commands via events — the audio thread never touches the ECS. |
| Editor UI, inspector, undo, gizmos, the canonical scene document | `editor-tools` | The editor mutates its live preview world through this subsystem's public API only; it may never reach into tables by pointer. |
| Action-list sequences, timeline playback runtime | `scripting` | See §2.12: the native components/systems programming model is committed here; `scripting` builds the *data-driven runtime* (SequenceRunner, TimelinePlayer) on top of ECS primitives. No VM (N2). |
| Save/persistence files | `core` + reflection | This subsystem exposes reflection-safe component serialization; the file location and versioning policy are FR-RUN-065 and live outside. |

### 1.3 The design stance in one paragraph

MCE's ECS is not a performance stunt; it is the engine's **reflection-driven, serializable, editor-facing object model** (research §12). That is why archetype tables win over sparse sets despite EnTT's simpler model: columnar tables make reflection-driven batch serialization, deterministic iteration order, and inspector reads all fall out of one storage shape. Performance targets (NFR-T-01: 100k entities × transform+velocity iterate+write < 1.5 ms single-thread; ~10k-entity design ceiling, N9) are comfortably inside what dynamic archetype tables deliver; the design below never spends complexity that a 10,000-entity game cannot pay back.

---

## 2. Public interface

The binding API-surface budget (FR-DX-010) says a 2D game requires knowing at most **12 named types** from `mce::`: `World, Entity, Frame, Commands, EventQueue<T>, Sprite, LocalTransform, InputState, AudioHandle, Material2D, Camera2D` + the component/system macros. Everything else lives in `mce::detail`. Every API below is checked against that budget; internal-only types are marked.

### 2.1 Entities, components, registration

```cpp
namespace mce {

// The ONLY thing gameplay code may hold across frames (binding).
// 8 bytes, trivially copyable, hashable, serializable.
struct Entity {
    uint32_t index;       // slot in the entity array
    uint32_t generation;  // bumped on destroy; stale handles fail, never alias
    bool is_null() const;
    static const Entity null;
};

// Component declaration: a reflected plain struct. One macro line supplies
// component id, reflection registration, and static invariants.
//   Rule: components are trivially copyable, no heap, no destructors.
MCE_COMPONENT(LocalTransform,
    Vec3 position;
    Quat rotation = Quat::identity();
    Vec3 scale    = Vec3::one();
)
MCE_COMPONENT(Velocity,  Vec3 v;)
MCE_COMPONENT(Health,    float hp; float max;)

enum class EnemyState : uint8_t { Idle, Patrol, Chase, Attack };
MCE_COMPONENT(EnemyBrain,  EnemyState state = EnemyState::Idle;
                           float     stateTime = 0.0f;
                           Entity    target = Entity::null; )
// static_assert(std::is_trivially_copyable_v<T>) fires inside MCE_COMPONENT.
// Registration w.register_components<Ts...>() binds the type to the
// reflection registry (name, fields, size/stride) in one step (FR-DX-020).

} // namespace mce
```

`MCE_COMPONENT` is a macro + constexpr-trait registration (the `reflection` subsystem's macro+constexpr trait system, per the binding from-scratch list). Adding a component requires **zero** serializer changes and **zero** editor code — the inspector, scene save/load, and save-game serialization all read the reflection record (FR-ENT-020, FR-DX-020).

### 2.2 World — creation, access, lifecycle

```cpp
class World {
public:
    // --- entities -------------------------------------------------------
    Entity spawn();                                    // immediate; empty entity
    Entity spawn(const EntityDesc& desc);              // + component rows (see §3.8)
    void   destroy(Entity e);                          // MUST go through Commands in systems
    bool   alive(Entity e) const;                      // generational check
    template<class T> const T* try_get(Entity e) const;     // O(location + column)
    template<class T> void     set(Entity e, const T& value); // bumps changed tick
    // Raw mutable get() outside a query is allowed ONLY in tool/editor code
    // and hook implementations; systems use query access (§2.4).

    // --- registration ---------------------------------------------------
    template<class... Ts> void register_components();  // id + reflection + hooks
    template<auto SysFn>  void register_system();     // MCE_SYSTEM has done the rest
    void register_event<T>(Phase producer_phase);      // typed queue, no god-queue
    template<class T> void set_resource(T res);        // singleton, replaces in place
    template<class T> T&   resource();                 // Time, InputState, ActiveCamera...

    // --- scene / prefab runtime ------------------------------------------
    // mce_ecs takes *cooked chunk payloads*, never asset-system types (layering
    // rule assets → ecs, not the reverse — ARCHITECTURE §2.2): the caller
    // (runtime shell / PlaySession, above both) resolves AssetId → chunk bytes
    // via mce_asset and passes the span.
    Result<void, Error> load_scene(Span<const byte> scene_chunk);   // CHNK_SCENE payload
    void                unload_scene();                             // despawns scene-owned entities
    Result<Span<Entity>, Error> instantiate_prefab(Span<const byte> prefab_chunk, SpawnParams p);

    // --- module wiring ----------------------------------------------------
    void install_module(void (*fn)(World&));   // e.g. gameplay_module (research §13.2)

    // --- debug / determinism / tooling ------------------------------------
    uint64_t state_hash() const;                 // FR-RUN-040 harness (§3.9)
    struct Metrics { uint32_t entities, tables, avg_table_occupancy_x100, pending_commands; };
    Metrics metrics() const;                     // editor ECS view, Tracy, component-soup watch
};
```

Access rules, stated as invariants rather than suggestions:

- **Generational or nothing.** Any reference an entity holds of another entity (`target`, `owner`, `ChildOf.parent`) is an `Entity` handle. A stale handle returns `nullptr` from `try_get` and `false` from `alive` — never an aliased row (FR-ENT-010 acceptance).
- **Raw component pointers never leave a system scope.** Table rows move on structural change; the binding forbids cross-frame pointers anyway, so the API does not hand them out across sync points.
- **Singletons are resources, not one-component entities** (research §10): `Time`, `InputState`, `ActiveCamera`, physics world, mixer state live outside the tables, explicitly registered.

### 2.3 Queries

```cpp
// Built from Frame inside a system; never stored across ticks.
f.query<const LocalTransform&, Velocity&>()
 .with<EnemyBrain>()                 // tag/optional component, not in the item tuple
 .without<Dead>()
 .changed<const LocalTransform&>()   // v0.5: change-detection filter
 .each([](Entity e, const LocalTransform& t, Velocity& v) { /* ... */ });

// Table-level access for bulk/system-heavy code (still zero-alloc):
for (TableSpan<const LocalTransform, Velocity> ts :
     f.query<const LocalTransform&, Velocity&>().tables()) {
    // t is a columnar pair of pointers over one archetype table; linear loops.
}
```

- Queries are **frame-scoped view objects** — no registration ceremony, no invalidation hazard for the user: a query never outlives its `Frame`, and structural changes cannot invalidate it mid-iteration because they cannot happen mid-iteration at all (§2.5).
- Matching table lists are cached per query signature and invalidated by a global structure-version counter (§3.3) — repeated queries within a phase cost one integer compare.

### 2.4 Systems, phases, and Frame

Systems are plain functions; the declaration is the documentation (FR-DX-030). Registration order within a phase **is** execution order, asserted by a registration-order test and rendered live in the editor's pipeline view.

```cpp
// Phases (the complete v1 list — adding a phase is a design event, not a habit):
enum class Phase : uint8_t {
    Startup,          // once, deterministic order, after scene load
    FixedUpdate_Pre,  // input apply, timers, gameplay pre
    FixedUpdate_Physics, // owned by `physics`: broadphase, solve, controller
    FixedUpdate_Post, // transform propagation, trigger resolution, gameplay post
    Render_Pre,       // per RENDER frame: extract, interpolate (alpha), UI state
    Render_Submit,    // renderer-only phase (translate to RHI + submit)
    Editor,           // editor-only systems; compiled out of shipped runtime
};

// The macro registers a system function into a phase with an ordering name.
MCE_SYSTEM(enemy_think, mce::Phase::FixedUpdate_Pre, after("input_apply"))
void enemy_think(Frame& f) { /* ... */ }

// Frame: the only context a system receives.
struct Frame {
    // --- time (fixed-step world view; FR-RUN-020/050) ---
    uint64_t tick;        // fixed-step counter (also the change-tick epoch)
    float    dt;          // exactly 1/sim_hz — NEVER scaled; time_scale scales accumulator inflow (FR-RUN-050)
    float    alpha;       // render interpolation factor; valid in Render_Pre only
    float    time_scale;  // slow-mo/pause multiplier (determinism test covers scaled dt)

    // --- services ---
    Commands&    commands;          // deferred structural changes (§2.5)
    EventWriter  events;            // f.events().push(DamageDealt{...}) (§2.6)
    template<class T> const T& resource() const;   // read
    template<class T> T&       resource();         // write (rare, explicit)

    // --- queries (§2.3) ---
    template<class... Cs> QueryBuilder<Cs...> query();

    // --- accessors that exist to be refused ---
    // No heap allocator is reachable from Frame. Per-frame scratch comes from
    // f.scratch() — a bump sub-arena of the frame allocator, freed at sync point.
    FrameScratch& scratch();
};
```

The frame loop shape (owned by `core`/RUN, shown here to fix the phase contract):

```
while (running) {
    render_frame_begin();                    // frame-linear allocator slot reset (after fence)
    pump_platform_events();                  // SDL3 -> input subsystem -> InputState snapshot
    accumulator += frame_delta();
    steps = 0;
    while (accumulator >= FIXED_DT && steps < MAX_STEPS) {   // MAX_STEPS = 5 (spiral clamp)
        run_phase(FixedUpdate_Pre);          // each phase ends in a SYNC POINT (§2.5)
        run_phase(FixedUpdate_Physics);
        run_phase(FixedUpdate_Post);
        accumulator -= FIXED_DT; ++steps; ++tick;
    }
    alpha = accumulator / FIXED_DT;          // interpolated rendering (Gaffer On Games)
    run_phase(Render_Pre);
    run_phase(Render_Submit);                // renderer: extract -> RHI translate -> GL submit
    render_frame_end();
}
```

There is deliberately **no per-render-frame gameplay phase** to register into — the API makes the phase split the only path (research §4.3). `Render_Pre` systems (render extraction, camera lerp) are engine-owned; user gameplay code belongs in `FixedUpdate_*`.

### 2.5 Commands — deferred structural change

```cpp
class Commands {
public:
    Entity spawn();                                      // handle valid immediately,
                                                         // components appear at sync point
    void   destroy(Entity e);
    template<class T> void add(Entity e, T value = {});
    template<class T> void remove<T>(Entity e);
    template<class T> void set(Entity e, const T& v);    // allowed for not-yet-queried data

    // Bulk helpers (spawn storms; FR-ENT-020 acceptance: 10k spawns in one tick):
    template<class T> Span<Entity> spawn_n(uint32_t n, const T& initial);
};
```

Rules:

- A system **may not** see its own structural change. Need the new row this tick? Split into two systems across the sync point — the pipeline makes the dependency visible instead of making it a race (Overwatch's deferred-command model, GDC 2017).
- Queries are never invalidated mid-iteration: nothing mutates tables while a phase runs. This is the single most important safety property of the runtime, and it is enforced by construction (only `Commands` and sync points mutate structure; raw `set()` of an existing component mutates data in place and is fine).
- Application order == recorded order within each application pass (§3.4) — determinism holds by construction.

### 2.6 Events

```cpp
// Registration: every event type has an explicit producer phase and named consumers.
w.register_event<DamageDealt>(/*produced in*/ Phase::FixedUpdate_Physics);

// Producer:
f.events().push(DamageDealt{ .target = e, .amount = 12.0f, .from = attacker });

// Consumer (a system declares what it reads; editor event-flow view renders it):
MCE_SYSTEM(health_apply, mce::Phase::FixedUpdate_Post, after("enemy_think"))
void health_apply(Frame& f) {
    for (const DamageDealt& ev : f.events().read<DamageDealt>()) {
        if (Health* h = f.world().try_get<Health>(ev.target))
            h->hp -= ev.amount;
    }
}
```

- Typed queues, individually registered — no god-queue. Double-buffered: writers push to the back buffer all frame; the sync point swaps, so readers see a stable, immutable list (Nystrom, *Game Programming Patterns*, Event Queue; Bevy's `Events` design).
- Reader guarantee (FR-ENT-030 acceptance): a reader never sees a partially written list — unit-tested.
- Engine-internal lifecycle reactions (a `Sprite` gaining a GPU handle, a `RigidBody2D` releasing its body) are **hooks** at command-application time, not events. Observers/trigger cascades are rejected for v1 (research §6); reactions are systems reading event queues unless a hook is provably the only correct tool.

### 2.7 Change detection (v0.5)

```cpp
// Every component column carries a parallel u32 changed-tick column (per row).
f.query<LocalTransform&>().changed<LocalTransform&>()   // only rows touched since last read
 .each([&](Entity e, LocalTransform& wt) { /* re-interpolate / propagate */ });

// Inside a query item:
if (v.is_added())   { /* first tick this component existed */ }
if (v.is_changed()) { /* tick > last-read tick */ }
// Writes through query access bump the row's tick automatically.
// Raw struct writes via pointer do NOT — documented footgun; .touch() marks dirty
// explicitly (mirrors Bevy's DetectChanges; research §7).
```

Cost model: 4 bytes per component instance (~6% on a 64-byte `WorldTransform`) — accepted at MCE's scale, rejected per-table granularity because one entity moving must not dirty 5,000 rows. Killer consumers: dirty-transform propagation (§2.8), render interpolation of only-moved entities (FR-ENT-040), inspector change highlighting.

### 2.8 Hierarchy (v0.5)

```cpp
MCE_COMPONENT(ChildOf, Entity parent = Entity::null;)   // single parent; users write this
// WorldTransform is engine-written: read-only to gameplay, written by propagation.
MCE_COMPONENT(WorldTransform, Mat4 world; Vec3 position; Quat rotation; Vec3 scale;)
// `Children` is NOT a user component: the parent-side index is a sparse set
// maintained by a hook on ChildOf add/remove — users cannot create desyncs.
```

Propagation (FR-ENT-050): a `FixedUpdate_Post` system iterates entities with `Changed<LocalTransform>` (or newly `ChildOf`-attached), builds the dirty subtree set, walks it **depth-first from roots**, composes `WorldTransform = parent.World * local`, and bumps the child's tick. Dirty-flag discipline (Nystrom) means rotating one parent touches exactly its subtree — asserted in debug (only changed subtrees re-walked).

No generic relationship pairs in v1. Three concrete relationship components (`ChildOf`, `OwnerOf`, physics `JointOf`) ship as plain components; pair machinery is admitted when a **fourth** concrete relationship appears (recorded trigger, research §8).

### 2.9 Starter kit (FR-ENT-080)

MVP ships this closed set as engine components — both anchor games build from it with zero engine-source changes:

- Transform: `LocalTransform`, `WorldTransform` (engine-written), `ChildOf` (v0.5)
- 2D: `Sprite`, `AnimatedSprite` (sheet params, fps, loop/pingpong/once, frame events), `Camera2D` (zoom/rotation/pixel-snap)
- 2D (v0.5): `ParticleEmitter` (cooked `CHNK_PART` preset handle + live emission state) driven by a `particles_spawn`/`particles_advance` pair of systems in `FixedUpdate_Post`/`Render_Pre` that emit into the same `SpriteInstance` extract path as `Sprite` — one draw vocabulary for both (FR-R2D-050)
- 3D: `MeshRenderer` (asset handle, material), `Camera3D`, `Light` (directional/point/spot params)
- Physics: `RigidBody2D`, `Collider2D` (shape variant Box2|Circle2|Convex2, C++ flags `isSensor`/`oneWay` per `physics` §2.1 — serialized snake_case `is_sensor`/`one_way` per the physics §2.1 mapping), `CharacterController2D`
- Audio: `AudioSource` (asset handle, bus, gain, loop)
- Text: `TextLabel` (font asset, string, size, color)
- Gameplay-generic: `Health`, `Velocity`, `State<EnumT>` helper component for FSMs, `Active`/`Visible` tags

### 2.10 Scene lifecycle: load → activate → tick → unload

The runtime **never mutates the canonical scene** (binding). The scene asset is immutable input; the World is an instantiation of it.

```
load_scene(guid):
  1. assets: mmap the scene chunk from the .mce pack (CRC + version checked there)
  2. remap:   build template-id -> entity slot table; resolve asset GUIDs -> handles
  3. bulk:    append rows per archetype (columns copied in table order), no per-entity work
  4. hooks:   on_add fires per component (allocates GPU/audio/physics mirrors)
  5. state:   world enters Inactive (systems registered but not ticked)

activate():
  - Startup phase runs once, in registered order (spawn dynamic entities, bind cameras)
  - world enters Active; FixedUpdate phases tick from the next frame
  - single active scene at MVP; scene-state STACK below composes shells

deactivate()/unload():
  - on_remove hooks release subsystem mirrors (physics bodies, audio voices)
  - entity-count assert: no leaked rows after unload (FR-RUN-030)
```

**App-shell scenes (FR-RUN-030)** are scene states on a stack, driven by `core`'s application shell but implemented here:

```cpp
// SceneState = { AssetId scene; bool persistent_below; enter/exit hooks }
SceneStack& scenes = world.scene_stack();
scenes.push(SceneState{ .scene = "menu.scene"_guid,  .replace_all = true });
scenes.push(SceneState{ .scene = "gameplay.scene"_guid });       // pause pushes on top
scenes.pop();                                                     // back to gameplay
// Transitions drain event queues and flush pending commands before swap —
// menu → gameplay → pause → menu round-trips with zero leaked entities (tested).
```

**Play-in-editor (v0.5, FR-EDT-060):** the editor builds a fresh World from the scene document via the *same* `load_scene` path (there is exactly one scene-load code path — P6), ticks it, and discards it on Stop. Play→Stop→Play yields identical first-tick state (NFR-M-03, determinism harness). The editor's *live preview world* outside play mode is a second World instance mutated through this API (A11).

### 2.11 Prefabs (v0.5)

```cpp
// Authoring: editor serializes an entity sub-tree through reflection into the
// scene document as (template ref + override list). Cook flattens nested prefabs
// and emits a .mce template chunk (component columns + placeholder-handle table).
// Runtime: instantiate = bulk row copy + intra-prefab handle remap + overrides.
SpawnParams p;
p.overrides = { { "LocalTransform.position", Vec3{10, 0, 0} } };  // reflection path
Result<Span<Entity>, Error> e = world.instantiate_prefab("enemy_grunt"_guid, p);
```

No runtime inheritance (`IsA` rejected); live prefab *redefinition* is an editor-time feature (re-instantiate or diff-apply). "Open all doors of class X" is a tag component + system, not inheritance (research §9).

### 2.12 Gameplay programming model — the committed answer

**Native C++20 components + systems, full stop.** No Lua, no visual scripting, no coroutines in v1 (N2, research §11.2). The complete behavior toolbox:

| Tool | Shape | Tier |
|---|---|---|
| Components + systems | reflected structs + registered functions (§2.1–2.4) | MVP |
| FSM | enum-state data component + per-state systems filtered by `with<State value>` + `fsm.h` helper (transition tables, timed states, entry/exit) | MVP |
| Scripted beats | `Sequence` component + `SequenceRunner` system; steps are data (`wait/move_to/play_anim/emit/call`) | v0.5 |
| Timelines | cooked `TimelineAsset` track/clip chunks + `TimelinePlayer` component | v1.0 |

The `scripting` subsystem owns the SequenceRunner/TimelinePlayer *runtimes*; they are ordinary systems in `FixedUpdate_Post` built on ECS primitives — they introduce no second object model. C++20 coroutines were evaluated and rejected: coroutine frames allocate and do not serialize; the action list survives save/load (research §11.2). FSM example — the pattern the whole model hangs on:

```cpp
MCE_COMPONENT(EnemyBrain, EnemyState state = EnemyState::Idle; float stateTime = 0; Entity target;)

MCE_SYSTEM(enemy_think, mce::Phase::FixedUpdate_Pre, after("input_apply"))
void enemy_think(Frame& f) {
    f.query<EnemyBrain&, const LocalTransform&>().each([&](Entity e, EnemyBrain& b, const LocalTransform& t) {
        b.stateTime += f.dt();                       // uniform "time in state"
        switch (b.state) {
        case EnemyState::Patrol:
            if (b.stateTime > 2.0f) { b.state = EnemyState::Chase; b.stateTime = 0; }
            break;
        case EnemyState::Chase:
            if (f.world().alive(b.target)) { /* seek */ }
            else { b.state = EnemyState::Idle; b.stateTime = 0; }
            break;
        default: break;
        }
    });
}
// State is data => inspectable live in the editor, save-game-able, hash-tested (FR-ENT-090).
```

### 2.13 Save/persistence (FR-RUN-065 — owned with `core`)

`core` owns the file mechanics (`SaveWriter`/`SaveReader`, atomic temp-write + rename, `.bak` rotation, the `save://` mount — see `core` §2.9). This subsystem owns *what a save is*:

- **Header:** `{save_format_version, module_version, scene GUID, tick, playtime}`. The scene GUID re-loads the cooked scene chunk as the base world; the dump below re-applies live state on top (the Quake/GoldSrc "map + deltas" model, cited in existing-engines §6).
- **Body: `[Save]`-flagged reflection dump.** Component fields opt in with a `[Save]` marker on the reflected field (position, health, FSM state, inventory); non-flagged fields (asset handles, entity handles, cached derived data) are rebuilt on load — handles are re-resolved by the same GUID/remap pass as `load_scene` (§3.8). `module_version` mismatch is surfaced to the shell as "save from older module version" — load proceeds with defaults for new fields, never silently.
- **Physics body state is never saved** (velocities/contacts are re-derived from transforms on first physics tick — `physics` §1.2); audio mixer state likewise (only bus gains in the [Save] set).
- **Tier:** M6 (v0.5) — MVP games ship without saves; the [Save] marker is defined in the MVP reflection schema so components never need retrofitting.

---

## 3. Internal design — data structures & algorithms

### 3.1 Entity ids and the location index

- **Entity = `{u32 index, u32 generation}`.** The entity array holds `generation[i]` and a free-list head per recycled slot; `destroy` bumps generation. Capacity target 65,536 slots (NFR-T-04: 10k design ceiling per scene), growable.
- **Entity location index:** a sparse set — `EntityLocation { Table* table; uint32_t row; }` indexed by `entity.index`. This is the flecs-3/Bevy hybrid: tables for iteration, sparse sets for lookup. Two indirections for random access is the accepted price of columnar iteration.
- **Component ids:** `uint16_t`, dense-assigned at registration; table membership is a 256-bit mask (4 × u64). 256 component types is years of headroom (research decision 4).

### 3.2 Archetype tables and columns

```
Archetype (table):
  mask: 256-bit component set
  rows: uint32_t (live row count)
  columns[256]: Column { void* data; uint32_t* changed_ticks; uint32_t stride; }
  edges: add[ComponentId] -> ArchetypeId, remove[ComponentId] -> ArchetypeId
```

- **Dynamic tables first** (columns are growable arrays from a pool-backed arena; rows memmove on swap-remove) — research §3.5. Fixed 64 KiB **chunked** tables are a recorded v0.5+ optimization behind the same table API; chunking buys cache defragmentation and row reservation, not correctness, and stable row pointers are not required (binding forbids cross-frame pointers).
- Structural change = move the entity's full row old-table → new-table along the archetype graph edge (cache the edge; miss → intern a new archetype). O(row-size) is the accepted cost because structural change is deferred and bulk-applied (§3.4).
- **Row memory** comes from the ECS arena (pool of column blocks per `platform-engineering` §2.3), never CRT malloc, never the frame allocator (tables outlive frames).
- Component types are `trivially_copyable` and destructor-free (static_assert), so row moves are `memcpy` and hashing/serialization are bulk. The one sanctioned non-trivial case — a component referencing an engine resource — holds a `Handle<T>`, never a pointer.

### 3.3 Query cache and iteration

- `QuerySig = sorted ComponentId list + filters`, hashed; a map caches `Sig -> TableList`. A global `structure_version` counter (bumped by any sync-point application or table creation) invalidates all caches in O(1) — next query rebuilds its list.
- Iteration walks cached tables in **table-creation order** (deterministic for a given recorded op order — the property the determinism test leans on), then loops each table's columns linearly. `each()` is the convenience wrapper; bulk/table access is the performance path for hot systems (transform propagation, render extraction).
- Cost guardrail: NFR-T-01 (100k × transform+velocity < 1.5 ms) is a Tracy-verified CI benchmark with a synthetic 100k-entity world.

### 3.4 Command buffers and sync points

- Per-phase `Commands` records **POD records into the frame-linear allocator** (16 MiB slot budget from `core`): `op { Spawn | Destroy | Add | Remove | Set | SpawnN } + entity + payload (inline, ≤ payload size cap)`. No heap, no per-op allocation.
- **Sync point** = end of each phase. Application passes, in fixed order:
  1. **Spawn pass** — append all spawned rows (grouped by target archetype; `spawn_n` reserves in bulk — the 10k-spawn-storm path).
  2. **Mutation pass** — apply add/remove/set **in recorded order** (grouping is only a fast-path for consecutive identical ops; order preservation is what makes `state_hash` stable).
  3. **Destroy pass** — swap-remove rows, bump generations, recurse into `ChildOf` children (v0.5: destroying a parent orphans-destroy is the committed semantic).
  4. **Hook pass** — `on_add/on_remove/on_set` fire interleaved with the mutation pass at application time; hooks are the sanctioned place to touch `rhi-renderer`/`physics`/`audio-input-ui` (create/destroy mirrors). A hook may enqueue commands but never mutate structure directly.
  5. **Event swap** — double-buffered queues swap and hand readers an immutable span; late-registered readers get the two-frame guard so nothing is missed.
- Recorded order is exactly what the determinism test replays: same command stream ⇒ same table layout ⇒ same hash.

### 3.5 Event queues

`EventQueue<T>`: two frame-linear ring buffers + reader generation stamps (Bevy's two-frame guard simplified to phase granularity). Registry entries `{type, producer phase, consumer systems}` feed the editor's event-flow view (live per-type counts, FR-ENT-030). Capacity asserted (overflow is a programming error — events are gameplay-semantic, low-rate, and bounded by design; a system flooding events is a bug to surface loudly).

### 3.6 Change ticks

- Global `u32` tick counter (fixed-step count). Per-row tick columns updated by `set()`/query writes. `is_added` = tick == creation tick; `is_changed` = tick > system's last-run tick. Wraparound at 60 Hz ≈ 2.2 years — handled Bevy-style with a rotating window + same-frame sentinel; a footnote, not a feature (research §7).

### 3.7 Hierarchy propagation algorithm (v0.5)

```
seed:   entities with Changed<LocalTransform> OR new ChildOf
walk:   for each seed, ascend to root; mark subtree dirty (skip if already marked);
        depth-first from each root: world = parent_world * local; bump child tick
assert(debug): propagated-row count == dirty-subtree size (FR-ENT-040's
        "1 of 5k moves → exactly 1 re-interpolated" acceptance is this assert at
        the render-extract layer)
```

The `Children` parent-side index is a sparse set per parent, maintained by hooks — users never write it, so the Bevy dual-component desync footgun cannot exist by construction.

### 3.8 Scene and prefab instantiation path

Cooked scene chunk (produced by `assets` + `editor-tools` from the canonical document) layout:

```
[header: version, entity_count, archetype_count, CRC]
[per archetype: mask + component column payloads (16-byte aligned)]
[remap table: template entity index -> runtime Entity (filled at load)]
[asset reference table: GUID -> resolved AssetId (validated by `assets`)]
[optional: prefab template ref + override list]
```

Load is: mmap → validate (done by `assets`) → per-archetype bulk row append → remap pass rewrites `Entity`-typed fields (`ChildOf.parent`, `EnemyBrain.target`, prefab-internal references) → hook pass. No parsing, no per-entity allocation beyond rows. Scene save→cook→load→hash equals the editor-side reference hash (FR-ENT-060). Prefab instantiation is the same path with `N=1..n` replications plus the override list applied through reflection setters (FR-ENT-070).

**Animation remap (glTF node tracks, FR-R3D-010):** the scene chunk's remap table carries `node_index → Entity` for the source glTF node graph; cooked `CHNK_ANIM` tracks reference source node indices, so at load each track's target index resolves through the same table into an `Entity` handle stored on the clip instance. Playback is an `AnimationPlayer` component (`{clip: Handle<AnimClip>, time, speed, loop}`) advanced by a single `animation_sample` system in `Render_Pre` (alpha-aware): it samples the node tracks and writes `LocalTransform` pose deltas with `Changed<>` ticks, so hierarchy propagation (§3.7) and render extract see animated transforms exactly like hand-moved ones. MVP scope is the diorama prop: one player per entity, single-node TRS; blending/crossfade is a v0.5 sketch behind the same component.

### 3.9 Determinism machinery

- **Iteration order** is table order (a function of recorded op history) — stable given identical op streams; never derived from pointers, hashes of addresses, or unordered containers in sim code. `std::unordered_map` is banned in the sim; interning uses sorted vectors or a deterministic open-addressing map from `core`.
- **`World::state_hash()`**: walk tables in archetype-id order, hash each column's rows (FNV-1a over bytes) — this is the FR-RUN-040 harness primitive (10k headless ticks, Debug+Release, identical hashes). FP discipline (`/fp:precise` in determinism builds) is enforced by `core`'s build config; the ECS contributes by making state walk order total.
- **Composition point for per-subsystem hashes** (ARCHITECTURE §5.1, binding): `state_hash` is the *entry point*, not the whole artifact of record. Subsystems register per-domain hashers into it — `physics` (body poses), `audio` (command-stream hash, ADR-0012), `renderer` (draw-order key hash, separate test), `input` (post-quantization `InputState` stream) — and the `mce_devhost` harness composes them. The ECS guarantees the component-table half is total-ordered; each subsystem answers for its own mirror state.
- **Replay (FR-RUN-070)** records per-tick input snapshots; replay feeds the same command stream through the same pipeline. The determinism harness is also the replay harness — one mechanism, two requirements.

### 3.10 Memory map (per-milestone budget lines, reconciled in Tracy)

| Pool | Backing | Growth policy |
|---|---|---|
| Entity slots + generations | ECS arena | fixed 64k slots at MVP |
| Table columns | ECS pool arena (64 KiB blocks) | grow by block |
| Entity-location sparse sets | ECS arena | grow ×2 |
| Command buffer records | **frame-linear allocator** | reset each frame slot |
| Event buffers | **frame-linear allocator** | reset each frame slot |
| Query cache | ECS arena | invalidation, not free |

---

## 4. Threading & lifecycle

### 4.1 Threads

- **MVP through v0.5: single-threaded sim and render on the main thread.** The phase pipeline is explicitly ordered; there is no scheduler to debug and determinism is trivial to defend (research §4.2, platform-engineering §3.4). Tracy zones wrap every system (`MCE_SYSTEM` emits the zone) and every phase.
- **The audio thread never touches the World.** Gameplay→audio crosses only via the `audio-input-ui` SPSC command FIFO, fed by an audio-bridge system at a sync point (Doumler's real-time rules; FR-AUD-010).
- **Editor:** the editor's preview world and play-mode world are `World` instances on the main thread; ImGui reads them between ticks, never concurrently. Play-mode edit application happens in the editor's own phase, not inside sim phases.
- **Auto-parallel experiment (M3+, off by default):** derive per-system read/write sets from query signatures, run disjoint systems in `FixedUpdate_Pre/Post` on the `core` job graph behind a flag; the determinism CI test is the gate that keeps it honest. It is an experiment, not a promise — the committed model is ordered phases.

### 4.2 Lifecycle states

```
World: Empty → SceneLoading → Inactive → [Startup] → Active ⇄ (pause: phases gated,
       time_scale = 0) → Unloading → Empty
App shell (FR-RUN-030): SceneStack push/pop over the same lifecycle; on focus loss /
       minimize the RUN layer pauses the accumulator (FR-RUN-060); quit flushes saves
       then unwinds the stack asserting zero leaked entities in debug.
```

- **Pause/single-step/slow-mo** (FR-EDT-060, FR-RUN-050) are tick gates at the loop level + `time_scale`; both are deterministic because they gate *ticks*, not per-system behavior.
- **Hot-reload interplay (FR-AST-090):** outside play mode, editor re-cook → `unload_scene` → `load_scene` on the preview world (≤ 1 s budget at sample scale). Handles are generational, so a reload cannot alias stale references; asset-handle failure degrades to the magenta placeholder per FR-DX-050.

---

## 5. Dependencies

### 5.1 Upstream — what ecs-gameplay consumes

| Provider | What | Interface consumed |
|---|---|---|
| `core` | allocators (`FrameLinear`, pool arena), `Handle<T>`, `Result<T,Error>`, Tracy zones, asserts/crash handler, deterministic hash + open-addressing map, tick counter | `Frame` is built on `core` primitives; command buffers/events allocate from the frame-linear allocator |
| `reflection` | component type records (name, fields, size), property paths for overrides/inspector/save | `MCE_COMPONENT` registration; serialization of scene chunks (editor-side write) and save games |
| `audio-input-ui` | `InputState` resource snapshot (sampled per tick, FR-INP-030); audio command FIFO endpoint | consumed as resources + a bridge system in `FixedUpdate_Post` (post-physics so velocity-driven sfx/particles see final poses) |

**Note on `assets`:** `assets` depends on *this* subsystem (it consumes registered component metadata for the scene cook), never the reverse — `mce_ecs` includes no `mce_asset` headers (layering rule assets → ecs, ARCHITECTURE §2.2). Cooked scene/prefab chunks arrive as raw byte spans handed to `load_scene`/`instantiate_prefab` by the caller (runtime shell / PlaySession), which sits above both and does the `AssetId` → payload resolution; hot-reload notifications likewise reach the World as "reload this span" calls from above.

### 5.2 Downstream — what consumes ecs-gameplay

| Consumer | Relationship | Rule |
|---|---|---|
| `rhi-renderer` | Read-only extract in `Render_Pre`: declared queries over `WorldTransform` + render components; writes only to its own extract buffers | Renderer never mutates gameplay state; interpolation alpha comes from `Frame`. All GL confined to the RHI backend (binding seam). |
| `physics` | Registers systems into `FixedUpdate_Physics` (explicit ordering), owns body/collider mirror structures, reads/writes transform + rigidbody components, emits trigger/collision **events** (FR-PHY-035) | No scheduler privileges; physics is a well-behaved phase member. `on_add/on_remove` hooks create/destroy bodies. |
| `editor-tools` | Inspector reads/writes components via reflection + `try_get`/`set`; hierarchy view reads `ChildOf`; pipeline view reads the phase registry; event-flow view reads the event registry; play-mode builds a World via `load_scene` | Editor **owns the canonical scene document**; this subsystem sees only cooked chunks at runtime. Editor never touches table internals. |
| `scripting` | SequenceRunner/TimelinePlayer are plain systems over ECS primitives | Introduces no second object model; no VM (N2). |
| `core` (RUN shell) | Scene states / pause / focus / quit are SceneStack operations on this subsystem's API | RUN owns the loop; ecs-gameplay owns what a tick *is*. |

**The dependency arrow the binding enforces:** nothing above the RHI may call GL, and nothing in gameplay may call SDL3 or ImGui — both are CI-greppable (`mce::platform` owns SDL3; ImGui is invisible to `mce_runtime` at the CMake target wall, FR-UI-048).

**Shared interfaces hosted here (fix F5):** `mce::debug::IDebugDrawSink` is defined in `mce_ecs` — *implemented* by `rhi-renderer` (`DebugDraw`), *filled* by `physics` during its extract contribution, registered as a World resource at composition. Direction of includes stays `physics ↓ ecs` and `render ↓ ecs`; the sink is the sanctioned data path between the two L2 subsystems, so neither grows a horizontal edge. Any future interface that two L2 subsystems must exchange gets the same treatment: define it in `mce_ecs`, invert the call.

---

## 6. Test strategy

Mandatory per the binding test policy; every item names its FR.

1. **ECS unit tests** (FR-ENT-010/020/030):
   - stale-handle lookup fails without aliasing (destroy → respawn slot → old generation miss);
   - command-buffer application order (recorded order preserved across passes; spawn-storm 10k spawns applies in bulk at sync point);
   - query stability: a system's query result set is immutable across its own body (structurally-mutating ops deferred — grep-testable + behavioral);
   - event double-buffer: reader sees complete, immutable lists; late reader registration misses nothing; no partially-written list visible;
   - hook invocation exactly once per add/remove/set at application time.
2. **Change detection + hierarchy** (FR-ENT-040/050, v0.5): moving 1 of 5k entities re-interpolates exactly that entity (debug-assert count in render extract); rotating a parent moves children same-frame; dirty propagation touches only changed subtrees (assert).
3. **Determinism harness** (FR-RUN-040/070, NFR-F-04): headless 10k-tick run, `state_hash()` after each 1k ticks, identical across 3 consecutive runs **and** Debug vs Release; input-stream replay reproduces final hashes. CI from M1 on every PR; zero GL calls verified by symbol/leak grep (FR-RUN-025). A scripted odd-render-rate input sampler proves tick-edge determinism alongside FR-INP-030's test.
4. **Performance gates** (NFR-T-01, FR-ENT-010): 100k entities × transform+velocity iterate+write < 1.5 ms single-thread, Tracy-verified, run in the perf job (not per-PR; per milestone).
5. **Serialization round-trips** (FR-ENT-060/070): scene save→cook→load→hash equals reference; prefab override round-trip; nested-prefab flatten equals manually-composed scene hash; `.mce` negative cases (bad magic/truncation/CRC/version-ahead) surface as diagnosable `Result` errors from `assets` through `load_scene` — never a crash.
6. **Inspector/reflection integration** (FR-EDT-020, FR-DX-020): register a 3-field sample component in the test target; inspector renders and round-trips edits with zero editor code.
7. **FSM/sequence semantics** (FR-ENT-090/100): state time resets on transition; timed transitions fire on the exact tick with a synthetic clock; a `Sequence` round-trips through save/resume.
8. **Sanitizers + fuzzing:** ASan/UBSan per PR; the scene-chunk loader path fuzzed through the `.mce` reader's negative test corpus.
9. **Sample ladder as regression subjects** (FR-DX-070): `02_boids` (M1) and `03_platformer` run 300 headless fixed steps in CI with a state-hash snapshot; they are the living determinism and API-surface tests.

---

## 7. Risks & mitigations

| # | Risk | Mitigation |
|---|---|---|
| 1 | **Archetype fragmentation** (component soup → many tiny tables; the classic archetype-model failure, research §11.4) | Data-over-tags culture written into the component design guide; `World::metrics()` publishes table count + occupancy in the editor and Tracy from M1; fragmentation is a *visible* metric from day one. |
| 2 | **Command-buffer learning curve** — deferred mutation feels indirect; users (the author-as-user) will want immediate `add<T>`. | The sync-point model is documented as the engine's central rule; an editor "deferred ops" debug view shows pending commands per phase; `World::set` on *existing* components is immediate, so the deferral surface is only structural change. |
| 3 | **ECS scope creep** — relationships/pairs, observers, runtime prefab inheritance, coroutines all *look* like small additions. | Each has a recorded trigger condition (4th concrete relationship; observer cascade need; shared-mutable-prefab need). Features ship when a fourth concrete use case appears, not when reference implementations look richer. |
| 4 | **Determinism regressions** from the auto-parallel experiment (a v0.5/ROADMAP-M6 flag) or an unordered container slipping into sim code. | Flag-gated experiment + the FR-RUN-040 CI hash test on every PR; clang-tidy check (from ROADMAP M1) + code review rule banning unordered containers and address-derived ordering in `mce::ecs`. |
| 5 | **Archetype move cost on structural storms** (O(row) per op) — naive application could hurt the bullet-storm case the archetype model is worst at. | Bulk spawn pass + `spawn_n`; deferred application amortizes to a single sync point; 10k-spawn storm is an explicit acceptance test (FR-ENT-020). If profiling still shows cost, chunked tables (§3.2) are the recorded optimization, not a redesign. |
| 6 | **Static-init ordering** for macro-registered components/systems on MSVC. | No cross-TU static registration order dependencies: component ids are assigned at explicit `register_components` calls (function-order controlled), `MCE_COMPONENT` only emits type metadata; `constinit`/function-local statics where a registry must exist pre-`main`. |
| 7 | **Tick-column memory growth** (4 B/instance across all components) and tick wraparound. | Measured and budgeted (§3.10); wraparound handled Bevy-style with a rotating window + sentinel (correctness footnote); per-table tick elision remains a fallback if Tracy shows cache cost. |
| 8 | **Play-mode budget risk** (enter/exit ≤ 1 s, NFR-M-03) if scene load does per-entity work. | The instantiation path is bulk-by-archetype by construction (§3.8); play-mode entry is the same `load_scene` path, measured from the "Run in player" button onward (ROADMAP M3 — finding F4 moved it out of M1). |
| 9 | **The editor/runtime seam blurring** — convenience pressure to let the editor mutate scenes directly or ImGui leak into runtime paths. | Structural: the runtime sees only cooked chunks; the canonical document lives in `editor-tools`. CMake target wall + CI symbol scans enforce ImGui/SDL/JSON absence (FR-UI-048, FR-AST-010). |
| 10 | **Solo-dev bandwidth** — this is the largest single M1 item (research §13.3) and competes with the renderer for the same weeks. | MVP scope is frozen to the MVE checklist (tables, queries, command buffers, phases, events, scene save/load — no hierarchy, observers, or parallelism); hierarchy/change-detection/prefabs are v0.5 line items, not MVP temptations; cut ladder rungs 1–2 absorb pressure before any ECS scope is cut. |

---

## 8. Milestone mapping (from research §13.3, binding tiers)

Tier names with ROADMAP milestone IDs (ARCHITECTURE §10.1 is the mapping authority; re-baselined per finding F4: reflection moves to M2).

- **ROADMAP M1 (MVP):** archetype tables + queries + location index, phases + command buffers + sync points, event queues, component registration with hand-rolled type metadata (reflection-bounded registration upgrades in place when `mce_refl` lands), chunk-payload scene load/save, starter kit, determinism harness + headless mode.
- **ROADMAP M2 (MVP):** reflection-bounded registration (lands with `mce_refl`, finding F4); `World::metrics()` surfaced in the inspector.
- **ROADMAP M3 (MVP):** FSM helper component + the `FixedUpdate_Post` bridge systems where physics/audio hooks are proven (the `03_platformer` sample exercises both).
- **v0.5 (ROADMAP M5b):** play-in-editor World construction (PlaySession `enter`/`exit` through the same `load_scene` path); interpolation-driven render extract.
- **v0.5 (ROADMAP M6):** change detection (tick columns + filters), `ChildOf` hierarchy + dirty propagation, particle system (`ParticleEmitter`, §2.9 addendum) + save-game reflection dump (§2.13).
- **v0.5 (ROADMAP M6–M7):** prefab cook/instantiate + overrides; auto-parallel experiment behind a flag.
- **v1.0 (ROADMAP M8–M9):** timeline assets + player (with `scripting`); runtime-UI bridge design (separate system, events-only boundary).

## References

Skypjack, *ECS back to basics*; flecs v3, Bevy ECS, EnTT documentation (studied, never linked); Timothy Ford, "Overwatch Gameplay Architecture and Netcode," GDC 2017; Bob Nystrom, *Game Programming Patterns* (Component, Event Queue, Dirty Flag, Update Method); Glenn Fiedler, "Fix Your Timestep," Gaffer On Games; Catherine West, RustConf 2018; Bitsquid/Stingray "Building a Data-Oriented Entity System" series; Jason Gregory, *Game Engine Architecture* 3rd ed. ch. 14; Timur Doumler, *C++ in the Audio Context*, CppCon 2015; Unity DOTS documentation (chunked archetypes, the ergonomic-friction cautionary record); repo-internal: `docs/research/ecs-gameplay.md`, `docs/research/platform-engineering.md`, `docs/REQUIREMENTS.md`.
