#pragma once
// Melee attack (M-MELEE). kit::install_stock installs it.
//
//   Melee3D   on the attacker (a character or the player): a swing hits every other Health3D in front of it. It does not
//             depend on Weapon3D, so it works with any wielded gun and costs no ammo.
//
// A swing starts when `trigger` is set to 1 (the engine clears it), or, with `use_input`, when the action "Melee" is pressed;
// only when `cooldown` is 0, the attacker is alive and not a seated Rider3D. After `windup` s it hits every entity with a
// Health3D (other than the attacker, and not on its non-zero team) whose distance is at most `reach` and whose bearing from the
// attacker's forward (-Z of its own pose) on the ground plane is within `arc_deg` / 2, in entity order, through apply_damage
// (so Damage3D, Died3D, crimes and cash drops work as for a bullet). `cooldown` then lasts 1 / `rate` s from the press.
// MeleeSwung3D reports each resolved swing. System: "kit_melee" (FixedUpdate_Pre, after "kit_loadout").

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct Melee3D {
    f32 damage = 35.0f;
    f32 reach = 2.2f;          // m
    f32 arc_deg = 100.0f;      // the cone in front that a swing hits
    f32 rate = 2.5f;           // swings/s
    f32 windup = 0.15f;        // s between the press and the hit
    f32 cooldown = 0.0f;       // engine-written, s until the next swing may start
    f32 timer = 0.0f;          // engine-written, s until the swing in progress hits
    u8 use_input = 0;          // 1: the action "Melee" starts a swing
    u8 trigger = 0;            // set to 1 to start a swing (the engine clears it)
    u8 swinging = 0;           // engine-written
    u8 pad0_ = 0;
    u32 swings = 0;            // engine-written
    u32 pad_ = 0;
};
MCE_COMPONENT(Melee3D, (damage, Save, Range{0, 100000}), (reach, Save, Range{0, 100}), (arc_deg, Save, Range{1, 360}),
              (rate, Save, Range{0.01f, 100}), (windup, Save, Range{0, 10}), (cooldown, Save, ReadOnly),
              (timer, Save, ReadOnly), (use_input, Help{"1: the action Melee starts a swing"}),
              (trigger, Help{"set to 1 to start a swing (the engine clears it)"}), (swinging, Save, ReadOnly),
              (swings, ReadOnly, Transient))

// Pushed when a swing hits (or misses: hits = 0).
struct MeleeSwung3D {
    Entity entity;   // the attacker
    u32 hits = 0;
    u32 pad_ = 0;
};
MCE_EVENT(MeleeSwung3D, entity, hits)

// Registers the component and event and adds the system. Idempotent.
void install_melee3d(World& world);

}  // namespace mce::kit
