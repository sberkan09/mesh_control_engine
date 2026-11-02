#pragma once
// Weapon switching (M-OPENWORLD-GAPS 2b). A `Loadout3D` on a holder (the player) names up to four weapon entities, each a
// `Weapon3D` of its own (a child of the holder, `owner` the holder, usually `aim_from` the camera), and says which one is in
// hand. Nothing is copied between slots: every weapon keeps its own magazine, reserve, reload and cooldown, so switching
// loses no state; a weapon in the pack just stops being driven.
//
// Input (`use_input` 1): the NextWeapon and PrevWeapon actions cycle over the filled slots, Weapon1..Weapon4 pick one.
// Scripts and AI set `select` (1..4) instead; the engine clears it. The weapon in hand gets the Fire and Reload input
// (its `Weapon3D::use_input` is set from the loadout), the others get none and have `fire` and `reload` cleared. A
// switch makes the new weapon wait `switch_time` seconds before its first shot.
//
// The loadout mirrors the weapon in hand (`ammo`, `magazine`, `reserve`) so a HUD can bind "Loadout3D/ammo".
//
// Things that look for "the holder's weapon" (the ammo shop, confiscation) use `held_weapon` and `weapons_of`: the weapon
// in hand, or all of them, falling back to a `Weapon3D` on the holder itself when it has no loadout.
//
// "kit3d_loadout" (FixedUpdate_Pre, after "kit3d_weapons"; the switch shows from the next tick).

#include "mce/ecs/entity.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}  // namespace mce

namespace mce::kit {

struct Weapon3D;

struct Loadout3D {
    Entity slot0;                   // weapon entities (Weapon3D); null = empty
    Entity slot1;
    Entity slot2;
    Entity slot3;
    f32 switch_time = 0.4f;         // s a weapon just drawn waits before it can shoot
    u8 active = 0;                  // slot in hand (0..3)
    u8 select = 0;                  // set to 1..4 to draw that slot (the engine clears it)
    u8 use_input = 0;               // 1: NextWeapon, PrevWeapon and Weapon1..Weapon4 switch it, Fire and Reload drive the weapon in hand
    u8 applied = 0;                 // engine-written: the slot (+ 1) last drawn; 0 before the first tick
    u16 ammo = 0;                   // engine-written: the weapon in hand
    u16 magazine = 0;               // engine-written
    u32 reserve = 0;                // engine-written
};
MCE_COMPONENT(Loadout3D, (slot0, Help{"weapon entity"}), (slot1, Help{"weapon entity"}), (slot2, Help{"weapon entity"}),
              (slot3, Help{"weapon entity"}), (switch_time, Range{0, 10}, Help{"s a drawn weapon waits before it shoots"}),
              (active, Save, Range{0, 3}, Help{"slot in hand"}), (select, Range{0, 4}, Help{"set to 1..4 to draw that slot"}),
              (use_input, Help{"1: the weapon actions drive it"}), (applied, ReadOnly, Transient), (ammo, ReadOnly, Transient),
              (magazine, ReadOnly, Transient), (reserve, ReadOnly, Transient))

// The weapon `holder` has in hand: its Loadout3D's active slot, else its own Weapon3D. Null when it has none.
Weapon3D* held_weapon(World& world, Entity holder);

// Every weapon `holder` carries (the slots of its Loadout3D, else its own Weapon3D), at most `cap`; returns how many.
// The pointers hold until the world next changes structure.
usize weapons_of(World& world, Entity holder, Weapon3D** out, usize cap);

// Registers the component and adds "kit3d_loadout" (after install_combat3d, which it calls). Idempotent.
void install_loadout3d(World& world);

}  // namespace mce::kit
