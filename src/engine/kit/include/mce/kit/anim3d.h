#pragma once
// 3D animation as data (M-DATA-REACH 7): the event an Animator's graph raises and the component
// that pins an entity to a bone (a node) of an animated mesh. install_kit3d registers both; the
// Animator itself (kit3d.h) runs the graph.
//
//   world.events().read<kit::AnimEvent3D>()   // in FixedUpdate_Pre of the next tick, or later
//
// "kit3d_animators" (FixedUpdate_Post) pushes an AnimEvent3D for every event of the playing graph
// state the playhead crosses and moves root-motion entities; "kit3d_ik" (after it) gathers the
// goals of the graphs' IK chains (IKTarget3D entities, the ground under foot chains);
// "kit3d_attach" (after that) writes each AttachToBone entity's LocalTransform from the target's
// pose and the bone's animated matrix.
//
// "kit3d_locomotion" (FixedUpdate_Post, before "kit3d_animators") writes each Locomotion3D's motion
// into its Animator's graph parameters (M-MOVE3D 5).

#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct AnimEvent3D {
    Entity entity;  // the Animator's
    u64 id = 0;     // StringId (fnv1a64) of the event name, as the graph stores it
};

// Places this entity at a bone (any node of the mesh: a joint, a hand, a muzzle) of `target`'s
// MeshRef, posed by the target's Animator, every tick. The entity should be a root (no ChildOf):
// its LocalTransform is overwritten with the world pose. `offset` and `rotation` are in the
// bone's space. An unknown bone or a target without a mesh leaves the entity where it is.
struct AttachToBone {
    Entity target;
    FixedString<32> bone;
    math::vec3 offset;
    math::quat rotation;
    u32 found = 0;  // engine-written: 1 while the bone resolves
};
MCE_COMPONENT(AttachToBone, (target, Help{"the entity with the animated MeshRef"}),
              (bone, Help{"node name in the mesh (a joint, a socket)"}), (offset, Help{"in the bone's space"}),
              (rotation, Help{"in the bone's space"}), (found, ReadOnly, Transient))

// The goal of IK chain `chain` ("two_bone" or "look_at", by its name in the graph) of `animator`'s
// Animator: this entity's world position, every tick (M-GAMEPLAY3D 5). A hand on a ledge, a head
// following the player. `weight` (0..1) fades the chain in and out, times the chain's own weight.
struct IKTarget3D {
    Entity animator;
    FixedString<24> chain;
    f32 weight = 1.0f;
    u32 found = 0;  // engine-written: 1 while the chain resolves
};
MCE_COMPONENT(IKTarget3D, (animator, Help{"the entity whose Animator graph has the chain"}),
              (chain, Help{"the IK chain's name in the graph"}), (weight, Range{0, 1}), (found, ReadOnly, Transient))


// Feeds how an entity moves to an Animator's graph parameters, by their names, so a character's
// run, idle, jump and crouch animate with no code (M-MOVE3D 5). Every tick it measures this
// entity's motion: a CharacterController3D's velocity, grounded and crouched; else a NavAgent3D's
// velocity; else the change of its world position. Then it sets whichever of these parameters the
// graph declares:
//   speed           ground speed, m/s          grounded  1 on the ground, else 0
//   move_x, move_y  ground velocity in the     airborne  1 - grounded
//                   entity's frame (right,     crouch    1 while crouched
//                   forward; forward is -Z)    vertical_speed  m/s, up positive
// With stride_speed set, the Animator plays at ground speed / stride_speed (clamped to
// min_rate..max_rate) while moving and at 1 when still, so feet keep pace with the ground.
// With turn_rate set, a root entity (no ChildOf) turns at up to turn_rate rad/s to face where it
// moves while faster than turn_min_speed: for a player's CharacterController3D, which input moves
// but nothing turns (a NavAgent3D has face_motion, a crowd walker faces its pavement). Its rotation is
// rebuilt as a yaw about +Y: any pitch or roll it had is dropped while it turns.
struct Locomotion3D {
    Entity animator;               // the entity with the Animator (the model); null = this entity
    f32 smoothing = 0.1f;          // s: how quickly the speeds follow the motion (0 = at once)
    f32 stride_speed = 0.0f;       // m/s the moving clips are authored at; 0 = leave Animator.speed
    f32 min_rate = 0.5f;
    f32 max_rate = 2.0f;
    f32 ground_speed = 0.0f;       // engine-written: m/s, smoothed
    f32 vertical_speed = 0.0f;     // engine-written: m/s, smoothed, up positive
    math::vec2 local_move;         // engine-written: m/s in the entity's frame (x right, y forward), smoothed
    math::vec3 last_position;      // engine-written: the world position last tick
    u8 grounded = 0;               // engine-written
    u8 crouched = 0;               // engine-written
    u8 seen = 0;                   // engine-written: 1 once last_position holds a position
    u8 found = 0;                  // engine-written: parameters the graph declares, as a count
    f32 turn_rate = 0.0f;          // rad/s it turns to face its motion; 0 = never
    f32 turn_min_speed = 0.3f;     // m/s below which it keeps its facing
};
MCE_COMPONENT(Locomotion3D, (animator, Help{"the entity with the Animator; empty = this entity"}),
              (smoothing, Range{0, 2}, Help{"seconds for the speeds to follow the motion"}),
              (stride_speed, Range{0, 50}, Help{"m/s the moving clips are authored at: play rate = speed / this; 0 = off"}),
              (min_rate, Range{0, 8}), (max_rate, Range{0, 8}), (ground_speed, ReadOnly, Transient),
              (vertical_speed, ReadOnly, Transient), (local_move, ReadOnly, Transient), (last_position, ReadOnly, Transient, WorldPoint),
              (grounded, ReadOnly, Transient), (crouched, ReadOnly, Transient), (seen, ReadOnly, Transient),
              (found, ReadOnly, Transient), (turn_rate, Range{0, 60}, Help{"rad/s a root entity turns to face where it moves; 0 = never"}),
              (turn_min_speed, Range{0, 20}, Help{"m/s below which it keeps its facing"}))

}  // namespace mce::kit
