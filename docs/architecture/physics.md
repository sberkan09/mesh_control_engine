# MCE Architecture — Physics (2D + 3D)

**Project:** Mesh Control Engine (MCE) · **Document class:** Subsystem architecture · **Date:** 2026-10-06
**Subsystem:** `physics` · **CMake targets:** `mce_physics2d` (hand-written), `mce_physics3d` (embedded Jolt 5, v0.5)
**Status:** Architecture design. Binding inputs: `docs/REQUIREMENTS.md` §3.6 (FR-PHY-010..100), the technology decision's binding from-scratch list (2D physics is hand-written; Jolt 5 is the pre-approved 3D exception valve), the fixed-timestep/determinism bindings (FR-RUN-020/040), and `docs/research/ecs-gameplay.md` §4/§5 (phases, command buffers, hooks) + `docs/research/existing-engines.md` L6 (fixed timestep is settled science). Resolution A10 of REQUIREMENTS Appendix A delegates the hand-written-vs-Jolt vehicle choice for FR-PHY-080 to this document; the decision is recorded here as Decision D4 and must be mirrored into `docs/adr/0003-embed-jolt-for-3d.md` before the first `JPH` include lands (ROADMAP M5b timebox).

---

## 0. Decision summary (read this first)

| # | Decision | Commitment |
|---|---|---|
| D1 | **Custom vs embedded** | 2D physics is **fully hand-written** in `mce_physics2d` (Catto's Box2D GDC material as the reference design; Box2D itself never linked at runtime — it serves as the test-time oracle, D9). 3D physics is **embedded Jolt 5** in `mce_physics3d` behind a facade, admitted by the pre-approved exception (REQUIREMENTS N3, FR-PHY-080, Appendix A/A10) and recorded by ADR. One world per dimension; no fake common base class unites them. |
| D2 | **Transform authority** | For entities with a live physics body, the **physics world is authoritative** for pose (and velocity of dynamic bodies) during play; physics mirrors pose back into `LocalTransform` each fixed tick, bumping the change tick. Direct `LocalTransform` writes to such entities are a debug assert. Teleports go through `World2D::teleport()`. In editor edit mode there is **no physics world at all** — `LocalTransform` is the plain truth. |
| D3 | **Sim ownership** | Physics stepping lives entirely in one explicit pipeline slot, the `FixedUpdate_Physics` phase, single-threaded through v1.0. Structural changes (body create/destroy) happen only at phase-end sync points via ECS command-buffer hooks — never mid-step, never mid-iteration. |
| D4 | **3D vehicle = Jolt 5, embedded, contained** | The `mce_physics3d` target is the only target in the repo allowed to include `Jolt/*.h`; a CI grep enforces it exactly like the GL-leak check. Gameplay code sees only `World3D` + MCE components/events. Jolt runs single-threaded with MCE's fixed 60 Hz step. |
| D5 | **One broadphase, one solver** | 2D broadphase is a dynamic AABB tree (Catto's BVH) from M1 — no MVP throwaway grid, no second implementation to keep alive. Solver is sequential impulse with warm starting, Box2D default constants, split-impulse position correction (pseudo-velocities; Baumgarte off). Stacking is explicitly out of MVP scope (REQUIREMENTS FR-PHY-060 note, cut-ladder rung 6). |
| D6 | **Character controllers are query-driven, not solver-driven** | The 2D controller is a kinematic body moved by explicit swept shape-casts against the world (move-and-collide), not an impulse-solved dynamic body. The 3D controller maps to Jolt's `CharacterVirtual` driven by the same shape of MCE-owned intent. Ground/wall/ceiling flags, slopes, one-way platforms, and platform carry are controller responsibilities. Coyote time / jump buffering are gameplay's, not physics'. |
| D7 | **No JPH/Box2D types above the physics targets** | Components are MCE plain structs; events are MCE `EventQueue<T>` types; queries take/return MCE `Vec2`/`Entity`. Body ids are physics-internal generational handles never stored in components or serialized. |
| D8 | **Physics is in the determinism contract** | Every physics code path is `/fp:precise`, iteration order is fully determined (entity-id order on import, body-id order on solve/islands, sorted hits on queries), and the FR-RUN-040 hash harness covers physics-driven transforms from M1 (2D) and v0.5 (3D poses). |
| D9 | **Box2D v3 is a test-time oracle, never a runtime dependency** | Box2D v3 (2024) is linked **only** into `test_physics2d` — identical deterministic scenes are stepped in both engines and compared within documented tolerance, mirroring the math library's DirectXMath-oracle policy. No runtime, editor, cook, or shipped target ever links it; a CI symbol grep (`b2`/`Box2D`) outside `test_physics2d` enforces this. Requires ADR-0016 (test-time-only dependency class, same as glslangValidator) before the first test include. |

---

## 1. Responsibilities & non-responsibilities

### 1.1 Responsibilities

The physics subsystem owns:

1. **The two physics worlds.** `World2D` (custom solver, MVP) and `World3D` (Jolt embed, v0.5). Each world owns its internal body storage, broadphase, narrowphase, solver state, and pair/event bookkeeping. Worlds are created per runtime world (game world, play-mode preview world) and destroyed wholesale with it.
2. **The ECS sync boundary.** Importing component state into body state, stepping at the fixed rate, mirroring poses back into `LocalTransform`, and emitting contact/trigger/controller events — all inside the `FixedUpdate_Physics` phase slot.
3. **Component set.** `RigidBody2D`, `Collider2D`, `CharacterController2D`, `CharacterGround2D` (output), and the v0.5 additions (`Joint2D` family, 3D equivalents). These are reflected plain structs: inspector-editable, scene-serializable, save-game-able — with zero physics-library includes needed to define them (shapes are plain structs over core-math payloads, defined in `components.hpp`).
4. **Queries.** Raycasts, shape casts, and overlap tests against both worlds, deterministic and allocation-free, callable from gameplay systems and editor tools.
5. **Character controllers.** The 2D swept move-and-collide controller (MVP, FR-PHY-030) and the 3D capsule controller over `CharacterVirtual` (v0.5, FR-PHY-080), including slopes, one-way platforms, moving-platform carry, and the camera-collision sweep used by UC-B.
6. **Collision filtering.** The per-game layer/mask table (authored as data), applied identically to bodies, queries, and controllers in both dimensions.
7. **Debug surface.** Collider/broadphase/contact debug-draw output for editor-tools overlays and the dev overlay; a state-hash hook for the determinism harness.
8. **Cook-side collider preparation.** The shape vocabulary and its cooked payload format (mesh colliders reference cooked mesh chunks; 3D `MeshShape`/convex hulls + ActiveEdges are prepared in the cook tool where Jolt is also linked).

### 1.2 Non-responsibilities (with who owns them instead)

| Not physics | Owner | Boundary note |
|---|---|---|
| Damage, pickups, kill zones, FSM transitions | ecs-gameplay (gameplay systems) | Physics emits `TriggerEnter/Exit` events; gameplay decides what they mean. |
| Coyote time, jump buffering, input edge detection | ecs-gameplay (player systems) | Physics reports instantaneous ground flags only; grace windows are gameplay timers over those flags. |
| Rendering the world | rhi-renderer | Physics only fills a debug-draw sink (line lists, shape outlines) that the renderer/editor draws. Physics never calls RHI or GL. |
| Transform hierarchy propagation | ecs-gameplay | Physics writes `LocalTransform` (bumping its change tick); the existing dirty-propagation system does the rest. |
| Render interpolation | core / ecs-gameplay (extract step) | Physics maintains only its own prev/cur pose ring internally; the engine-wide prev/cur transform contract is owned above physics (see §3.4). |
| Audio reactions | audio-input-ui | Consumes gameplay events; physics emits no audio events directly. |
| Save-game format | core (SaveWriter/SaveReader mechanics, core §2.9) + ecs-gameplay ([Save]-flagged reflection dump, ecs-gameplay §2.13) | Body state is not saved; a save restores the scene's authored bodies and re-derives velocities/contacts on the first physics tick. |
| Mesh asset parsing/cooking | assets | Physics consumes cooked chunks; it never parses glTF/PNG/JSON. Convex-hull and ActiveEdges computation runs inside the cook tool (cook-time dependency only). |
| Ragdolls, vehicles, cloth, soft bodies, destruction, GPU physics, networked physics | **Nobody** | FR-PHY-100 / Won't. Re-entry only by slip note. |
| Multi-threaded solving | **Nobody through v1.0** | Single-threaded sim is a determinism binding; parallel islands are explicitly not planned (see §4.4). |

### 1.3 Tiering (per REQUIREMENTS §6; milestone IDs per ARCHITECTURE §10.1)

- **MVP (ROADMAP M3, Artifact A) — cut-ladder rung 6 is the default shape:** `World2D` — static/kinematic/dynamic bodies, AABB + circle + convex polygon colliders, contact manifolds, **dynamic-vs-static solver** (kinematic bodies driven by the swept controller), swept 2D character controller (ground/wall/ceiling flags, slope limits, one-way platforms, moving-platform carry via velocity accumulation), sensors/triggers, queries, dynamic AABB tree broadphase, events, debug draw, determinism hooks. **Stacking, joints, and warm starting are out** (see next bullet).
- **MVP stretch (ROADMAP M3.5, may slip whole into v0.5 as the tier's shock absorber):** solver hardening only — warm-started sequential impulse, island traversal, restitution tuning, platform-carry promotion into the solver path, and the 20 hand-computed contact-manifold unit cases. **FR-PHY-035/040 close at M3**, not here: sensors/triggers and queries are Must FRs in the committed M3 shape above, and Must FRs may not ride a milestone that is allowed to slip whole. M3.5 carries only FR-PHY-010/020 hardening acceptance. Warm starting + islands make basic small stacks (2–3 boxes — the M3.5 demo) stable; robust stacking (towers, chains) waits for joints at v0.5.
- **v0.5 (ROADMAP M5b for the Jolt items, M6 for the rest, Ember Step full + Ashen Court):** joints (distance, revolute; spring optional), sleeping + islands, 2,000-body budget validation (FR-PHY-050), compound colliders via child entities, `World3D` over Jolt (`CharacterVirtual`, camera sweeps, mesh colliders) — Jolt lands in **M5b** (the M5a/M5b split, ROADMAP finding F28), the joint/sleeping/budget work in **M6**, SIMD broadphase hotspots only if Tracy demands (FR-PHY-070).
- **v1.0 (only if a game demands):** dynamic 3D knockable props via the same Jolt embed (FR-PHY-090, Could). UC-B must remain shippable without them.

---

## 2. Public interface

Two audiences, one rule: **gameplay code sees `mce::physics` types only** — never solver internals, never `BodyId` in serialized data, never a physics-library header.

### 2.1 Components (declared in `mce/physics/components.hpp`, registered by the physics module)

Shapes are physics-module plain structs over core-math payloads (`Vec2` only), so a game module (and the reflection system) can name colliders without touching the physics targets. Type names deliberately avoid `Box2D`/`Circle2D` spellings — those would collide with the Box2D oracle's vocabulary in discussions, tests, and grep output:

```cpp
// mce/physics/components.hpp  — JPH-free, solver-free, reflection-registered
namespace mce::physics {

enum class BodyType : uint8_t { Static, Kinematic, Dynamic };

// Body-level config. One entity = one body. Pose/velocity are NOT here (D2).
MCE_COMPONENT(RigidBody2D,
    BodyType  type          = BodyType::Static;
    float     gravityScale  = 1.0f;
    float     linearDamping = 0.0f;
    bool      fixedRotation = false;
    uint8_t   layer         = 0;        // index into the game's collision-layer table
    uint16_t  mask          = 0xFFFF;   // layers this body collides with
)

// One collider per body at MVP; compound shapes arrive in v0.5 as child entities
// carrying Collider2D (§2.2). shape holds core-math variants only.
MCE_COMPONENT(Collider2D,
    ColliderShape shape;                  // Box{half, offset, rot} | Circle{radius, offset} | Convex{hull}
    float  density     = 1.0f;            // 0 on Static bodies = massless (allowed)
    float  friction    = 0.6f;
    float  restitution = 0.0f;
    bool   isSensor    = false;           // FR-PHY-035: overlap events, no response
    bool   oneWay      = false;           // FR-PHY-030: blocks downward sweeps only
)

// Serialized field names in `.mcscene`/prefab JSON are the snake_case forms of the
// fields above (`is_sensor`, `one_way`, `gravity_scale`, …) — the C++ identifiers
// here are canonical in code; the mapping is stated once, here (CF20 register).

// Opts the entity into the swept controller (FR-PHY-030). Implies a Kinematic body.
MCE_COMPONENT(CharacterController2D,
    float maxSlopeAngleDeg  = 45.0f;
    float snapDownDistance  = 0.05f;      // metres; keeps ground contact on stair descents
    float skinWidth         = 0.005f;
)

// Controller OUTPUT, written by physics each tick, read by gameplay. Read-only
// in the inspector during play (grey-backed); never authored in edit mode.
MCE_COMPONENT(CharacterGround2D,
    ControllerFlags flags;                // Ground | WallLeft | WallRight | Ceiling | OnMovingPlatform
    Vec2   groundNormal;
    Vec2   platformVelocity;              // carry velocity of the platform underfoot
    Entity groundBody;                    // Entity handle, null when airborne
)

// v0.5 sketch — joints attach via child entities (JointOf relationship component,
// per ecs-gameplay §8 decision 10): the joint connects this entity to its parent.
MCE_COMPONENT(Joint2D,                    // v0.5
    JointKind kind;                       // Distance | Revolute
    Vec2  localAnchor;
    float stiffness   = 0.0f;             // spring (0 = rigid)
    float damping     = 0.0f;
    float maxForce    = 1e6f;
)

struct ColliderShape {                    // std::variant, core-math payloads only
    Box2    box;      // half extents + offset + rotation
    Circle2 circle;   // radius + offset
    Convex2 convex;   // up to 16 vertices, CCW, computed at cook or authored
};

} // namespace mce::physics
```

Notes:

- `CharacterGround2D` is engine-written data, but it stays a normal component so it is inspectable, tick-tracked (change detection comes free), and hashable by the determinism harness.
- The `entity → BodyId` mapping is an engine-internal sparse set inside `mce_physics2d`. It is deliberately **not** a component: it is non-serialized, non-inspectable rebuildable index state, and putting physics-internal ids into the ECS would leak body lifetime into save files and prefab diffs.
- Compound colliders (v0.5): a `ChildOf` entity with `Collider2D` and no `RigidBody2D` attaches its shape to the nearest ancestor body at hook time. This reuses the hierarchy instead of inventing a fixture-list component, matches the flecs/Bevy instinct of "composition via relationship," and keeps the inspector story uniform (each collider is still inspectable on its own entity).

### 2.2 The world facades

```cpp
// mce/physics/world2d.hpp — the only 2D type gameplay code touches
namespace mce::physics {

using BodyId = detail::GenerationalId;    // physics-internal; never serialized

class World2D {
public:
    World2D(const PhysicsConfig& config, core::IAllocator& persistent,
            ecs::EventRegistry& events);

    // ---- stepping (called ONLY by the physics phase system, §3.1)
    void step(float fixedDt);                        // import → solve → export → events

    // ---- body mutation (called from FixedUpdate systems; asserted elsewhere)
    void set_linear_velocity(Entity e, Vec2 v);      // kinematic + dynamic
    void apply_impulse (Entity e, Vec2 impulse, Vec2 worldPoint);
    void teleport      (Entity e, Vec2 pos, float rotRadians);  // resets sweeps + pose history
    void set_enabled   (Entity e, bool active);      // cheap pause of a body, keeps broadphase slot

    // ---- character controller (FR-PHY-030). `delta` = intended displacement this tick.
    MoveResult2D move_and_collide(Entity e, Vec2 delta);

    // ---- queries (FR-PHY-040). Read-only, any phase, zero heap allocation:
    //      results land in caller-provided frame-allocated buffers.
    std::optional<RayHit2D>    raycast     (Vec2 origin, Vec2 dir, float maxDist,
                                            const QueryFilter& f = {});
    void                       raycast_all (Vec2 origin, Vec2 dir, float maxDist,
                                            core::SmallVector<RayHit2D>& out,
                                            const QueryFilter& f = {});
    std::optional<RayHit2D>    shape_cast  (const ColliderShape& shape, Vec2 from, Vec2 to,
                                            const QueryFilter& f = {});
    bool                       overlap_point(Vec2 p, const QueryFilter& f = {});
    uint32_t                   overlap_aabb(math::AABB2 box,
                                            core::SmallVector<Entity>& out,
                                            const QueryFilter& f = {});

    // ---- editor/test surface
    void debug_draw(DebugDrawSink& sink, DebugDrawFlags flags) const;  // editor-tools overlay
    uint64_t hash_state(uint64_t seed) const;                          // FR-RUN-040 harness

    struct Stats { uint32_t bodies; uint32_t contacts; uint32_t islands; }; // dev overlay
    Stats stats() const;
};

struct QueryFilter {
    uint16_t layerMask = 0xFFFF;
    std::span<const Entity> exclude;          // e.g. self; frame-allocated
    // optional per-hit veto: return false to skip a candidate (used by drop-through)
    bool (*predicate)(Entity hit, void* user) = nullptr;
    void* user = nullptr;
};

struct RayHit2D   { Entity entity; Vec2 point; Vec2 normal; float t; };
struct MoveResult2D {
    Vec2  position;        // resolved pose this tick
    ControllerFlags flags; // Ground | WallLeft | WallRight | Ceiling | OnMovingPlatform
    Vec2  groundNormal;
    Entity groundBody;     // the platform/ground underfoot this tick, else null
    Vec2  platformVelocity;
    bool  hitCeiling;
};

} // namespace mce::physics
```

`World3D` (v0.5) mirrors these signatures — same method names, 3D math types, plus exactly two additions:

```cpp
class World3D {                                   // v0.5, facade over mce_physics3d
public:
    void step(float fixedDt);
    void set_linear_velocity(Entity e, Vec3 v);
    void teleport(Entity e, Vec3 pos, Quat rot);

    // CharacterVirtual-driven controller (FR-PHY-080)
    MoveResult3D move_character(Entity e, Vec3 delta, const ControllerParams3D& p);

    // Camera collision: sweep a sphere from head position along the boom (UC-B gate:
    // "camera never clips through level geometry"). Pulls the returned t into the camera rig.
    std::optional<ShapeHit3D> camera_sweep(Vec3 from, Vec3 to, float sphereRadius);

    std::optional<RayHit3D> raycast (Vec3 origin, Vec3 dir, float maxDist,
                                     const QueryFilter& f = {});
    std::optional<ShapeHit3D> shape_cast(const Sphere& s, Vec3 from, Vec3 to,
                                         const QueryFilter& f = {});
    void debug_draw(DebugDrawSink& sink, DebugDrawFlags flags) const;
    uint64_t hash_state(uint64_t seed) const;
};
```

The mirror-signature rule (D7's enforcement mechanism) is tested at compile time by a header-only test that names both worlds' methods with identical spelling — 2D/3D API drift is a compile error, not a code-review catch.

### 2.3 Events

Typed, double-buffered `EventQueue<T>` types (ecs-gameplay decision 8), registered once by the physics module with explicit producer (`physics_step`) and documented consumers:

```cpp
namespace mce::physics {
struct TriggerEnter  { Entity sensor; Entity other; };   // exactly once per entry (FR-PHY-035)
struct TriggerExit   { Entity sensor; Entity other; };   // exactly once; also on body destroy
struct ContactBegin2D{ Entity a; Entity b; Vec2 point; Vec2 normal; float approachSpeed; };
struct ContactEnd2D  { Entity a; Entity b; };
}
```

- Pair diffing, not per-tick emission: the world keeps an ordered set of open sensor pairs; after each step it emits *added* as `TriggerEnter` and *removed* as `TriggerExit`. Deterministic because the set is a sorted vector keyed by `(minIndex, maxIndex)` of body ids — never a hash map.
- Destroying either body (via command-buffer hook) synthesizes `TriggerExit` for its open pairs first, so gameplay never observes a trigger stuck on.
- `ContactBegin2D/End` exist for gameplay feel (hit sounds, dust) and audio-input-ui reactions; they are capped per tick by a config constant with a convar, so a particle-vs-floor storm cannot flood the queue (see §7, risk R5).

### 2.4 What gameplay code looks like against this API

```cpp
// Player movement — Artifact A. Coyote time lives HERE (gameplay), not in physics (D6).
// Controller calls land in FixedUpdate_Pre (physics is idle there; §3.1).
MCE_SYSTEM(PlayerMove, mce::Phase::FixedUpdate_Pre, after("input_apply"))
void player_move(Frame& f) {
    auto& world  = f.resource<physics::World2D>();
    const auto& in = f.resource<InputState>();

    f.query<CharacterGround2D&, const PlayerConfig&>()
     .each([&](Entity e, physics::CharacterGround2D& g, const PlayerConfig& cfg) {
        const bool grounded = (g.flags & physics::ControllerFlags::Ground) != 0;

        if (in.jump_pressed && (grounded || coyote_ok(e)))   // fixed-field fast path (audio-input-ui §2.1)
            jump(e);                                         // sets velocity below

        const Vec2 delta = intended_delta(e, in, g) * f.dt;
        const auto r = world.move_and_collide(e, delta);              // swept, resolved this tick
        write_flags(e, r);                                            // coyote timer updates
    });
}

// Pickup — trigger event consumer; physics knows nothing about health.
// Events produced in FixedUpdate_Physics are readable in FixedUpdate_Post (double-buffered).
MCE_SYSTEM(Pickups, mce::Phase::FixedUpdate_Post)
void pickups(Frame& f) {
    for (const auto& ev : f.events().read<physics::TriggerEnter>())
        if (f.has<Pickup>(ev.sensor)) { collect(ev.other); f.commands.destroy(ev.sensor); }
}
```

---

## 3. Internal design

### 3.1 Where physics sits in the fixed tick

Per the phase pipeline (ecs-gameplay §4.2), physics occupies one slot and touches the ECS only at defined moments:

```
FixedUpdate_Pre        gameplay systems: read CharacterGround2D, write intents
                       (set_linear_velocity / apply_impulse / move_and_collide calls land here)
  [sync point]         command buffers applied; hooks run → body create/destroy (§3.3)
FixedUpdate_Physics    the single registered system physics_step(Frame&):
    1  IMPORT   diff component state vs world (change-tick driven):
                  added/removed RigidBody2D & Collider2D   → create/destroy bodies (bulk, id order)
                  changed Collider2D (shape/material/filter) → rebuild shape in place
                  set/changed RigidBody2D fields            → type/damping/gravity/filter update
                  teleports and velocity writes already queued by Pre-phase calls
    2  STEP     fixed h = 1/60 (MVP, single step). v0.5: optional 2× substeps (h=1/120),
                config-gated, off by default so the determinism hash never silently changes.
    3  EXPORT   write poses → LocalTransform (bump change tick);
                write CharacterGround2D for controller entities;
                controller-flag components only, velocities stay internal (D2)
    4  EVENTS   sensor-pair diff + contact begin/end → double-buffered event queues
  [sync point]         command buffers applied; hooks may create/destroy bodies for NEXT tick
FixedUpdate_Post       hierarchy propagation (physics-written LocalTransforms now flow to
                       WorldTransform), trigger consumers, AI, animation...
Render                 extract reads LocalTransform + prev/cur contract (§3.4) with alpha
```

Import runs in entity-id order and export in body-id order; both orders are total, so the tick is reproducible (D8). Physics never runs queries-into-commands mid-step: gameplay reads the world in `Pre`, mutates via API calls that land in body arrays immediately (physics is idle then), and structural change is always deferred through the command-buffer sync point — the same discipline every other system follows, so no special physics rule exists.

### 3.2 `mce_physics2d` internals (MVP, full design)

Reference frame: Erin Catto's Box2D GDC slide series (the sequential-impulse line: *Sequential Impulse* through *Soft Constraints: Fulfilling the Dream*, plus his dynamic-AABB-tree and character-body talks) and the Box2D 2.x source as the canonical walking skeleton. Hand-written; Box2D is never linked at runtime, but its constants and algorithm shapes are the design oracle — and, per D9, Box2D v3 is additionally linked into the test executable only as a behavior oracle (ADR-0016, test-time dependency class), so solver changes can be validated against Catto's production solver rather than against hope.

**Body storage — SoA over generational ids.** Bodies live in physics-owned structure-of-arrays indexed by `BodyId{u32 index, u32 gen}`:

```cpp
struct BodySoA {
    // hot arrays, sized to live-body capacity, pool-allocated at world creation
    Vec2   pose[N];      float rot[N];        // integrated each step
    Vec2   vel[N];       float w[N];          // linear + angular velocity
    Vec2   force[N];     float torque[N];     // accumulated per tick, cleared after
    float  invMass[N], invI[N];               // 0 for static/kinematic
    float  restitution[N], friction[N], damping[N];
    uint16 flags[N];                          // static/kinematic/dynamic, sleeping, sensor-only
    uint8  layer[N];     uint16 mask[N];
    Entity owner[N];                          // back-reference for events/queries
};
```

Why not store bodies in the ECS as components? Three reasons, in order of weight: (1) the solver wants its own dense layout and sleep/island bookkeeping, not archetype churn when `RigidBody2D` fields change; (2) pose/velocity must NOT be ECS-authoritative (D2) — storing them in components would create a two-writer problem with the mirror-back; (3) generational `BodyId` + a physics-internal `entity_index → BodyId` sparse set keeps stale-handle semantics identical to the ECS's own (stale handle fails, never aliases — same invariant as FR-ENT-010, unit-tested the same way).

**Broadphase — dynamic AABB tree, one implementation (D5).** Catto's BVH: 32-bit node pool, fat AABBs with 0.1 m margin plus a per-motion predictive buffer, lowest-cost leaf insertion, balance-on-update (b2DynamicTree shape). It serves the single-screen MVP and the 2,000-body v0.5 budget (FR-PHY-050) alike; shipping a simpler MVP broadphase and a second real one would violate the one-path rule (P6) for ~600 lines saved. The tree is walked recursively for raycasts (closest-hit stack walk) and AABB queries; node arrays are pool-allocated at world creation, never allocated during a step.

**Narrowphase + manifolds.** Three pair classes, each producing up to a 2-point manifold with feature ids (for warm-start matching):

| Pair | Algorithm | Reference |
|---|---|---|
| circle–circle | normal from centers, 1 point | Box2D b2CircleContact |
| circle–polygon | closest feature via SAT-lite, clamp to edge, 1–2 points | b2CirclePolygonContact |
| polygon–polygon (incl. box–box; box is a 4-gon) | SAT over face normals → reference/incident face → clipping to ≤ 2 points | Box2D b2CollidePolygons |

Mixed material: `mixFriction = sqrt(a.f * b.f)`, `mixRestitution = max(a, b)` (Box2D rules, hand-computable for the FR-PHY-020 tests).

**Solver — sequential impulse with warm starting (FR-PHY-020).** Per step:

1. **Pair update:** broadphase pairs → narrowphase manifolds; match against last tick's manifolds by feature id to carry accumulated impulses (warm start).
2. **Islands:** union-find over contact-connected + joint-connected dynamic bodies (v0.5 for sleep; contacts still traverse islands for solve grouping at MVP).
3. **Velocity solve:** for each contact point — relative normal velocity, restitution bias (captured pre-solve, applied only above the 1 m/s restitution threshold so resting stacks stay quiet), then Baumgarte OFF and position correction handled post-solve (Box2D 2.x default). Normal impulse `dn = -(vrel·n + bias) / kn`, accumulate + clamp ≥ 0; then tangent impulse for Coulomb friction clamped to `±μ·dn`. **8 velocity iterations.**
4. **Position solve:** split-impulse-style position correction (pseudo-velocities), **3 iterations**, linear slop 0.005 m, max correction 0.2 m/step, max correction velocity 3.0 m/s (all Box2D defaults — deliberately, they are 14 years of Catto's tuning, not my taste).
5. **Integrate velocities → positions** (semi-implicit Euler, the Box2D order).

Constants module (`solver_constants.hpp`), all `constexpr`, all overridable per-world for tests only. The 20-body stack test (FR-PHY-060, v0.5) is the acceptance for tuning; if it cannot settle jitter-free in its timebox, cut-ladder rung 6 fires (kinematic player, dynamic-vs-static only) rather than the timebox extending.

**Tunneling.** The character controller sweeps every tick (§3.6). Dynamic-vs-static tunneling at Artifact A speeds (≤ ~20 m/s ⇒ 0.33 m/tick at 60 Hz, well inside body sizes) is handled by the velocity clamp (max 200 m/s) plus the fat-AABB predictive margin; v0.5 adds optional substepping for fast projectiles. Bullet CCD is not planned (FR-PHY-100's spirit: no game in scope fires bullets fast enough to need it).

**Determinism mechanics.** Float only, `/fp:precise` for all physics TUs in every build (NFR-F-03; never `/fp:fast` in physics); no `/arch:AVX*` in the determinism config (avoids FMA contraction reassociation); iteration everywhere is index-ordered (entity-id on import, body-id on solve, sorted on query output); no hash-map iteration in any physics path (open sets are sorted vectors; the BVH pool is deterministic by construction); no time-dependent code (sleep timers count ticks, not seconds — v0.5). `hash_state` folds pose/velocity/flag arrays in body-id order into the FR-RUN-040 hash.

### 3.3 Body lifecycle across the ECS boundary

- **Create:** an entity gains `RigidBody2D` + ≥ 1 `Collider2D`. At the Pre-phase sync point, the `on_add` hooks (the sanctioned place to touch non-ECS subsystems, per ecs-gameplay §6) bulk-create bodies in entity-id order. A `Collider2D` without a body is legal (edit-mode scenes, sensor sketches) and simply has no world representation.
- **Change:** shape/material/filter edits detected via change ticks in IMPORT; shapes rebuild in place (BVH leaf refit, no body-id churn — preserving warm-start continuity where the shape allows).
- **Destroy:** `on_remove` hooks emit `TriggerExit`/`ContactEnd` for open pairs, then free the body (swap-remove + generation bump — stale `BodyId` fails lookup, the ECS generational-handle invariant mirrored inside physics).
- **Kinematic platforms:** moved by their owning system each tick via `teleport` + explicit `set_linear_velocity` (the platform velocity is what the controller reads for carry). Platform carry state comes from the controller's `groundBody` + `platformVelocity` output, not from solver friction tricks — friction-carry is the classic jitter source (Gaffer On Games' platformer notes; Box2D's own character guidance) and is excluded by construction.
- **Scene load / play mode:** the runtime world materializes a scene chunk, then bodies build in entity-id order during the same materialization pass — no post-load settle pass, no first-tick pop, and the play→stop→play identical-first-tick gate (NFR-M-03) holds because build order is part of the deterministic contract.

### 3.4 Poses, interpolation, and the transform contract (D2, in full)

| Actor | May write | May read |
|---|---|---|
| Editor (edit mode) | `LocalTransform`, `Collider2D`, `RigidBody2D` | — (no world exists) |
| Gameplay systems (FixedUpdate_Pre) | velocity/impulse via `World2D` calls; `CharacterController2D` params | `CharacterGround2D`, queries |
| Physics (FixedUpdate_Physics) | body-internal pose/velocity; `LocalTransform` of bodied entities; `CharacterGround2D` | everything |
| Render extract | nothing | `LocalTransform` + prev/cur contract with `alpha` |

A debug-mode ECS assert enforces the contract: `set<LocalTransform>` on an entity with a live dynamic/kinematic body is a hard assert (the one exception is the physics export itself). Interpolation is **not** physics' job: physics keeps its own prev/cur pose ring internally, and the engine-wide contract (owned by ecs-gameplay, consumed by rhi-renderer's extract step) interpolates `lerp(prev, cur, alpha)` for any entity whose transform changed — physics is just the largest producer of changed transforms. When FR-ENT-040 change detection lands (v0.5), interpolation automatically touches only moved bodies; physics does not need to know.

### 3.5 Queries (FR-PHY-040)

All queries share one pipeline: broadphase traversal (BVH stack walk, no allocation) → narrowphase refine → deterministic sort (`t` ascending, tie-break ascending body id) → optional filter callback. `QueryFilter` supports layer mask, exclusion span, and a predicate callback for game-specific rules (one-way drop-through, ghost-through-self). Shape casts sweep the shape's BVH-fattened AABB then refine with the pair-level algorithms — the character controller is just a shape-cast consumer with policy (§3.6). Everything is read-only, callable from any phase, and covered by the query-determinism test (fixed scene ⇒ byte-identical hit sets).

### 3.6 The 2D character controller (FR-PHY-030, MVP full design)

A kinematic body moved by explicit sweeps — never an impulse-solved dynamic body. This is the Box2D-community-standard shape for platformer protagonists and it is what the FR-PHY-030 acceptance (ride platforms without sliding, one-way from above only) actually measures. Per tick, `move_and_collide(e, delta)`:

1. **Horizontal sweep** — shape-cast along `delta.x` against the layer mask; on hit, stop at `hit − skinWidth`, set `vel.x = 0`, raise `WallLeft/WallRight` by normal direction.
2. **Vertical sweep** — same along `delta.y`; `Ground` on upward normal, `Ceiling` on downward, zeroing the corresponding velocity component.
3. **Slope policy** — if grounded and the ground normal's slope ≤ `maxSlopeAngleDeg`, project the horizontal delta onto the slope plane (walk up/down slopes without leaving ground); slope beyond the limit is treated as a wall (the classic "45° is a wall" platformer rule, authored per-character).
4. **Snap-down** — if the body was grounded last tick, moved, and is now airborne with ground within `snapDownDistance` below, snap onto it (kills stair-bounce and downslope flight).
5. **One-way platforms** — a `oneWay` collider participates only in downward sweeps (`normal.y > 0.5`) whose start pose has the body's feet above the collider top; drop-through is a temporary per-body filter exclusion (`DropThrough{uint8_t ticks}` component decremented in a gameplay system) — physics stays generic, the mechanic stays data.
6. **Moving-platform carry** — the controller adds `groundBody`'s `platformVelocity · dt` into the next tick's intended delta while `OnMovingPlatform` holds; carry therefore survives platform acceleration without friction tuning and cannot slide (the FR-PHY-030 acceptance test runs 600 ticks on an accelerating platform and asserts zero net slide).

Ground flags are instantaneous. Coyote time, jump buffering, and wall-slide are gameplay systems reading these flags — physics must never learn about them (D6).

### 3.7 3D: the Jolt embed (v0.5, FR-PHY-080; ADR required)

**Decision (D4): embed Jolt 5 rather than hand-write a 3D solver.** Rationale, argued honestly:

1. **The acceptance criterion is traversal quality, not solver knowledge.** FR-PHY-080 gates on "slopes, steps, no wall-clipping; camera never enters geometry" — capsule-vs-trimesh sliding with internal-edge rejection, step offset, and stick-to-floor. That is the exact problem class that took Catto years in 2D and that Bullet's `btKinematicCharacterController` is famous for getting subtly wrong. Jolt's `CharacterVirtual` ships this behavior set, documented (its Architecture + CharacterController docs are the best in the open-source field), and is proven in Horizon Forbidden West's engine and as Godot 4's optional Jolt module.
2. **The curriculum is already paid for.** LRN-6's learning gate is the Catto solver — bought once, in 2D, where it is tractable. A second from-scratch solver in 3D duplicates the lesson at ~3× the edge-case cost, against REQUIREMENTS N3's explicit judgment ("hand-rolling 3D rigid bodies is a year of edge cases Ashen Court doesn't need").
3. **FR-PHY-090 (knockable props, Could) is free only via Jolt.** A hand-written 3D character controller cannot grow dynamic props without becoming a full rigid-body engine.
4. **License and maintenance fit.** MIT, single principal maintainer (Jorrit Rouwe), vcpkg-packaged, no transitive runtime dependencies — consistent with the closed dependency policy as the pre-approved exception.

Honest costs, accepted: an embed boundary to keep clean (§3.7.1), a pinned version to defend against churn (§7, R4), and a due-diligence pass on Jolt's build flags against the no-exceptions/no-RTTI runtime binding — Jolt uses no RTTI and its CMake surface exposes assert/profiler/double-precision switches, with exception use guarded behind its own macros; the ADR must verify the exact switches against our `/EHs-c-` + `_HAS_EXCEPTIONS=0` configuration **before the first include**, with the recorded fallback (plan B) being a hand-written kinematic-only 3D capsule controller over `World3D`'s shape-cast API — the same algorithm as §3.6 lifted to 3D — which satisfies FR-PHY-080's letter if Jolt's flags fight the toolchain.

**The boundary (D7):**

- `mce_physics3d` is a CMake target; `grep -r "JPH"` outside it fails CI (mirrors the GL-leak check in rhi-renderer).
- The facade converts at the door: `Vec3 ↔ JPH::Vec3`, `Quat ↔ JPH::Quat`, `AABB3 ↔ JPH::AABox`. No JPH type appears in any component, event, or header above the target.
- Components mirror the 2D names: `RigidBody3D`, `Collider3D` (sphere/box/capsule/convex/mesh), `CharacterController3D`, `CharacterGround3D`.
- Jolt's object layers map 1:1 onto MCE's layer table (one authored table serves both dimensions); the `BroadPhaseLayer` mapping is a fixed collapse inside `mce_physics3d`.
- Jolt steps single-threaded (`JobSystemSingleThreaded`) on MCE's 60 Hz tick; its `TempAllocator` gets a fixed 4 MiB carve-out at world creation (its lifetime differs from the frame allocator — per-tick scratch inside Jolt stays in Jolt's own allocator; no cross-ownership, no heap churn after warmup).
- Contact listeners bridge into `TriggerEnter3D`/`ContactBegin3D` event types shaped identically to the 2D ones.
- **Cook-side:** mesh colliders are cooked, not runtime-built. The cook tool (assets) links Jolt to build `ConvexHullShape`/`MeshShape` payloads with ActiveEdges computed offline and stored in `.mce` chunks; the runtime deserializes cooked shapes directly. This is why Jolt appears in the cook-tool dependency set too — cook-time-only, never shipped in the runtime beyond the already-embedded `mce_physics3d`.
- **Determinism:** Jolt documents same-platform, same-binary determinism with single-threaded stepping; the FR-RUN-040 hash extends to 3D poses from v0.5 with the thread count pinned to 1 in the determinism config. Cross-machine Jolt determinism is not claimed (same narrow definition as FR-RUN-040).

### 3.8 Memory layout and budgets

| Structure | Allocator | Budget (default) | Notes |
|---|---|---|---|
| Body SoA + BVH node pool + pair sets | world-owned pool allocator, sized at world creation | 8 MiB default, per-game configurable | Grows only at scene load / hook time; never inside `step()` |
| Contact/island/manifold scratch | frame-linear allocator | within the 16 MiB frame slot (NFR-F-03) | Reset each tick with the frame; zero heap in the hot path (binding) |
| Jolt temp allocator | Jolt-owned, 4 MiB carve-out | v0.5 | Never shared with MCE's frame allocator |
| Cook-time hull/mesh building | cook arena (assets) | cook-tool budget | Cook-time only |

Per-milestone memory budget tables (NFR-M-04) get a physics line from M1: bodies × ~200 B + contacts × ~64 B + BVH nodes × 48 B, reconciled against Tracy's memory track.

---

## 4. Threading & lifecycle

### 4.1 Threading model

**Physics is single-threaded through v1.0.** It owns its arrays exclusively during the `FixedUpdate_Physics` slot; no mutex exists anywhere in `mce_physics2d` because no other thread touches a live world. This is a determinism-first commitment (P4, FR-RUN-040), not an oversight: parallel island solving (Box2D 2.4's model) changes float accumulation order per island composition, which would put the solver outside the narrow FR-RUN-040 determinism definition or force fixed-point/quanta discipline across the whole solver. If the 2,000-body budget (FR-PHY-050) is ever threatened, the sanctioned order of relief is: SIMD broadphase hotspots (FR-PHY-070, scalar reference kept as oracle) → substep tuning → only then, a re-opened discussion of parallel islands with a determinism ADR. Cooking-side physics work (convex hulls, ActiveEdges) parallelizes freely on the job system — it is offline and not in the determinism contract.

### 4.2 World lifecycle

| State | Worlds alive | Bodies |
|---|---|---|
| Editor, edit mode | none | none — collider overlays draw from component data directly (editor-tools); gizmo edits hit components, not a world |
| Editor, play mode (v0.5) | one preview world per play session | built from the scene document at play entry (≤ 1 s budget, NFR-M-03), destroyed wholesale at stop — Play→Stop→Play yields identical first-tick state because build order is entity-id and world construction is stateless |
| Runtime (shipped game / run-in-player) | one world per runtime world | built at scene-chunk materialization; app shell scene transitions (FR-RUN-030) may keep or rebuild worlds per scene-state policy; scene-to-scene < 1 s (NFR-M-01) |
| Headless (FR-RUN-025) | `World2D` only, no renderer | full physics ticks with zero GL calls — the determinism harness runs here |

### 4.3 Frame-to-frame safety rules

- All cross-frame references gameplay holds are `Entity` handles (binding); `BodyId` never escapes `mce::physics`.
- Body mutation calls are valid only from `FixedUpdate` systems; calls from render-phase code are debug-build asserts (a physics API call outside the sim tick would silently break determinism).
- The `World2D&` resource is registered as a world resource (ecs-gameplay's closed singleton set: `Time`, `Input`, `ActiveCamera`, physics world, mixer), not as a component on a lone entity.
- Crash safety: no physics state is ever serialized to save files; a crash mid-tick costs at most the current frame — minidump path per FR-DBG-030 covers it.

---

## 5. Dependencies

### 5.1 Upstream (physics consumes)

| Subsystem | What physics takes | Contract note |
|---|---|---|
| **core** | math (`Vec2/Vec3/Quat/AABB2/Plane`), allocators (world pool + frame-linear), generational `Handle<T>` machinery, `Result<T,Error>`, Tracy zone macros, convars, `SmallVector` | Physics defines no math types of its own; the DirectXMath-oracle-tested core math is the single source. Physics-local shape structs (`Box2/Circle2/Convex2`) are thin wrappers over these, defined in `components.hpp`. |
| **ecs-gameplay** | component registration + reflection traits (`MCE_COMPONENT`), phase pipeline + system registration, command buffers + `on_add/on_remove/on_set` hooks, `EventQueue<T>` + `EventRegistry`, `Entity` handles, world resources, `Frame&` | Physics is a *model* of the gameplay programming model: it registers components and exactly one system per world. It holds no ECS pointers across ticks beyond the resource handle. |
| **assets** | cooked `.mce` chunks for mesh-collider payloads (3D convex hulls / meshes with ActiveEdges); the layer/filter table as authored data | Runtime physics reads only cooked payloads (binding asset policy); malformed payloads surface as diagnosable `Result` errors, never crashes — same negative-test bar as the `.mce` reader. |
| **rhi-renderer** | *indirect only*: a `DebugDrawSink` interface physics fills (line segments, wire shapes, AABBs, contact points) | Physics never includes renderer headers; editor-tools wires the sink to the renderer's debug line batch. The dependency direction is: editor-tools → {physics, rhi-renderer}, never physics → rhi-renderer. |

### 5.2 Downstream (consumes physics)

| Consumer | Uses |
|---|---|
| **ecs-gameplay** (gameplay systems) | `World2D`/`World3D` facades, controller API + `CharacterGround2D`, queries, trigger/contact events, components |
| **editor-tools** | `debug_draw` overlays (FR-EDT-090 colliders/bounds), inspector round-trip of collider components via reflection, play-mode world teardown/rebuild, live `Stats` for the debug panels |
| **audio-input-ui** | nothing directly — gameplay systems translate controller flags/events into audio events (footsteps, landings); physics emits engine events, never audio calls |
| **scripting** (v0.5+, data-driven sequences) | physics via the same facades/events only — a sequence step ("wait until grounded") reads `CharacterGround2D` through the ECS, never through a physics backdoor |

### 5.3 The dependency direction, stated as a rule

`core` ← `ecs-gameplay` ← `physics` ← `editor-tools`. Physics may not include anything above it; everything above it reaches physics through `mce/physics/*.hpp` facade headers. The CMake target graph plus the include-direction CI check (existing-engines L1) enforces this; the `JPH`-leak grep is the physics-specific instance of the same check.

---

## 6. Test strategy

Unit tests use doctest (pinned, per the committed test stack); physics tests live in a `test_physics2d` executable linked against the headless runtime — and this test executable, and only it, links Box2D v3 as the behavior oracle (D9, ADR-0016: test-time-only dependency, same class as glslangValidator/DirectXMath; never present in any runtime, editor, cook, or shipped target). The binding test policy (math oracle, round-trips, ASan per PR) applies; the physics-specific suite:

| # | Test | FR | Gate |
|---|---|---|---|
| 1 | **20 hand-computed contact-manifold cases** (box–box edge/vertex/face, box–circle, circle–circle, polygon–polygon; expected normal, points, penetration) | FR-PHY-010 | table-driven doctest; every case hand-solved on paper first (LRN-6 evidence) |
| 2 | **Solver behavior cases**: drop-and-restitution rebound heights, sliding-box stop distances, mixed-friction pairs — all against hand-computed sequential-impulse solutions with tolerance | FR-PHY-020 | per-case tolerance; no tuning constants hidden in tests |
| 3 | **Controller suite**: ride a moving platform 600 ticks (zero net slide), one-way blocked-from-above-only, drop-through exits cleanly, slope walk + slope-limit wall conversion, snap-down on stair descent, ceiling bump zeroes vy | FR-PHY-030 | unit tests with a synthetic fixed clock |
| 4 | **Trigger exactly-once**: enter → exit, enter → destroy-other, re-enter after exit | FR-PHY-035 | unit test; no duplicate or missed events across 10k randomized (seeded) motions |
| 5 | **Query determinism**: fixed scene, 10k seeded queries, byte-identical hit sets across runs and Debug-vs-Release | FR-PHY-040 | part of the FR-RUN-040 harness |
| 6 | **Broadphase budget**: 2,000 dynamic bodies (mixed sleep) broadphase+integrate < 3 ms, Tracy zone, dev machine | FR-PHY-050 | budget-line benchmark (dev machine, not a CI gate — machine variance) |
| 7 | **Joint suite** (v0.5): distance/revolute against hand-computed constraint solutions; 20-body stack settles < 1 s, no jitter | FR-PHY-060 | video + Tracy evidence attached to the milestone doc |
| 8 | **Sim determinism hash**: full-tick runs, N=10k ticks, hash includes `LocalTransform` + `CharacterGround2D` of all bodied entities; 3 consecutive runs + Debug-vs-Release equal | FR-RUN-040 / NFR-F-04 | CI, every PR from M1 |
| 9 | **Headless purity**: 10k physics-only ticks with zero GL calls | FR-RUN-025 | CI grep + ASan build |
| 10 | **Boundary hygiene**: `JPH` symbol grep outside `mce_physics3d` (from v0.5); `Box2D`/`b2` symbol grep — allowed in `test_physics2d` only, failed for every runtime/editor/cook target (D9) | binding RHI-seam discipline, D7, D9 | CI, every PR |
| 11 | **Negative/robustness**: teleport during active sweep, body destroyed via command buffer mid-pair, stale `BodyId`/`Entity` lookups, malformed cooked collider payload → `Result` error, never a crash | FR-DX-050 | doctest + ASan |
| 12 | **Render-verification**: collider debug overlays + controller motion verified in editor screenshots and a RenderDoc capture of the overlay pass | FR-EDT-090, FR-DBG-020 | milestone checklist (human-eyeballed capture gate) |
| 13 | **Jolt same-binary determinism** (v0.5): 3D controller scene, 10k ticks, pose hashes stable single-threaded | FR-RUN-040 extension | CI from the milestone that embeds Jolt |
| 14 | **Box2D-oracle scene diffs**: deterministic scenes (drop, slide, restitution, pyramid-lite ≤ 4 bodies, platform ride) stepped in `mce_physics2d` and Box2D v3 side-by-side; final pose + velocity compared within documented tolerances (the oracle validates the in-house solver; it does not replace the hand-computed cases of tests 1–2) | D9, FR-PHY-020 | `test_physics2d` only; runs locally and in the nightly CI lane (test-time dep adds link time, kept off the per-PR critical path) |

Golden-image tests do not apply to physics directly; the collider-overlay golden pair (editor view with/without debug draw) is the one visual fixture, run at cook per FR-DBG-020 policy.

---

## 7. Risks & mitigations

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | **Solver-tuning rabbit hole** — jitter, restitution instability, stack collapse eating weeks (the classic Box2D-clone stall). | High | High | Box2D's literal default constants from day one (not my taste — Catto's 14 years). Stacking out of MVP scope (REQUIREMENTS FR-PHY-060 note). Cut-ladder rung 6 is pre-agreed: > 3 weeks of solver instability ⇒ dynamic-vs-static only + kinematic player. The cut ladder, not heroics. |
| R2 | **Transform-authority ambiguity** — gameplay writes `LocalTransform` on a dynamic body and fights physics, or interpolation double-owns poses. | Medium | High (subtle, whole-game visual bugs) | The §3.4 contract table; debug assert on illegal writes; teleport only via API. Interpolation explicitly owned by the engine-wide prev/cur contract, not physics. |
| R3 | **Jolt build-flag friction** (exceptions/RTTI/float-flags vs the no-exception runtime binding). | Medium | High at the worst moment (M2/M3, mid-3D push) | ADR due-diligence item *before* the first include; verify flags on a spike branch. Recorded plan B: hand-written kinematic-only 3D controller over the shape-cast API (§3.6 lifted to 3D) — shippable per FR-PHY-080's letter. |
| R4 | **Jolt version churn** breaking the embed (the wgpu breaking-cadence lesson from the stack decision). | Medium | Medium | Pin exact version in `vcpkg.json`; upgrades only via ADR; the facade is ~15 functions, so an API break is a bounded `mce_physics3d` patch by construction. |
| R5 | **Event floods** — particle storms or shell casings against sensors flooding trigger queues, stalling gameplay systems. | Medium | Medium | Pair-diff emission (not per-tick), per-tick per-sensor cap + convar, documented per-event budget in the dev overlay's event-flow monitor (FR-ENT-030's editor view shows it live). |
| R6 | **Determinism drift** — FMA contraction or reassociation under `/fp:fast` release or `/arch:AVX2` breaking the hash test. | Medium | High (CI-blocking, mysterious) | Physics TUs compiled `/fp:precise` in every configuration; no `/arch:AVX*` in the determinism config; hash test runs Debug AND Release every PR (test 8). |
| R7 | **Moving-platform carry jitter/sliding** (the classic failure the FR-PHY-030 gate measures). | Medium | Medium | Carry by explicit `platformVelocity` accumulation in the controller, never solver friction; kinematic platforms moved via teleport + velocity; 600-tick zero-slide unit test. |
| R8 | **Collider/component desync in play mode** — inspector edits not reflected in bodies. | Low | Medium | All edits flow: reflection → commands → sync point → hooks → world update, same tick. Edit mode has no world to desync (§4.2). |
| R9 | **Query API misuse** — gameplay allocating per raycast, violating zero-hot-path-heap (NFR-F-03). | Medium | Medium | API takes out-params/`SmallVector` only; no query returns a container; ASan + debug allocator asserts catch violations. |
| R10 | **2D/3D API drift** — two worlds diverging in naming/semantics until UC-B's controls feel foreign. | Medium | Low | Mirror signatures (§2.2) + the compile-time mirror test; one shared layer/filter model; one `QueryFilter` type. |
| R11 | **Compound-collider scope creep** (fixture lists, per-shape materials, breakable compounds) arriving before v0.5. | Medium | Medium | MVP = one collider per body, stated in §2.1; compound = child entities (v0.5 sketch); anything richer needs a slip note per the milestone-gate binding. |
| R12 | **Convex hull cook cost/quality** for 3D mesh colliders (degenerate hulls, hull explosion on high-poly meshes). | Medium | Medium | Hulls computed at cook in the cook tool (assets owns the pipeline), vertex-weld + simplify first, hull vertex caps, cook-time validation errors with the asset path (FR-AST-040's diagnosable-error bar); runtime never builds shapes. |

---

## 8. Milestone mapping

Tier names with ROADMAP milestone IDs (ARCHITECTURE §10.1 is the mapping authority).

| Milestone | Physics deliverable | FRs green |
|---|---|---|
| **MVP — ROADMAP M3 (Artifact A)** | `mce_physics2d` rung-6 shape: components, `World2D`, dynamic-vs-static (+kinematic) contact response with friction/restitution, swept character controller with one-way + platform carry (velocity accumulation), sensors, queries, dynamic AABB tree broadphase, events, debug draw, `/fp:precise` determinism integration, tests 1–5, 8, 9 | FR-PHY-010, -020, -030, -035, -040 |
| **MVP stretch — ROADMAP M3.5 (slips whole to v0.5 if M3/M4 overrun; carries no Must FRs)** | Solver hardening: warm-started sequential impulse, island traversal, restitution tuning, 20 hand-computed manifold cases; basic 2–3-box stacking stability arrives with warm starting + islands (robust stacking waits for joints at v0.5) | FR-PHY-010, -020 (hardening acceptance) |
| **v0.5 — ROADMAP M5b (Jolt) + M6 (rest)** | Sleeping + islands, joints (distance + revolute, spring optional), 2,000-body budget validation, compound colliders via hierarchy, optional substeps (M6); **Jolt ADR + `mce_physics3d` (M5b)**: `CharacterVirtual` controller, camera sweeps, cooked mesh colliders, 3D events/queries, tests 6, 7, 10, 13 | FR-PHY-050, -060, -070, -080 |
| **v1.0 (only if demanded)** | Dynamic 3D props through the same Jolt embed | FR-PHY-090 (Could — UC-B ships without it regardless) |

The Jolt ADR (`docs/adr/0003-embed-jolt-for-3d.md`) must exist before the first `JPH` include: decision, build-flag verification results, version pin, thread-count pin, escape hatch (plan B), the boundary rules of §3.7 restated as enforceable checks, and the **adoption triggers** written as conditions (expected to fire at the M5b timebox): the embed is justified only when a 3D prototype needs (a) static concave level geometry (trimesh), (b) a kinematic character controller against meshes, or (c) compound shapes — until one fires, `JPH` is not included anywhere. The Box2D-oracle ADR (`docs/adr/0016-box2d-test-oracle.md`, test-time dependency class) must likewise exist before the first test-only include.

---

## 9. References

- Erin Catto, Box2D GDC slide series (sequential impulse, warm starting, position correction, dynamic AABB tree, character bodies), the Box2D 2.x source (design skeleton), and the Box2D v3 (2024) C rewrite — solver design, default constants, and the test-time behavior oracle per D9 (linked into `test_physics2d` only, ADR-0016).
- Glenn Fiedler, "Fix Your Timestep," Gaffer On Games — fixed-step integration contract; platformer platform-carry discussions.
- Jason Gregory, *Game Engine Architecture*, 3rd ed. — physics subsystem scope and middleware-embed boundaries (ch. 12); the embed-vs-from-scratch framing D1 follows.
- Jolt Physics documentation and `HelloJolt`/`CharacterVirtual` samples; Jorrit Rouwe's architecture docs — the embedded 3D reference and the CharacterVirtual controller mapping.
- Godot 4's optional Jolt module and Piccolo's Jolt integration — two prior solo/open embeds of exactly the shape D4 proposes; both kept the physics seam thin.
- Bob Nystrom, *Game Programming Patterns* — Update Method and Event Queue patterns underlying the phase/event contract.
- REQUIRES docs: `docs/REQUIREMENTS.md` §3.6, §6.3 (cut ladder rung 6), Appendix A/A10; `docs/research/ecs-gameplay.md` §4–§6 (phases, command buffers, hooks, events); `docs/research/existing-engines.md` L6; `docs/research/platform-engineering.md` §8 (determinism build, test policy).
