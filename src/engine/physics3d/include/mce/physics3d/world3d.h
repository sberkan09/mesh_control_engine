#pragma once
// World3D — the 3D physics world, a facade over embedded Jolt Physics 5 (ADR-0003, physics.md §3.7;
// ROADMAP M5b).
//
//  - static / kinematic / dynamic bodies; box, sphere, capsule, convex-hull, triangle-mesh and height-field
//    shapes
//  - CharacterVirtual-driven capsule controller (move_character) and the camera-boom sphere sweep
//    (camera_sweep) — the two 3D additions to World2D's method set (physics.md §2.2)
//  - raycasts and sphere casts, sorted and filtered like World2D's
//  - joints between two bodies or a body and the world (fixed, hinge, slider, distance, cone,
//    six-DOF) with limits, soft limits, friction, motors and a break force (M-GAMEPLAY3D step 2)
//  - ragdolls: a tree of capsule / sphere bodies joined by swing-twist joints (M-GAMEPLAY3D step 3)
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
enum class ShapeType3D : u8 { Box, Sphere, Capsule, Convex, Mesh, HeightField };

// A height-field sample that is a hole: the triangles touching it do not collide.
inline constexpr f32 kHeightFieldHole = 3.40282347e38f;  // FLT_MAX

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
    Span<const f32> heights;       // HeightField: samples² heights, rows along +z of samples along +x
    u32 samples = 0;               // HeightField: samples per side
    vec3 origin;                   // HeightField: where sample (0, 0) at height 0 sits
    vec3 spacing{1.0f, 1.0f, 1.0f};  // HeightField: metres between samples along x and z; y scales the heights

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
    // A terrain (M-TERRAIN 3), static bodies only: sample (x, z) at origin + spacing * (x, heights[z * samples + x], z),
    // each quad split along its (x, z)-(x + 1, z + 1) diagonal; kHeightFieldHole samples open holes.
    static Shape3D height_field(Span<const f32> heights, u32 samples, vec3 origin, vec3 spacing) {
        Shape3D s;
        s.type = ShapeType3D::HeightField;
        s.heights = heights;
        s.samples = samples;
        s.origin = origin;
        s.spacing = spacing;
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

// ---- joints (M-GAMEPLAY3D step 2) -----------------------------------------------------------------

enum class JointType3D : u8 {
    Fixed,     // no relative motion
    Hinge,     // turns about `axis` through `anchor` (a door, a wheel, a drawbridge)
    Slider,    // slides along `axis`, no turning (a piston, a lift)
    Distance,  // keeps the anchors between limit_min and limit_max apart (a rope, a chain link)
    Cone,      // a ball joint whose `axis` swings at most cone_half_angle away (a shoulder)
    SixDOF,    // each of the 6 axes locked, free or limited (anything else)
};
enum class JointMotorMode3D : u8 { Off, Velocity, Position };

// Everything in world space, at the moment the joint is created: the bodies' current poses are
// the joint's rest pose (hinge angle 0, slider position 0).
struct JointDesc3D {
    JointType3D type = JointType3D::Hinge;
    JointMotorMode3D motor = JointMotorMode3D::Off;  // Hinge and Slider only
    bool limits = false;             // Hinge, Slider: keep the angle / position in [limit_min, limit_max]
    bool collide_connected = false;  // the two bodies still collide with each other
    Entity a;                        // a body (created by create_body)
    Entity b;                        // the other body, or null: `a` is held to the world
    vec3 anchor;                     // the pivot (Distance: the point on a)
    vec3 anchor_b;                   // Distance: the point on b (or in the world)
    vec3 axis{0.0f, 1.0f, 0.0f};     // Hinge, Slider: the axis; Cone: the twist axis; SixDOF: the frame's X
    vec3 normal;                     // SixDOF: the frame's Y (zero: any perpendicular to `axis`)
    // Hinge: radians, min in [-pi, 0], max in [0, pi]. Slider: metres, min <= 0 <= max. Distance: metres
    // (always applied; < 0 takes the anchors' distance at creation).
    f32 limit_min = 0.0f;
    f32 limit_max = 0.0f;
    f32 cone_half_angle = 0.5f;      // Cone: radians
    // SixDOF, per axis (x, y, z of the frame): min > max = free, min == max = locked there, else limited.
    // Linear in metres, angular in radians (angular y and z in [-pi, pi], x in [-pi, pi]).
    vec3 linear_min, linear_max;
    vec3 angular_min, angular_max;
    f32 spring_frequency = 0.0f;  // > 0: Hinge / Slider / Distance limits are a spring of this Hz (soft)
    f32 spring_damping = 0.0f;    // the spring's damping ratio (0..1)
    f32 friction = 0.0f;          // Hinge: N*m, Slider: N, holding against motion
    f32 motor_target = 0.0f;      // Velocity: rad/s or m/s; Position: rad or m
    f32 motor_max_force = 0.0f;   // N (Slider) or N*m (Hinge); 0 = unlimited
    f32 motor_frequency = 2.0f;   // Position: how stiffly the motor drives to the target (Hz)
    f32 motor_damping = 1.0f;     // Position: damping ratio
    f32 break_force = 0.0f;       // N; the joint breaks (stops acting) above it; 0 = never
    f32 break_torque = 0.0f;      // N*m; likewise for turning; 0 = never
};

struct JointState3D {
    bool enabled = false;  // acting (not broken, not switched off)
    bool broken = false;   // broke under break_force / break_torque
    f32 position = 0.0f;   // Hinge: angle (rad); Slider: position (m); Distance: distance (m); others 0
    f32 force = 0.0f;      // the linear force it held with in the last step (N)
    f32 torque = 0.0f;     // the torque it held with in the last step (N*m)
};

// ---- ragdolls (M-GAMEPLAY3D step 3) ----------------------------------------------------------------

// One body of a ragdoll, in world space at the moment the ragdoll is created.
struct RagdollPart3D {
    i32 parent = -1;  // index of the parent part, lower than this part's own; -1 = the root (part 0 only)
    ShapeType3D shape = ShapeType3D::Capsule;  // Capsule, Sphere or Box
    f32 radius = 0.1f;                         // Capsule, Sphere
    f32 half_height = 0.1f;                    // Capsule: half the cylinder, along the body's local Y
    vec3 half{0.1f, 0.1f, 0.1f};               // Box
    vec3 position;                             // the body's centre
    quat rotation;
    f32 mass = 1.0f;                           // kg
    vec3 pivot;                                // the joint to the parent part (ignored for the root)
    vec3 twist_axis{0.0f, 1.0f, 0.0f};         // the axis the part twists about at the pivot
    f32 swing = 0.785398f;                     // how far the twist axis may swing (rad)
    f32 twist = 0.523599f;                     // how far the part may twist either way (rad)
    vec3 linear_velocity;
    vec3 angular_velocity;
};

struct RagdollDesc3D {
    Entity owner;  // reported by queries that hit a part (each part's user data)
    u8 layer = 0;
    u16 mask = 0xFFFF;
    f32 friction = 0.5f;          // contact friction
    f32 linear_damping = 0.05f;
    f32 angular_damping = 0.5f;
    f32 joint_friction = 0.0f;    // N*m each joint holds against motion
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
    // Changes config().gravity from the next step on (a scene's WorldSettings, M-DATA-REACH 3).
    void set_gravity(vec3 gravity);

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
    void set_angular_velocity(Entity e, vec3 v);  // kinematic + dynamic bodies, rad/s
    void apply_impulse(Entity e, vec3 impulse);  // dynamic bodies, at the centre of mass
    void teleport(Entity e, vec3 position, quat rotation);  // bodies and characters

    bool body_state(Entity e, BodyState3D& out) const;

    // ---- character controller (FR-PHY-080). `delta` = intended displacement this tick, gravity
    // and jumps included (gameplay owns them, as in 2D). Unknown entities return a zero result.
    MoveResult3D move_character(Entity e, vec3 delta, const ControllerParams3D& params = {});
    // Last move result.
    bool controller_state(Entity e, MoveResult3D& out) const;

    // ---- joints ---------------------------------------------------------------------------------------
    // A joint between a.body and b.body (or the world). 0 when a body is missing (characters are not
    // bodies), both are static, a == b, or the description is malformed. Destroying either body
    // destroys the joint.
    u32 create_joint(const JointDesc3D& desc);
    void destroy_joint(u32 joint);
    bool has_joint(u32 joint) const;
    // Motor mode and target from now on (Hinge, Slider).
    void set_joint_motor(u32 joint, JointMotorMode3D mode, f32 target, f32 max_force = 0.0f);
    // Switches a joint off and on again (a broken joint stays off).
    void set_joint_enabled(u32 joint, bool enabled);
    bool joint_state(u32 joint, JointState3D& out) const;

    // ---- ragdolls -------------------------------------------------------------------------------------
    // The parts as bodies and swing-twist joints between each part and its parent. Neighbouring
    // parts (a part and its parent, and parts that overlap at creation) never collide; parts of
    // different ragdolls do. 0 when there are no parts, a parent index is out of order, a shape is
    // malformed or the world is out of bodies.
    u32 create_ragdoll(const RagdollDesc3D& desc, Span<const RagdollPart3D> parts);
    void destroy_ragdoll(u32 ragdoll);
    bool has_ragdoll(u32 ragdoll) const;
    u32 ragdoll_part_count(u32 ragdoll) const;
    // The parts' poses now, in creation order (`out` holds ragdoll_part_count() states; entity = the owner).
    bool ragdoll_state(u32 ragdoll, Span<BodyState3D> out) const;
    // An impulse at the centre of mass of one part, or (part < 0) spread over every part by mass.
    void ragdoll_impulse(u32 ragdoll, i32 part, vec3 impulse);
    // True while any part is awake.
    bool ragdoll_active(u32 ragdoll) const;

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
        u32 joints = 0;
        u32 ragdolls = 0;
    };
    Stats stats() const;

    struct Impl;

private:
    mem::UniquePtr<Impl> impl_;
};

}  // namespace mce::physics
