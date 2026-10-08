#pragma once
// mce_physics2d components (physics.md §2.1). Plain reflected structs over core math: no solver
// types, no body ids. Pose and velocity are NOT components (D2) — physics owns them during play
// and mirrors the pose into LocalTransform each fixed tick.
//
// Field names are snake_case, which is also their serialized spelling (physics.md §2.1 note).
// Layouts are padding-free on purpose: World::state_hash hashes component bytes.

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/refl.h"

namespace mce::physics {

using math::vec2;

enum class BodyType : u8 { Static, Kinematic, Dynamic };
MCE_ENUM(BodyType, Static, Kinematic, Dynamic)

enum class ShapeType : u8 { Box, Circle, Convex };
MCE_ENUM(ShapeType, Box, Circle, Convex)

inline constexpr u32 kMaxPolygonVertices = 16;

// Box{half, offset, rotation} | Circle{radius, offset} | Convex{vertices} — one shape per
// collider, tagged by `type`. All values are in the body's local frame (metres, radians).
struct ColliderShape {
    ShapeType type = ShapeType::Box;
    u8 vertex_count = 0;  // Convex only; any winding, hull is computed at body creation
    u8 pad_[2] = {};
    vec2 half{0.5f, 0.5f};  // Box
    vec2 offset;            // Box, Circle
    f32 rotation = 0.0f;    // Box
    f32 radius = 0.5f;      // Circle
    vec2 vertices[kMaxPolygonVertices];  // Convex

    static ColliderShape box(vec2 half_extents, vec2 offset = {}, f32 rotation = 0.0f) {
        ColliderShape s;
        s.type = ShapeType::Box;
        s.half = half_extents;
        s.offset = offset;
        s.rotation = rotation;
        return s;
    }
    static ColliderShape circle(f32 radius, vec2 offset = {}) {
        ColliderShape s;
        s.type = ShapeType::Circle;
        s.radius = radius;
        s.offset = offset;
        return s;
    }
    static ColliderShape convex(Span<const vec2> points) {
        ColliderShape s;
        s.type = ShapeType::Convex;
        const usize n = points.size() < kMaxPolygonVertices ? points.size() : kMaxPolygonVertices;
        for (usize i = 0; i < n; ++i) s.vertices[i] = points[i];
        s.vertex_count = static_cast<u8>(n);
        return s;
    }
    friend bool operator==(const ColliderShape&, const ColliderShape&) = default;
};
MCE_REFL_STRUCT(ColliderShape, type, vertex_count, half, offset, rotation, radius, vertices)

// Body-level config. One entity = one body.
struct RigidBody2D {
    BodyType type = BodyType::Static;
    bool fixed_rotation = false;
    u8 layer = 0;  // index into the game's collision-layer table (0..15)
    u8 pad_ = 0;
    u16 mask = 0xFFFF;  // layers this body collides with
    u16 pad2_ = 0;
    f32 gravity_scale = 1.0f;
    f32 linear_damping = 0.0f;
    friend bool operator==(const RigidBody2D&, const RigidBody2D&) = default;
};
MCE_COMPONENT(RigidBody2D, type, fixed_rotation, (layer, Range{0, 15}), mask, gravity_scale, linear_damping)

// One collider per body at MVP (compound shapes arrive at v0.5 as child entities).
struct Collider2D {
    ColliderShape shape;
    f32 density = 1.0f;  // 0 = massless (static/kinematic)
    f32 friction = 0.6f;
    f32 restitution = 0.0f;
    bool is_sensor = false;  // FR-PHY-035: overlap events, no response
    bool one_way = false;    // FR-PHY-030: blocks downward controller sweeps only
    u8 pad_[2] = {};
    friend bool operator==(const Collider2D&, const Collider2D&) = default;
};
MCE_COMPONENT(Collider2D, shape, density, (friction, Range{0, 2}), (restitution, Range{0, 1}), is_sensor, one_way)

// Opts the entity into the swept controller (FR-PHY-030). Implies a kinematic body.
struct CharacterController2D {
    f32 max_slope_deg = 45.0f;
    f32 snap_down_distance = 0.05f;  // metres; keeps ground contact on stair descents
    f32 skin_width = 0.005f;
    friend bool operator==(const CharacterController2D&, const CharacterController2D&) = default;
};
MCE_COMPONENT(CharacterController2D, (max_slope_deg, Range{0, 89}), snap_down_distance, skin_width)

struct ControllerFlags {
    static constexpr u32 Ground = 1u << 0;
    static constexpr u32 WallLeft = 1u << 1;
    static constexpr u32 WallRight = 1u << 2;
    static constexpr u32 Ceiling = 1u << 3;
    static constexpr u32 OnMovingPlatform = 1u << 4;
};

// Controller OUTPUT, written by physics each tick, read by gameplay. Physics adds it to
// controller entities that lack it.
struct CharacterGround2D {
    u32 flags = 0;  // ControllerFlags bits
    vec2 ground_normal;
    vec2 platform_velocity;  // carry velocity of the platform underfoot
    Entity ground_body;      // null when airborne
};
MCE_COMPONENT(CharacterGround2D, (flags, ReadOnly, Transient), (ground_normal, ReadOnly, Transient),
              (platform_velocity, ReadOnly, Transient), (ground_body, ReadOnly, Transient))

// ---- events (double-buffered EventQueue<T>, producer FixedUpdate_Physics) -------------------
struct TriggerEnter {  // exactly once per entry (FR-PHY-035)
    Entity sensor;
    Entity other;
};
struct TriggerExit {  // exactly once; also when either body is destroyed
    Entity sensor;
    Entity other;
};
struct ContactBegin2D {
    Entity a;
    Entity b;
    vec2 point;
    vec2 normal;  // from a to b
    f32 approach_speed;
};
struct ContactEnd2D {
    Entity a;
    Entity b;
};

}  // namespace mce::physics
