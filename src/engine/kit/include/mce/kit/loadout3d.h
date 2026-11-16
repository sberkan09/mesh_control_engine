#pragma once
// Weapon switching (M-WEAPONSWITCH). kit::install_stock installs it.
//
//   WeaponPreset3D  one weapon's numbers on its own entity: damage, range, rate, spread, pellets, magazine, reload time,
//                   automatic, impulse, the shot clip and its own `ammo` / `reserve`.
//   Loadout3D       on the wielder, next to its single Weapon3D: up to three preset entities (`slot0..2`). The player keeps
//                   one Weapon3D, so everything that reads it (the HUD's ammo, WeaponAudio, Confiscate3D, AmmoShop3D,
//                   HitMarker3D, ImpactFx3D) is unchanged; switching copies a preset onto it.
//
// A switch: the leaving weapon's ammo and reserve go back into its preset, a reload in progress is cancelled, the entering
// preset is copied onto the Weapon3D (an equipment damage bonus stays on top), its ammo and reserve are restored (a preset
// that was never used starts with a full magazine), `Weapon3D.cooldown` is raised to `draw_time` so a switch cannot skip the
// fire rate, WeaponAudio.shot takes the preset's clip when it has one and WeaponSwitched3D is pushed.
//
// Requests: set `select` to 1 + slot (the engine clears it); with `use_input` the actions Slot1..Slot3 pick a slot and
// WeaponNext / WeaponPrev cycle through the filled ones. A seated Rider3D, a dead wielder and an empty or current slot are
// ignored. System: "kit_loadout" (FixedUpdate_Pre, after "kit3d_health"; a switch takes effect on the next shot).

#include "mce/asset/handle.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct WeaponPreset3D {
    asset::AssetRef<asset::Sfx> shot;   // WeaponAudio.shot while it is wielded; null keeps the current clip
    u32 ammo = 0;                       // rounds in it (engine-written while another weapon is wielded)
    u32 reserve = 0;                    // spare rounds
    f32 damage = 20.0f;                 // per pellet
    f32 range = 100.0f;                 // m
    f32 rate = 5.0f;                    // shots/s
    f32 spread_deg = 0.0f;
    f32 impulse = 0.0f;
    f32 reload_time = 1.5f;             // s
    u16 pellets = 1;
    u16 magazine = 0;                   // 0 = never runs dry
    u8 automatic = 1;
    u8 primed = 0;                      // engine-written: 1 once `ammo` holds a real count
    u16 pad_ = 0;
};
MCE_COMPONENT(WeaponPreset3D, (shot, Help{"WeaponAudio.shot while wielded; null keeps the current clip"}),
              (ammo, Save), (reserve, Save), (damage, Save, Range{0, 100000}), (range, Save, Range{0, 100000}),
              (rate, Save, Range{0.01f, 1000}), (spread_deg, Save, Range{0, 90}), (impulse, Save, Range{0, 100000}),
              (reload_time, Save, Range{0, 60}), (pellets, Save, Range{1, 64}), (magazine, Save, Range{0, 10000}),
              (automatic, Save, Range{0, 1}), (primed, Save, ReadOnly))

struct Loadout3D {
    Entity slot0;                       // WeaponPreset3D entities; null = empty
    Entity slot1;
    Entity slot2;
    u8 current = 0;                     // the wielded slot, 0 to 2
    u8 use_input = 0;                   // 1: Slot1..Slot3 and WeaponNext / WeaponPrev switch
    u8 select = 0;                      // set to 1 + slot to switch (the engine clears it)
    u8 started = 0;                     // engine-written: the current preset has been applied
    f32 draw_time = 0.4f;               // s after a switch before the next shot
    u32 switches = 0;                   // engine-written
    u32 pad_ = 0;
};
MCE_COMPONENT(Loadout3D, (slot0, Help{"a WeaponPreset3D entity"}), (slot1, Help{"a WeaponPreset3D entity"}),
              (slot2, Help{"a WeaponPreset3D entity"}), (current, Save, Range{0, 2}),
              (use_input, Help{"1: Slot1..3 and WeaponNext / WeaponPrev switch"}),
              (select, Help{"set to 1 + slot to switch (the engine clears it)"}), (started, Save, ReadOnly),
              (draw_time, Save, Range{0, 10}), (switches, ReadOnly, Transient))

// Pushed when the wielded slot changes.
struct WeaponSwitched3D {
    Entity entity;   // the wielder
    u32 from = 0;
    u32 to = 0;
};
MCE_EVENT(WeaponSwitched3D, entity, from, to)

// Registers the components and adds the system. Idempotent.
void install_loadout3d(World& world);

}  // namespace mce::kit
