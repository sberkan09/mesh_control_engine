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

}  // namespace mce::kit
