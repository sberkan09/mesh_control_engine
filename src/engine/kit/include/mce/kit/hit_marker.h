#pragma once
// Hit marker (M-HITMARKER): feedback that a shot landed. kit::install_stock installs it.
//
//   HitMarker3D  on the shooter (the player): every WeaponHit3D whose `owner` holds it and whose target is another
//                living entity with a Health3D sets `timer` to `duration`; a Died3D it is the `killer` of sets
//                `kill_timer`. Both count down in seconds; a HUD panel bound to them (`range` [0.001, ...]) shows
//                the marker only while they are above 0. `hits` and `kills` count them (engine-written).
//
// System: "kit_hit_marker" (FixedUpdate_Pre, after "kit3d_health").

#include "mce/asset/handle.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct HitMarker3D {
    asset::AssetRef<asset::Sfx> hit_clip;   // 2D tick on a landed shot; null: silent
    asset::AssetRef<asset::Sfx> kill_clip;  // 2D ding on a kill; null: silent
    f32 duration = 0.15f;  // seconds a hit marker stays up
    f32 timer = 0.0f;      // engine-written: seconds left of the hit marker
    f32 kill_timer = 0.0f; // engine-written: seconds left of the kill marker
    u32 hits = 0;          // engine-written
    f32 gain_db = -6.0f;
    u32 pad_ = 0;
};
MCE_COMPONENT(HitMarker3D, (hit_clip, Help{"tick sound of a landed shot (2D)"}), (kill_clip, Help{"ding of a kill (2D)"}),
              (gain_db, Range{-60, 12}), (duration, Save, Range{0.01f, 2}, Help{"seconds a marker stays up"}),
              (timer, ReadOnly, Transient, Help{"> 0 while the hit marker shows"}),
              (kill_timer, ReadOnly, Transient, Help{"> 0 while the kill marker shows"}),
              (hits, ReadOnly, Transient))

// Registers the component and adds the system. Idempotent.
void install_hit_marker(World& world, asset::AssetSystem* assets = nullptr);

}  // namespace mce::kit
