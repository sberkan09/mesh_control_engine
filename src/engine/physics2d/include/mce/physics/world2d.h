#pragma once
// World2D — the hand-written 2D physics world (physics.md §2.2, §3.2; ROADMAP M3 rung-6 shape).
//
//  - static / kinematic / dynamic bodies; box, circle and convex colliders (one per body)
//  - dynamic AABB tree broadphase; SAT + clipping manifolds; GJK distance for casts
//  - sequential-impulse solver (8 velocity / 3 position iterations, Box2D 2.x constants), warm
//    started from last step's impulses matched by feature id, solved island by island (M3.5)
//  - swept kinematic character controller (move_and_collide), sensors, queries, events
//
// Determinism (D8): single-threaded, float only, compiled with precise FP everywhere, every loop
// runs in body-slot order and every query result is sorted. hash_state() folds all body state.
//
// The world knows nothing about the ECS beyond the Entity handle; mce/physics/module.h bridges it
// into the FixedUpdate_Physics phase. Body ids never leave this class: callers name bodies by
// the Entity that owns them.

#include <optional>

#include "mce/containers/array.h"
#include "mce/memory/allocator.h"
#include "mce/physics/components.h"

namespace mce::physics {

struct PhysicsConfig {
    vec2 gravity{0.0f, -9.81f};
    f32 fixed_dt = 1.0f / 60.0f;  // used by the controller's platform carry
    u32 velocity_iterations = 8;
    u32 position_iterations = 3;
    bool warm_starting = true;          // carry accumulated impulses across steps (M3.5)
    f32 restitution_threshold = 1.0f;   // m/s: slower approaches never bounce (resting stays quiet)
    u32 max_contact_events_per_tick = 256;  // ContactBegin2D/End2D cap (risk R5)
};

struct QueryFilter {
    u16 layer_mask = 0xFFFF;
    Span<const Entity> exclude;  // e.g. self
    bool include_sensors = false;
    // Optional per-hit veto: return false to skip a candidate (e.g. drop-through).
    bool (*predicate)(Entity hit, void* user) = nullptr;
    void* user = nullptr;
};

struct RayHit2D {
    Entity entity;
    vec2 point;
    vec2 normal;
    f32 t = 0.0f;  // distance along the ray (raycast) or fraction of the sweep (shape_cast)
};

struct MoveResult2D {
    vec2 position;   // resolved body origin this tick
    u32 flags = 0;   // ControllerFlags
    vec2 ground_normal;
    Entity ground_body;  // the ground/platform underfoot this tick, else null
    vec2 platform_velocity;
    bool hit_ceiling = false;
};

// Debug draw output (physics.md §5.1): physics fills it, editor-tools wires it to the renderer.
class DebugDrawSink {
public:
    virtual ~DebugDrawSink() = default;
    virtual void line(vec2 a, vec2 b, math::Color c) = 0;
    virtual void circle(vec2 center, f32 radius, math::Color c) = 0;
};
struct DebugDrawFlags {
    static constexpr u32 Shapes = 1u << 0;
    static constexpr u32 Aabbs = 1u << 1;
    static constexpr u32 Contacts = 1u << 2;
    static constexpr u32 Broadphase = 1u << 3;
};

struct BodyDesc {
    RigidBody2D body;
    Collider2D collider;
    vec2 position;
    f32 angle = 0.0f;
    const CharacterController2D* controller = nullptr;  // forces a kinematic body
};

// Read-only snapshot of one body, for the ECS export and tools.
struct BodyState {
    Entity entity;
    BodyType type = BodyType::Static;
    vec2 position;  // body origin
    f32 angle = 0.0f;
    vec2 linear_velocity;
    f32 angular_velocity = 0.0f;
    bool enabled = true;
    bool controller = false;
};

class World2D {
public:
    explicit World2D(const PhysicsConfig& config = {}, mem::IAllocator& alloc = mem::heap());
    ~World2D();
    World2D(World2D&&) noexcept;
    World2D& operator=(World2D&&) noexcept;
    World2D(const World2D&) = delete;
    World2D& operator=(const World2D&) = delete;

    const PhysicsConfig& config() const;

    // ---- body lifecycle (the ECS bridge drives these from components) -------------------------
    // False when the entity already has a body or the shape is malformed (degenerate box,
    // < 3 hull points...): the entity simply has no world representation.
    bool create_body(Entity e, const BodyDesc& desc);
    // Synthesizes TriggerExit / ContactEnd2D for the body's open pairs (delivered by the next
    // step), then frees the slot. Stale handles are a no-op.
    void destroy_body(Entity e);
    bool has_body(Entity e) const;
    // In-place updates (no slot churn). Return false if the entity has no body or the new shape
    // is malformed (the old shape is kept).
    bool set_body_config(Entity e, const RigidBody2D& rb);
    bool set_collider(Entity e, const Collider2D& c);
    bool set_controller(Entity e, const CharacterController2D& c);

    // ---- stepping (FixedUpdate_Physics only) ------------------------------------------------------
    void step(f32 dt);

    // ---- body mutation (FixedUpdate systems) --------------------------------------------------------
    void set_linear_velocity(Entity e, vec2 v);  // kinematic + dynamic
    void set_angular_velocity(Entity e, f32 w);
    void apply_impulse(Entity e, vec2 impulse, vec2 world_point);
    void apply_force(Entity e, vec2 force);  // accumulated until the next step
    void teleport(Entity e, vec2 position, f32 angle);
    void set_enabled(Entity e, bool active);  // keeps the slot; disabled bodies neither move nor collide

    bool body_state(Entity e, BodyState& out) const;
    vec2 linear_velocity(Entity e) const;

    // ---- character controller (FR-PHY-030). `delta` = intended displacement this tick. -------
    // `filter` narrows what blocks the sweep (layer mask, exclude list, predicate) — e.g. a
    // drop-through excludes the one-way platform underfoot for a few ticks.
    MoveResult2D move_and_collide(Entity e, vec2 delta, const QueryFilter& filter = {});
    // Last move result (what the ECS export writes to CharacterGround2D).
    bool controller_state(Entity e, MoveResult2D& out) const;

    // ---- queries (FR-PHY-040): read-only, any phase, sorted, allocation-free after warm-up ------
    std::optional<RayHit2D> raycast(vec2 origin, vec2 dir, f32 max_dist, const QueryFilter& f = {}) const;
    // All hits along the ray, sorted by t then body slot. `out` is cleared first.
    void raycast_all(vec2 origin, vec2 dir, f32 max_dist, Array<RayHit2D>& out, const QueryFilter& f = {}) const;
    // Sweeps `shape` (at rotation 0) from `from` to `to`; t is the swept fraction.
    std::optional<RayHit2D> shape_cast(const ColliderShape& shape, vec2 from, vec2 to, const QueryFilter& f = {}) const;
    bool overlap_point(vec2 p, const QueryFilter& f = {}) const;
    // Entities whose fat broadphase box and shape AABB overlap `box`, in body-slot order.
    u32 overlap_aabb(math::aabb2 box, Array<Entity>& out, const QueryFilter& f = {}) const;

    // ---- events produced by the last step (the bridge copies them into EventQueue<T>) ----------
    Span<const TriggerEnter> trigger_enters() const;
    Span<const TriggerExit> trigger_exits() const;
    Span<const ContactBegin2D> contact_begins() const;
    Span<const ContactEnd2D> contact_ends() const;

    // ---- editor / test surface ------------------------------------------------------------------
    void debug_draw(DebugDrawSink& sink, u32 flags) const;
    u64 hash_state(u64 seed) const;
    // Calls fn(const BodyState&) for every live body in slot order.
    template <class Fn>
    void each_body(Fn&& fn) const {
        BodyState s;
        for (u32 i = 0, n = slot_count(); i < n; ++i)
            if (body_at(i, s)) fn(s);
    }

    struct Stats {
        u32 bodies = 0;
        u32 contacts = 0;      // touching contact constraints last step
        u32 islands = 0;       // contact-connected groups of enabled dynamic bodies last step
        u32 sensor_pairs = 0;  // open sensor overlaps
        u32 proxies = 0;
        i32 tree_height = 0;
    };
    Stats stats() const;
    bool validate_broadphase() const;

    struct Impl;

private:
    u32 slot_count() const;
    bool body_at(u32 slot, BodyState& out) const;
    mem::UniquePtr<Impl> impl_;
};

}  // namespace mce::physics
