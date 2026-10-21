#pragma once
// Timed status effects on Health3D (M-STATUS 1). A StatusEffects component holds up to kStatusSlots effects; each has an id
// (what a game names it by), a kind, an amount and a time left. Everything is whole microseconds and fixed-step arithmetic, so
// the same inputs give the same state hash.
//
//   Regen      heals `amount` hp per second while it lasts (capped at Health3D.max; a dead entity is not healed)
//   Poison     `amount` hp per second, taken in pulses every half second through apply_damage (so armor, Died3D, ragdolls and
//              LootDrop all work as for any damage); the instigator is the entity that applied it
//   Armor      adds `amount` to Health3D.armor (kept within 0..1) while it lasts
//   MaxHealth  adds `amount` to Health3D.max (never below 1; current is clamped down when max falls) while it lasts
//
// The two bonus kinds are applied as a remembered change (`applied`), like Equipment's item bonuses: the authored base is never
// overwritten and the end of the effect gives back exactly what it added. `applied` is not saved: Health3D is not in a save game
// either, so the bonus applies again on the first tick after a load.
//
// An id is hashed to 32 bits for the slot. Applying an id that is already active refreshes it (kind, amount and time are
// replaced; nothing stacks). When all slots are busy a new id is refused and StatusRefused is sent.
//
// Input events: ApplyStatus, ClearStatus (an empty id clears everything; clears are handled before applies in a tick, so a
// cleanse followed by a re-apply in the same tick ends with the effect on). Regen and Poison need a positive amount; amounts
// are limited to +-1e6.
// A Health3D replaced at run time (a respawn that overwrites the component) loses the bonus kinds until they end, and the
// end then subtracts what the old one had: clear the effects when respawning. Output events: StatusApplied, StatusEnded
// (`expired` 1: ran out, 0: cleared), StatusRefused. System "kit_status" (FixedUpdate_Pre, after "kit_craft").

#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

inline constexpr u32 kStatusSlots = 8;
inline constexpr u32 kStatusPulseUs = 500000;  // poison damage comes in half-second pulses
inline constexpr f32 kStatusMaxSeconds = 3600.0f;

enum class StatusKind : u8 { Regen, Poison, Armor, MaxHealth, Cooldown };  // Cooldown changes nothing itself: `status.has` and item use read it
MCE_ENUM(StatusKind, Regen, Poison, Armor, MaxHealth, Cooldown)

struct StatusEffects {
    u32 key[kStatusSlots] = {};        // hash of the effect id, 0: empty
    FixedString<32> id[kStatusSlots];  // the id itself, for the events
    u8 kind[kStatusSlots] = {};        // StatusKind
    f32 amount[kStatusSlots] = {};
    u32 left_us[kStatusSlots] = {};    // microseconds until it ends
    u32 pulse_us[kStatusSlots] = {};   // Poison: microseconds into the current pulse
    f32 applied[kStatusSlots] = {};    // Armor / MaxHealth: what has been added so far (engine-written, not saved)
    Entity source[kStatusSlots];       // who applied each, credited for poison damage (engine-written, not saved)
};
MCE_COMPONENT(StatusEffects, (key, Save, ReadOnly), (id, Save, ReadOnly), (kind, Save, ReadOnly), (amount, Save, ReadOnly), (left_us, Save, ReadOnly),
              (pulse_us, Save, ReadOnly), (applied, ReadOnly), (source, ReadOnly, Transient))

// The 32-bit slot key of an effect id (never 0).
u32 status_key(StringView id);
// Slot of an active effect, or -1.
i32 status_find(const StatusEffects& s, u32 key);

// ---- input events
struct ApplyStatus {
    Entity target;
    FixedString<32> id;
    u32 kind = 0;           // StatusKind
    f32 amount = 0.0f;      // hp/s for Regen and Poison (positive), the added value for Armor and MaxHealth (+-1e6)
    f32 seconds = 1.0f;     // 0.01 to 3600
    Entity source;          // credited for Poison damage
};
struct ClearStatus {
    Entity target;
    FixedString<32> id;     // empty: every effect
};
MCE_EVENT(ApplyStatus, target, id, kind, amount, seconds, source)
MCE_EVENT(ClearStatus, target, id)

// ---- output events
struct StatusApplied {
    Entity target;
    FixedString<32> id;
    u32 kind = 0;
    u32 refreshed = 0;      // 1: the id was already active
};
struct StatusEnded {
    Entity target;
    FixedString<32> id;
    u32 expired = 0;        // 1: ran out, 0: cleared
};
struct StatusRefused {
    Entity target;
    FixedString<32> id;
};
MCE_EVENT(StatusApplied, target, id, kind, refreshed)
MCE_EVENT(StatusEnded, target, id, expired)
MCE_EVENT(StatusRefused, target, id)

// Registers the component and events and adds the system (after install_crafting, which it calls). Idempotent.
void install_status(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
