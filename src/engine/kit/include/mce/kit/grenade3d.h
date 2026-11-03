#pragma once
// Thrown grenades (M-GRENADE). kit::install_stock installs it.
//
//   Grenade3D      on the thrower (a character or the player): a stack of `count` grenades. It does not depend on
//                  Weapon3D or Loadout3D, so it is thrown with any wielded gun.
//   GrenadeLive3D  on a thrown grenade (engine-spawned): its owner, velocity, the fuse left and the blast numbers copied at the
//                  throw, so changing Grenade3D later does not alter a grenade in flight.
//
// A throw starts when `trigger` is set to 1 (the engine clears it), or, with `use_input`, when the action "Throw" is pressed;
// only when `count` > 0, `cooldown` is 0, the thrower is alive and not a seated Rider3D. The grenade leaves the thrower's
// position along its forward (-Z of its own pose) at `throw_speed` plus `lift` m/s up, and `count` drops by one. It falls under
// the physics world's gravity and bounces (`bounce`) off what the physics world's ray finds, if the world has Physics3D; without
// one it just falls. When `fuse` runs out it explodes: every Health3D within `radius` of it takes `damage` falling off
// linearly to 0 at the edge, in entity order, through apply_damage with the thrower as instigator (so Damage3D, Died3D, crimes,
// cash drops and the progress counters work as for a bullet). The blast hurts everyone, the thrower included, and is not
// blocked by walls. A shove on physics bodies is left for later.
// GrenadeThrown3D and GrenadeBlast3D report both. System: "kit_grenade" (FixedUpdate_Pre, after "kit_progress").

#include "mce/asset/handle.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct Grenade3D {
    asset::AssetRef<asset::Mesh> mesh;  // the model that flies; null = an invisible grenade
    f32 damage = 120.0f;                // at the centre of the blast
    f32 radius = 6.0f;                  // m
    f32 fuse = 2.5f;                    // s from the throw to the blast
    f32 throw_speed = 14.0f;            // m/s along the thrower's forward
    f32 lift = 4.0f;                    // m/s up on top of that, so a throw arcs
    f32 bounce = 0.4f;                  // 0..1, the share of the speed kept along the normal after a hit
    f32 rate = 1.0f;                    // throws/s
    f32 scale = 0.15f;                  // the model's scale
    f32 cooldown = 0.0f;                // engine-written, s until the next throw
    u16 count = 3;                      // grenades in the stack
    u16 max_count = 5;                  // what the ammo shop restocks to
    u8 use_input = 0;                   // 1: the action Throw throws one
    u8 trigger = 0;                     // set to 1 to throw one (the engine clears it)
    u16 pad0_ = 0;
    u32 thrown = 0;                     // engine-written
};
MCE_COMPONENT(Grenade3D, mesh, (damage, Save, Range{0, 100000}), (radius, Save, Range{0.1f, 1000}), (fuse, Save, Range{0, 60}),
              (throw_speed, Save, Range{0, 1000}), (lift, Save, Range{0, 1000}), (bounce, Save, Range{0, 1}),
              (rate, Save, Range{0.01f, 100}), (scale, Range{0.01f, 100}), (cooldown, Save, ReadOnly), (count, Save),
              (max_count, Save), (use_input, Help{"1: the action Throw throws one"}),
              (trigger, Help{"set to 1 to throw one (the engine clears it)"}), (thrown, ReadOnly, Transient))

struct GrenadeLive3D {
    Entity owner;           // credited for the damage
    math::vec3 velocity;    // m/s
    f32 timer = 0.0f;       // s until the blast
    f32 damage = 120.0f;
    f32 radius = 6.0f;
    f32 bounce = 0.4f;
    u32 pad_ = 0;
};
MCE_COMPONENT(GrenadeLive3D, (owner, Help{"credited for the damage"}), (velocity, Help{"m/s"}), (timer, Range{0, 60}),
              (damage, Range{0, 100000}), (radius, Range{0.1f, 1000}), (bounce, Range{0, 1}))

// Pushed when a grenade leaves the thrower.
struct GrenadeThrown3D {
    Entity entity;     // the thrower
    u32 left = 0;      // grenades still in the stack
    u32 pad_ = 0;
};
MCE_EVENT(GrenadeThrown3D, entity, left)

// Pushed when a grenade explodes.
struct GrenadeBlast3D {
    Entity entity;        // the grenade (destroyed this tick)
    Entity owner;
    math::vec3 point;
    u32 hits = 0;         // Health3D entities hurt
};
MCE_EVENT(GrenadeBlast3D, entity, owner, point, hits)

// Registers the components and events and adds the system. Idempotent.
void install_grenade3d(World& world);

}  // namespace mce::kit
