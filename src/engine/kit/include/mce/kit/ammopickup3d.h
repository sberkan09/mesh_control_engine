#pragma once
// Ammo crates (M-AMMOPICKUP). kit::install_stock installs it.
//
//   AmmoPickup3D  on a placed entity (a crate), modelled on CashPickup3D: the first entity with a Weapon3D that comes within
//                 `radius` gets `rounds` more spare rounds, never past `max_reserve`. With a Loadout3D every stored preset is
//                 topped up the same way, and `grenades` are added to a Grenade3D up to its `max_count`. If nothing could be
//                 added (all full) the crate stays. Otherwise AmmoPickedUp3D is pushed and the crate is destroyed
//                 (`respawn` 0) or waits `respawn` s (engine-written `left`) before it can be taken again.
// Weapons with `magazine` 0 or `infinite_ammo` are ignored. System "kit3d_ammopickup" (FixedUpdate_Pre, after "kit3d_cashpickup").

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct AmmoPickup3D {
    u32 rounds = 30;         // spare rounds given to each weapon
    u32 max_reserve = 150;   // a reserve is never filled past this
    f32 radius = 1.5f;       // m, from the entity
    f32 respawn = 0.0f;      // s until it can be taken again; 0: destroyed when taken
    f32 left = 0.0f;         // engine-written: s until it can be taken
    u16 grenades = 0;        // grenades given to a Grenade3D, up to its max_count
    u16 pad_ = 0;
};
MCE_COMPONENT(AmmoPickup3D, (rounds, Range{0, 100000}, Help{"spare rounds given to each weapon"}),
              (max_reserve, Range{0, 100000}, Help{"a reserve is never filled past this"}), (radius, Range{0.1f, 100}, Help{"m"}),
              (respawn, Range{0, 100000}, Help{"s before it can be taken again; 0: destroyed when taken"}), (left, ReadOnly, Transient),
              (grenades, Range{0, 100}, Help{"grenades given to a Grenade3D"}))

// A holder took an AmmoPickup3D.
struct AmmoPickedUp3D {
    Entity entity;
    Entity pickup;
    u32 rounds = 0;     // spare rounds added over all weapons
    u32 grenades = 0;
};
MCE_EVENT(AmmoPickedUp3D, entity, pickup, rounds, grenades)

// Registers the component and event and adds the system. Idempotent.
void install_ammopickup3d(World& world);

}  // namespace mce::kit
