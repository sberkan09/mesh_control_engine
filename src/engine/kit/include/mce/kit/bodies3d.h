#pragma once
// 3D bodies as components (M-DATA-REACH 6): dynamic and kinematic rigid bodies, a capsule
// character controller and trigger volumes on top of kit3d's Physics3D world, so a 3D scene
// authored in the editor or over MCP moves without game code. kit::install_stock installs them.
//
//   RigidBody3D            a dynamic or kinematic body: box, sphere, capsule or the convex hull of a mesh
//   CharacterController3D  a walking capsule (slopes, steps, ground snap) moved by `move` / `jump`
//                          or by the player's Move / Jump input relative to the 3D camera
//   Trigger3D              a box or sphere volume raising TriggerEnter3D / TriggerExit3D events for
//                          the bodies and characters whose bounding spheres overlap it
//
// The entity's position is the body's centre (a character's capsule centre). Bodies and
// characters are created the first tick an entity has the component (scene or runtime spawn) and
// destroyed with the component or the entity. Moving a body's or a static collider's entity from
// outside (the editor, MCP, a sequence) teleports the body there; otherwise physics writes the
// pose back into LocalTransform after each step. A body's entity should be a root (no ChildOf):
// the pose is world space.
//
// Systems: "kit3d_bodies" (FixedUpdate_Pre: create, teleport, velocities, characters), after
// "kit3d_physics" in FixedUpdate_Physics "kit3d_writeback" (poses into LocalTransform) and
// "kit3d_triggers" (the events; FixedUpdate_Physics is their producer phase, so FixedUpdate_Post
// systems read them in the same tick).

#include "mce/asset/handle.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

enum class BodyType3DKind : u8 { Dynamic, Kinematic };
MCE_ENUM(BodyType3DKind, Dynamic, Kinematic)

enum class BodyShape3D : u8 { Box, Sphere, Capsule, ConvexMesh };
MCE_ENUM(BodyShape3D, Box, Sphere, Capsule, ConvexMesh)

struct RigidBody3D {
    asset::AssetRef<asset::Mesh> hull;  // ConvexMesh: the hull of this mesh's vertices (entity scale applied)
    BodyType3DKind type = BodyType3DKind::Dynamic;
    BodyShape3D shape = BodyShape3D::Box;
    u8 layer = 0;                       // collision layer 0..15
    bool set_velocity = false;          // dynamic: apply `velocity` once (the engine clears it)
    u16 mask = 0xFFFF;                  // layers this body collides with
    bool stop_spin = false;             // dynamic: zero the body's spin once (a respawn; the engine clears it)
    u8 pad_ = 0;
    math::vec3 half{0.5f, 0.5f, 0.5f};  // Box half extents
    f32 radius = 0.5f;                  // Sphere, Capsule
    f32 half_height = 0.5f;             // Capsule: half the cylinder between the caps (along Y)
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    f32 density = 1000.0f;              // kg/m^3
    f32 gravity_scale = 1.0f;
    f32 linear_damping = 0.05f;
    math::vec3 velocity;                // kinematic: m/s every tick; dynamic: with set_velocity
    math::vec3 impulse;                 // dynamic: N*s applied once at the centre of mass (the engine zeroes it)
    math::vec3 current_velocity;        // engine-written: the body's velocity after the last step
    u32 pad2_ = 0;
};
MCE_COMPONENT(RigidBody3D, (hull, Help{"ConvexMesh: a mesh whose vertices make the hull"}), type, shape,
              (layer, Range{0, 15}), (set_velocity, Help{"dynamic: apply 'velocity' once (the engine clears it)"}), mask,
              (stop_spin, Help{"dynamic: zero the spin once, e.g. on a respawn (the engine clears it)"}),
              (half, Help{"Box half extents, metres"}), (radius, Range{0.01f, 100}), (half_height, Range{0, 100}),
              (friction, Range{0, 2}), (restitution, Range{0, 1}), (density, Range{1, 100000}), (gravity_scale, Range{-10, 10}),
              (linear_damping, Range{0, 10}), (velocity, Help{"kinematic: m/s every tick; dynamic: with set_velocity"}),
              (impulse, Help{"dynamic: N*s applied once (the engine zeroes it)"}), (current_velocity, ReadOnly, Transient))

struct CharacterController3D {
    f32 radius = 0.3f;
    f32 half_height = 0.6f;             // cylinder half height: total height = 2 * (half_height + radius)
    f32 max_slope_deg = 50.0f;
    f32 step_height = 0.35f;
    f32 stick_to_floor = 0.5f;
    f32 speed = 5.0f;                   // m/s at full Move input
    f32 jump_speed = 5.0f;              // m/s up
    f32 gravity_scale = 1.0f;
    math::vec3 move;                    // m/s along the ground (y ignored), when use_input is off
    u8 layer = 0;
    bool use_input = false;             // Move / Jump input, relative to the primary 3D camera's yaw
    bool jump = false;                  // set to jump when grounded (the engine clears it)
    bool grounded = false;              // engine-written
    u16 mask = 0xFFFF;
    u16 pad_ = 0;
    math::vec3 velocity;                // engine-written: after the last move
    u32 flags = 0;                      // engine-written: physics::ControllerFlags3D
    // Feel (M-MOVE3D 1). The defaults keep the original controller: full speed at once, a jump only
    // on the tick it is asked for while grounded.
    f32 acceleration = 0.0f;            // m/s^2 towards the wanted ground velocity; 0 = at once
    f32 deceleration = 0.0f;            // m/s^2 when there is no Move; 0 = at once
    f32 air_acceleration = 0.0f;        // m/s^2 in the air, either way; 0 = the ground rates
    f32 coyote_time = 0.0f;             // s after walking off a ledge that a jump still works
    f32 jump_buffer = 0.0f;             // s a jump asked for in the air waits for the ground
    f32 jump_cut = 1.0f;                // rising speed is scaled by this when Jump is let go (1 = off)
    f32 sprint_multiplier = 1.5f;       // speed scale while sprinting
    f32 crouch_half_height = 0.0f;      // the cylinder half height crouched; 0 = no crouch
    f32 crouch_multiplier = 0.5f;       // speed scale while crouched
    u8 air_jumps = 0;                   // extra jumps before landing
    bool sprint = false;                // run faster (or the Sprint action, with use_input)
    bool crouch = false;                // crouch (or the Crouch action held, with use_input)
    bool release_jump = false;          // set to cut a rising jump (the engine clears it)
    bool crouched = false;              // engine-written: false again only once there is room to stand
    u8 air_jumps_used = 0;              // engine-written
    u16 airborne_ticks = 0;             // engine-written: ticks since the ground (saturates)
    u16 buffered_ticks = 0;             // engine-written: ticks a buffered jump may still wait
    u8 jumping = 0;                     // engine-written: left the ground by jumping (1; 2 once cut) until landing
    u8 pad2_ = 0;
};
MCE_COMPONENT(CharacterController3D, (radius, Range{0.05f, 10}), (half_height, Range{0, 10}), (max_slope_deg, Range{0, 89}),
              (step_height, Range{0, 2}), (stick_to_floor, Range{0, 2}), (speed, Range{0, 100}, Help{"m/s at full Move input"}),
              (jump_speed, Range{0, 100}), (gravity_scale, Range{-10, 10}), (move, Help{"m/s along the ground when use_input is off"}),
              (layer, Range{0, 15}), (use_input, Help{"walk by the Move / Jump actions, relative to the 3D camera"}),
              (jump, Help{"set to jump when grounded (the engine clears it)"}), (grounded, ReadOnly, Transient), mask,
              (velocity, ReadOnly, Transient), (flags, ReadOnly, Transient),
              (acceleration, Range{0, 1000}, Help{"m/s^2 towards the wanted ground velocity; 0 = at once"}),
              (deceleration, Range{0, 1000}, Help{"m/s^2 to a stop with no Move; 0 = at once"}),
              (air_acceleration, Range{0, 1000}, Help{"m/s^2 in the air; 0 = the ground rates"}),
              (coyote_time, Range{0, 1}, Help{"seconds after leaving a ledge that a jump still works"}),
              (jump_buffer, Range{0, 1}, Help{"seconds a jump pressed early waits for the ground"}),
              (jump_cut, Range{0, 1}, Help{"rising speed scale when Jump is let go (1 = full height always)"}),
              (sprint_multiplier, Range{0, 10}), (crouch_half_height, Range{0, 10}, Help{"cylinder half height crouched; 0 = no crouch"}),
              (crouch_multiplier, Range{0, 10}), (air_jumps, Range{0, 10}),
              (sprint, Help{"run faster (the Sprint action too, with use_input)"}),
              (crouch, Help{"crouch (the Crouch action too, with use_input); stands only with room overhead"}),
              (release_jump, Help{"set to cut a rising jump to jump_cut (the engine clears it)"}),
              (crouched, ReadOnly, Transient), (air_jumps_used, ReadOnly, Transient), (airborne_ticks, ReadOnly, Transient),
              (buffered_ticks, ReadOnly, Transient), (jumping, ReadOnly, Transient))

// A character touched down (M-MOVE3D 1): FixedUpdate_Pre, from "kit3d_bodies".
struct Landed3D {
    Entity entity;
    f32 impact_speed = 0.0f;  // m/s downward at touchdown
};

enum class TriggerShape3D : u8 { Box, Sphere };
MCE_ENUM(TriggerShape3D, Box, Sphere)

struct Trigger3D {
    TriggerShape3D shape = TriggerShape3D::Box;
    u8 pad_[3] = {};
    math::vec3 half{0.5f, 0.5f, 0.5f};  // Box half extents (axis-aligned, scaled by the entity)
    f32 radius = 0.5f;                  // Sphere
    math::vec3 offset;
    u32 inside = 0;                     // engine-written: overlapping bodies and characters now
    u32 enters = 0;                     // engine-written
    Entity last_other;                  // engine-written: the last body to enter or leave
};
MCE_COMPONENT(Trigger3D, shape, (half, Help{"Box half extents, metres (axis-aligned)"}), (radius, Range{0.01f, 1000}), offset,
              (inside, ReadOnly, Transient), (enters, ReadOnly, Transient), (last_other, ReadOnly, Transient))

// Events (producer FixedUpdate_Physics).
struct TriggerEnter3D {
    Entity trigger;
    Entity other;
};
struct TriggerExit3D {  // also when either entity is destroyed
    Entity trigger;
    Entity other;
};
MCE_EVENT(TriggerEnter3D, trigger, other)
MCE_EVENT(TriggerExit3D, trigger, other)

// Registers the components and events and adds the systems (after install_kit3d, which it calls).
// Idempotent. `assets` (may be null) loads ConvexMesh hulls.
void install_bodies3d(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
