#pragma once
// Event sounds (M-GAMEFEEL-SFX): a sound for the game events that were silent. kit::install_stock installs it.
//
//   EventSfx3D  on the entity the events belong to (the player): one clip and gain per event; a null clip is silent.
//               throw        GrenadeThrown3D     whose `entity` holds this
//               blast        GrenadeBlast3D      whose `owner` holds this
//               melee        MeleeSwung3D        whose `entity` holds this
//               cash         CashPickedUp3D      whose `entity` holds this
//               ammo         AmmoPickedUp3D      whose `entity` holds this
//               goal         GoalDone3D          whose `owner` holds this
//               reload_start WeaponReloadStarted3D whose `owner` holds this
//               reloaded     WeaponReloaded3D    whose `owner` holds this
//               dry          WeaponDry3D         whose `owner` holds this
//               switch       WeaponSwitched3D    whose `entity` holds this
//
// Each matching event pushes one audio::PlaySfx (2D, bus SFX) when the audio events exist.
// System: "kit_event_sfx" (FixedUpdate_Post).

#include "mce/asset/handle.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct EventSfx3D {
    asset::AssetRef<asset::Sfx> throw_clip;
    asset::AssetRef<asset::Sfx> blast_clip;
    asset::AssetRef<asset::Sfx> melee_clip;
    asset::AssetRef<asset::Sfx> cash_clip;
    asset::AssetRef<asset::Sfx> ammo_clip;
    asset::AssetRef<asset::Sfx> goal_clip;
    asset::AssetRef<asset::Sfx> reload_start_clip;
    asset::AssetRef<asset::Sfx> reloaded_clip;
    asset::AssetRef<asset::Sfx> dry_clip;
    asset::AssetRef<asset::Sfx> switch_clip;
    f32 throw_db = -6.0f;
    f32 blast_db = 0.0f;
    f32 melee_db = -6.0f;
    f32 cash_db = -8.0f;
    f32 ammo_db = -8.0f;
    f32 goal_db = -4.0f;
    f32 reload_start_db = -8.0f;
    f32 reloaded_db = -8.0f;
    f32 dry_db = -10.0f;
    f32 switch_db = -8.0f;
};
MCE_COMPONENT(EventSfx3D, (throw_clip, Help{"grenade thrown (2D); null: silent"}),
              (blast_clip, Help{"grenade blast of this owner (2D)"}), (melee_clip, Help{"melee swing (2D)"}),
              (cash_clip, Help{"cash picked up (2D)"}), (ammo_clip, Help{"ammo crate picked up (2D)"}),
              (goal_clip, Help{"goal reached (2D)"}), (reload_start_clip, Help{"a reload begins (2D)"}),
              (reloaded_clip, Help{"a reload ends (2D)"}),
              (dry_clip, Help{"trigger pulled on an empty gun with no spare rounds (2D)"}),
              (switch_clip, Help{"weapon switched (2D)"}), (throw_db, Range{-60, 12}), (blast_db, Range{-60, 12}),
              (melee_db, Range{-60, 12}), (cash_db, Range{-60, 12}), (ammo_db, Range{-60, 12}),
              (goal_db, Range{-60, 12}), (reload_start_db, Range{-60, 12}), (reloaded_db, Range{-60, 12}),
              (dry_db, Range{-60, 12}), (switch_db, Range{-60, 12}))

void install_event_sfx3d(World& world);

}  // namespace mce::kit
