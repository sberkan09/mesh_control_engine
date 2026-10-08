#pragma once
// World3D — the 3D physics world, a facade over embedded Jolt Physics 5 (ADR-0003, physics.md §3.7;
// ROADMAP M5b).
//
//  - static / kinematic / dynamic bodies; box, sphere, capsule, convex-hull and triangle-mesh shapes
//  - CharacterVirtual-driven capsule controller (move_character) and the camera-boom sphere sweep
//    (camera_sweep) — the two 3D additions to World2D's method set (physics.md §2.2)
//  - raycasts and sphere casts, sorted and filtered like World2D's
//
// Nothing in this header names a Jolt type: callers see MCE math types and the Entity that owns
// each body. Jolt is reachable only from src/engine/physics3d/ (the JPH wall in check_walls.py).
//
// Determinism (ADR-0003 §8): single-threaded stepping at a fixed 1/60 s, Jolt built with
// CROSS_PLATFORM_DETERMINISTIC, every loop in creation order. hash_state() folds all body and
// character state.

#include <optional>

#include "mce/containers/array.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"

namespace mce::physics {

using math::quat;
using math::vec3;

enum class BodyType3D : u8 { Static, Kinematic, Dynamic };
enum class ShapeType3D : u8 { Box, Sphere, Capsule, Convex, Mesh };

// One shape per body, in the body's local frame (metres). Capsules and characters stand on +Y.
// Convex and Mesh point at caller memory that only has to outlive create_body(): the shape is
// built there (ADR-0003 §9 — cooked colliders replace this path later without changing it).
struct Shape3D {
    ShapeType3D type = ShapeType3D::Box;
    vec3 half{0.5f, 0.5f, 0.5f};  // Box
    f32 radius = 0.5f;             // Sphere, Capsule
    f32 half_height = 0.5f;        // Capsule: half the length of the cylinder part
    Span<const vec3> points;       // Convex: hull input; Mesh: vertices
    Span<const u32> indices;       // Mesh: 3 per triangle, counter-clockwise seen from the front

    static Shape3D box(vec3 half_extents) {
        Shape3D s;
        s.type = ShapeType3D::Box;
        s.half = half_extents;
        return s;
    }
    static Shape3D sphere(f32 r) {
        Shape3D s;
        s.type = ShapeType3D::Sphere;
        s.radius = r;
        return s;
    }
    static Shape3D capsule(f32 half_cylinder_height, f32 r) {
        Shape3D s;
        s.type = ShapeType3D::Capsule;
        s.half_height = half_cylinder_height;
        s.radius = r;
        return s;
    }
    static Shape3D convex(Span<const vec3> hull_points) {
        Shape3D s;
        s.type = ShapeType3D::Convex;
        s.points = hull_points;
        return s;
    }
    static Shape3D mesh(Span<const vec3> vertices, Span<const u32> triangle_indices) {
        Shape3D s;
        s.type = ShapeType3D::Mesh;
        s.points = vertices;
        s.indices = triangle_indices;
        return s;
    }
};

struct BodyDesc3D {
    BodyType3D type = BodyType3D::Static;
    Shape3D shape;
    vec3 position;
    quat rotation;
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    f32 density = 1000.0f;  // kg/m^3, dynamic bodies only
    f32 gravity_scale = 1.0f;
    f32 linear_damping = 0.05f;
    u8 layer = 0;       // index into the game's collision-layer table (0..15), as in 2D
    u16 mask = 0xFFFF;  // layers this body collides with
};

// The capsule controller's shape and walking rules. Passed to create_character() and again on
// every move_character() (the shape is fixed at creation; the rules may change per call).
struct ControllerParams3D {
    f32 radius = 0.3f;
    f32 half_height = 0.6f;          // cylinder half height: total height = 2 * (half_height + radius)
    f32 max_slope_deg = 50.0f;       // steeper ground is a wall
    f32 step_height = 0.35f;         // stairs this tall are climbed while walking; 0 = off
    f32 stick_to_floor = 0.5f;       // snap down this far when walking off a ledge/down a slope; 0 = off
    f32 padding = 0.02f;             // skin kept between the capsule and geometry
    f32 mass = 70.0f;                // kg, for pushing dynamic bodies
    f32 max_push_force = 100.0f;     // N
    u8 layer = 0;
    u16 mask = 0xFFFF;
};

struct ControllerFlags3D {
    static constexpr u32 Ground = 1u << 0;       // standing on walkable ground
    static constexpr u32 SteepGround = 1u << 1;  // touching ground steeper than max_slope_deg
    static constexpr u32 Wall = 1u << 2;         // a near-vertical contact this tick
    static constexpr u32 Ceiling = 1u << 3;      // a downward-facing contact above the capsule
    static constexpr u32 OnMovingBody = 1u << 4; // the ground underfoot moves
};

struct MoveResult3D {
    vec3 position;          // resolved character position (capsule centre) this tick
    vec3 velocity;          // the velocity the controller ended with (after sliding)
    u32 flags = 0;          // ControllerFlags3D
    vec3 ground_normal;     // zero when airborne
    Entity ground_body;     // the body underfoot this tick, else null
    vec3 ground_velocity;   // velocity of the ground point (platform carry)
    bool hit_ceiling = false;
};

struct QueryFilter3D {
    u16 layer_mask = 0xFFFF;
    Span<const Entity> exclude;  // e.g. self
    bool include_dynamic = true;
};

struct RayHit3D {
    Entity entity;
    vec3 point;
    vec3 normal;
    f32 t = 0.0f;  // distance along the ray
};

struct ShapeHit3D {
    Entity entity;
    vec3 point;     // contact point on the hit surface
    vec3 normal;    // surface normal at the contact, pointing back at the swept shape
    vec3 position;  // swept shape centre at the time of impact (where to put the camera)
    f32 t = 0.0f;   // fraction of the sweep [0, 1]
};

struct PhysicsConfig3D {
    vec3 gravity{0.0f, -9.81f, 0.0f};
    f32 fixed_dt = 1.0f / 60.0f;
    u32 max_bodies = 4096;
    u32 max_body_pairs = 8192;
    u32 max_contact_constraints = 4096;
    usize temp_allocator_bytes = 4u << 20;  // ADR-0003 §5: fixed 4 MiB carve-out
};

// Read-only snapshot of one body.
struct BodyState3D {
    Entity entity;
    BodyType3D type = BodyType3D::Static;
    vec3 position;
    quat rotation;
    vec3 linear_velocity;
    vec3 angular_velocity;
};

class World3D {
public:
    explicit World3D(const PhysicsConfig3D& config = {});
    ~World3D();
    World3D(World3D&&) noexcept;
    World3D& operator=(World3D&&) noexcept;
    World3D(const World3D&) = delete;
    World3D& operator=(const World3D&) = delete;

    const PhysicsConfig3D& config() const;

    // ---- body lifecycle ----------------------------------------------------------------------------
    // False when the entity already has a body/character or the shape is malformed (non-positive
    // extents, < 4 hull points, an empty or out-of-range mesh): the entity then has no body.
    bool create_body(Entity e, const BodyDesc3D& desc);
    // A capsule character standing with its centre at `position`. Same false cases as create_body.
    bool create_character(Entity e, vec3 position, const ControllerParams3D& params = {});
    // Frees a body or a character. Stale handles are a no-op.
    void destroy_body(Entity e);
    bool has_body(Entity e) const;

    // ---- stepping ------------------------------------------------------------------------------------
    // dt must equal config().fixed_dt (asserted, physics.md D7).
    void step(f32 dt);

    // ---- body mutation -----------------------------------------------------------------------------
    void set_linear_velocity(Entity e, vec3 v);  // kinematic + dynamic bodies
    void apply_impulse(Entity e, vec3 impulse);  // dynamic bodies, at the centre of mass
    void teleport(Entity e, vec3 position, quat rotation);  // bodies and characters

    bool body_state(Entity e, BodyState3D& out) const;

    // ---- character controller (FR-PHY-080). `delta` = intended displacement this tick, gravity
    // and jumps included (gameplay owns them, as in 2D). Unknown entities return a zero result.
    MoveResult3D move_character(Entity e, vec3 delta, const ControllerParams3D& params = {});
    // Last move result.
    bool controller_state(Entity e, MoveResult3D& out) const;

    // ---- camera collision (UC-B gate): sweep a sphere along the boom from the pivot `from` to the
    // desired camera position `to` against static + kinematic geometry. On a hit, put the camera at
    // ShapeHit3D::position. Characters and dynamic props never pull the camera in.
    std::optional<ShapeHit3D> camera_sweep(vec3 from, vec3 to, f32 sphere_radius) const;

    // ---- queries: read-only, closest hit, ties broken by creation order -------------------------
    // `dir` need not be normalized; t is in metres along it.
    std::optional<RayHit3D> raycast(vec3 origin, vec3 dir, f32 max_dist, const QueryFilter3D& f = {}) const;
    std::optional<ShapeHit3D> shape_cast(f32 sphere_radius, vec3 from, vec3 to, const QueryFilter3D& f = {}) const;

    // ---- editor / test surface ---------------------------------------------------------------------
    u64 hash_state(u64 seed) const;

    struct Stats {
        u32 bodies = 0;
        u32 active_bodies = 0;  // awake dynamic/kinematic bodies
        u32 characters = 0;
    };
    Stats stats() const;

    struct Impl;

private:
    mem::UniquePtr<Impl> impl_;
};

}  // namespace mce::physics
