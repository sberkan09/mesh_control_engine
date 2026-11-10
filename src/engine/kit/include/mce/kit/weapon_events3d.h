#pragma once
// Reload and dry-fire events of Weapon3D (M-WEAPONFEEL). Pushed by the "kit3d_weapons" system (FixedUpdate_Pre) next to
// WeaponFired3D; `owner` is Weapon3D::owner, or the weapon's own entity when that is null.
//
//   WeaponReloadStarted3D  the tick a reload begins (Reload action, `reload` set, or an empty magazine with rounds to
//   load) WeaponReloaded3D       the tick it finishes; `rounds` is how many went into the magazine WeaponDry3D a
//   trigger pull on an empty gun that cannot reload (no reserve); at most one per 1 / `rate` s

#include "mce/ecs/entity.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct WeaponReloadStarted3D {
    Entity weapon;
    Entity owner;
};
MCE_EVENT(WeaponReloadStarted3D, weapon, owner)

struct WeaponReloaded3D {
    Entity weapon;
    Entity owner;
    u32 rounds = 0;
};
MCE_EVENT(WeaponReloaded3D, weapon, owner, rounds)

struct WeaponDry3D {
    Entity weapon;
    Entity owner;
};
MCE_EVENT(WeaponDry3D, weapon, owner)

}  // namespace mce::kit
