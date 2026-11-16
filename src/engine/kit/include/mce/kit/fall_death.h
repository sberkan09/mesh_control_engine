#pragma once
// A safety net under the world (M-FALLDEATH). kit::install_stock installs it.
//
//   FallDeath3D  on an entity with a Health3D: when its LocalTransform y drops below `min_y` it takes lethal damage
//                (apply_damage, so Damage3D and Died3D fire and a Respawn3D on the entity wakes it at the nearest
//                hospital). A hole in the ground, a missed collider or a launch off the map ends in WASTED instead of
//                an endless fall. An invulnerable entity is left falling; a dead one is skipped.
//
// System: "kit_fall_death" (FixedUpdate_Pre, after "kit3d_health").

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct FallDeath3D {
    f32 min_y = -30.0f;   // metres; below this the entity dies
    u32 pad_ = 0;
};
MCE_COMPONENT(FallDeath3D, (min_y, Save, Range{-100000, 0}, Help{"world y below which the entity dies"}))

// Registers the component and adds the system. Idempotent.
void install_fall_death(World& world);

}  // namespace mce::kit
