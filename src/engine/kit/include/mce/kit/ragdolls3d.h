#pragma once
// Ragdolls as a component (M-GAMEPLAY3D step 3): a skinned character that goes limp, falls, and
// blends back into its animation, on physics::World3D's ragdolls (Jolt). kit::install_stock
// installs it.
//
//   Ragdoll3D  on an entity with a skinned MeshRef (and usually an Animator)
//
// With `on_trigger`, a TriggerEnter3D naming the entity (a Trigger3D it walks into, a Raycast3D or
// Overlap3D that touches it, or its own sensor touching something) sets `active` and clears its
// NavAgent3D's `move_to`: death by a cart, a trap or a fall, with no code.
//
// Setting `active` builds the ragdoll from skin 0's skeleton as the mesh is posed at that moment,
// moving as the animation was moving (and as the entity's own body, if it has one): one body per
// bone, a capsule from the bone to its children, a box for a bone with several children (a pelvis,
// a chest), a capsule or sphere for a leaf (a hand, the head), sized from the vertices the bone
// moves, joined to its parent by a swing-twist joint. Bones that move no vertex, or are shorter
// than `min_bone_length`, ride with their parent. The mesh's sidecar can exclude or include a bone
// and set its radius, swing, twist and mass (cook "ragdoll" settings, user guide 9.8).
//
// While active the physics poses the mesh (rendering and AttachToBone alike); the entity's own
// transform stays where it was, and `root_position` says where the root bone is now (to move the
// entity there before getting up). Clearing `active` removes the bodies and blends from the last
// physics pose back to the animation over `blend_time`. Other settings are read when the ragdoll
// is built.
//
// The parts collide with everything on `mask` and each other (not with their neighbours). They
// also collide with the entity's own body or character controller when the layers allow it: put
// the ragdoll on a layer the controller ignores, or remove the controller while it is limp.
//
// Systems: "kit3d_ragdolls" (FixedUpdate_Pre, after "kit3d_bodies": build, impulses, remove, blend
// back) and "kit3d_ragdoll_pose" (FixedUpdate_Physics, after "kit3d_writeback": the pose, `settled`,
// `root_position`).

#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct Ragdoll3D {
    u8 active = 0;                 // 1: the physics takes the skeleton; 0: back to the animation
    u8 layer = 0;                  // the parts' collision layer (0..15)
    u16 mask = 0xFFFF;             // the layers the parts collide with
    f32 mass = 70.0f;              // kg, shared by the parts by size (and the sidecar's weights)
    f32 blend_time = 0.3f;         // seconds from the physics pose back to the animation
    f32 min_bone_length = 0.05f;   // metres: shorter bones ride with their parent
    f32 radius_scale = 1.0f;       // scales every part's thickness
    f32 swing = 45.0f;             // degrees a bone may swing from its pose at activation
    f32 twist = 20.0f;             // degrees a bone may twist either way
    f32 friction = 0.6f;           // contact friction of the parts
    f32 damping = 0.5f;            // angular damping of the parts
    f32 joint_friction = 0.0f;     // N*m each joint holds against motion (a stiffer body)
    math::vec3 impulse;            // N*s, applied once (when active) and cleared: a hit, a blast
    FixedString<32> impulse_bone;  // the bone `impulse` lands on; empty = the whole body alike
    u32 parts = 0;                 // engine-written: bodies in the ragdoll (0 while inactive)
    f32 blend = 0.0f;              // engine-written: 1 = the physics poses the mesh, 0 = the animation
    math::vec3 root_position;      // engine-written: the root bone's world position
    u32 settled = 0;               // engine-written: 1 once every part has come to rest
    u8 on_trigger = 0;             // 1: goes limp (active = 1) when a TriggerEnter3D names this entity
    u8 pad_[3] = {};
};
MCE_COMPONENT(Ragdoll3D, (active, Help{"1: limp, the physics poses the mesh; 0: blend back to the animation"}),
              (layer, Range{0, 15}), (mask, Help{"the layers the parts collide with"}), (mass, Range{0.1f, 100000}, Help{"kg"}),
              (blend_time, Range{0, 10}, Help{"seconds back to the animation"}),
              (min_bone_length, Range{0, 10}, Help{"metres: shorter bones ride with their parent"}), (radius_scale, Range{0.1f, 10}),
              (swing, Range{0, 175}, Help{"degrees"}), (twist, Range{0, 175}, Help{"degrees, either way"}), (friction, Range{0, 1}),
              (damping, Range{0, 1}, Help{"angular damping"}), (joint_friction, Range{0, 100000}, Help{"N*m"}),
              (impulse, Help{"N*s, applied once and cleared"}), (impulse_bone, Help{"empty = the whole body"}),
              (parts, ReadOnly, Transient), (blend, ReadOnly, Transient), (root_position, ReadOnly, Transient),
              (settled, ReadOnly, Transient),
              (on_trigger, Help{"1: goes limp when a trigger or sensor event names it (a hit, a fall); stops its NavAgent3D"}))

// Registers Ragdoll3D and adds the systems (after install_bodies3d, which it calls). Idempotent.
void install_ragdolls3d(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
