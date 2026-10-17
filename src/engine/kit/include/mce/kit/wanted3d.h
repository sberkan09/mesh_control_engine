#pragma once
// Wanted level and police (M-OPENWORLD C3): crimes raise heat on an offender, police units see it, chase it,
// shoot it and give up when they lose it. kit::install_stock installs it.
//
//   Wanted3D   on the offender (the player): heat, the level made from it (0..max_level), and how crimes count
//   Police3D   on a police unit: when it responds, what it sees, and a small state machine
//              (Idle, Pursue, Attack, Search)
//
// Crimes. `Damage3D` and `Died3D` (combat3d.h) turn into heat when the instigator (or the entity it hangs
// under) has a Wanted3D and the victim's Health3D `team` is `civilian_team` or `police_team` (hits and kills
// each have a heat value). A hit on a police unit is always known; a civilian hit counts only when a living
// Police3D within `witness_range` of the victim has a clear line to it (`require_witness` 0: always). Scripts
// and other systems call `report_crime`. Each counted crime pushes `Crime3D`.
//
// Level. `level` is heat / `heat_per_level` rounded up, at most `max_level`. A change pushes
// `WantedChanged3D`. Heat falls by `decay` per second once no police unit has seen the offender for
// `cooldown_delay` seconds (a new crime restarts that wait); at level 0 every unit goes back to Idle.
//
// Police3D. A unit whose `min_level` is above the offender's level ignores it. Otherwise it looks for the
// nearest offender within `sight_range` with a clear line (eye `eye_height` above the entity): seen, it is
// Pursue (further than `attack_range`, or below `attack_level`) or Attack; lost, it is Search for
// `search_time` seconds at the place it last saw the offender, then Idle. In Attack a unit with a Weapon3D
// on its own entity turns to face the target (`aim`) and sets `fire`. With `drive_nav` a NavAgent3D follows
// the target (Pursue), walks to where it was last seen (Search) and stands still (Attack, Idle). A unit with
// an AiBrain also gets the blackboard keys "wanted" (number), "police_state" (number), "target" (entity,
// cleared when none) and "last_known" (vec3), so a behavior tree can take over the movement.
//
// "kit3d_wanted" runs in FixedUpdate_Pre after "kit3d_health". Everything is in the components, so it is
// deterministic, hashed and saved.

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct Wanted3D {
    f32 heat = 0.0f;
    f32 heat_per_level = 20.0f;
    f32 hit_civilian = 5.0f;        // heat per hit on a civilian_team victim
    f32 kill_civilian = 15.0f;
    f32 hit_police = 15.0f;         // heat per hit on a police_team victim
    f32 kill_police = 30.0f;
    f32 witness_range = 40.0f;      // m: police farther from a civilian victim saw nothing
    f32 cooldown_delay = 10.0f;     // s unseen before heat decays
    f32 decay = 2.0f;               // heat/s
    f32 unseen_time = 0.0f;         // engine-written: s since a unit saw it (or a crime)
    math::vec3 last_known;          // engine-written: where a unit last saw it
    u32 seen_by = 0;                // engine-written: units that see it now
    u32 crimes = 0;                 // engine-written: crimes counted
    u8 max_level = 5;
    u8 level = 0;                   // engine-written
    u8 police_team = 2;             // Health3D::team of police
    u8 civilian_team = 3;           // Health3D::team of civilians
    u8 require_witness = 1;
    u8 pad_[3] = {};
};
MCE_COMPONENT(Wanted3D, (heat, Save, Range{0, 1000}), (heat_per_level, Range{1, 1000}), (hit_civilian, Range{0, 1000}),
              (kill_civilian, Range{0, 1000}), (hit_police, Range{0, 1000}), (kill_police, Range{0, 1000}),
              (witness_range, Range{0, 1000}, Help{"m"}), (cooldown_delay, Range{0, 3600}, Help{"s unseen before heat decays"}),
              (decay, Range{0, 1000}, Help{"heat/s"}), (unseen_time, Save, ReadOnly), (last_known, Save, ReadOnly),
              (seen_by, ReadOnly, Transient), (crimes, Save, ReadOnly), (max_level, Range{1, 10}), (level, ReadOnly, Transient),
              (police_team, Help{"Health3D::team of police units"}), (civilian_team, Help{"Health3D::team of civilians"}),
              (require_witness, Help{"1: a civilian hit counts only when a police unit sees the victim"}))

enum class PoliceState : u8 { Idle, Pursue, Attack, Search };
MCE_ENUM(PoliceState, Idle, Pursue, Attack, Search)

struct Police3D {
    u8 min_level = 1;               // responds from this wanted level
    u8 attack_level = 2;            // shoots from this wanted level (below it, it only follows)
    PoliceState state = PoliceState::Idle;  // engine-written
    u8 drive_nav = 1;               // 1: drives the entity's NavAgent3D
    u8 aim = 1;                     // 1: Attack turns the entity to the target and sets its Weapon3D's `fire`
    u8 pad_[3] = {};
    f32 sight_range = 30.0f;        // m
    f32 attack_range = 20.0f;       // m
    f32 search_time = 8.0f;         // s at the last known place before giving up
    f32 eye_height = 1.6f;          // m
    f32 state_time = 0.0f;          // engine-written: s in the state
    Entity target;                  // engine-written: the offender it follows
    math::vec3 last_known;          // engine-written
};
MCE_COMPONENT(Police3D, (min_level, Range{1, 10}, Help{"responds from this wanted level"}),
              (attack_level, Range{1, 10}, Help{"shoots from this wanted level"}), (state, ReadOnly, Transient),
              (drive_nav, Help{"1: drives the NavAgent3D"}), (aim, Help{"1: Attack faces the target and fires the Weapon3D"}),
              (sight_range, Range{0, 1000}, Help{"m"}), (attack_range, Range{0, 1000}, Help{"m"}),
              (search_time, Range{0, 600}, Help{"s at the last known place"}), (eye_height, Range{0, 10}),
              (state_time, ReadOnly, Transient), (target, Save, ReadOnly), (last_known, Save, ReadOnly))

// Events (producer FixedUpdate_Pre).
struct Crime3D {
    Entity offender;
    Entity victim;
    f32 heat = 0.0f;                // heat added
    math::vec3 position;
};
struct WantedChanged3D {
    Entity offender;
    u32 from = 0;
    u32 to = 0;
};
MCE_EVENT(Crime3D, offender, victim, heat, position)
MCE_EVENT(WantedChanged3D, offender, from, to)

// Adds `heat` to `offender`'s Wanted3D as a crime at `position` (`victim` may be null) and pushes Crime3D. The
// caller decides whether anyone saw it: a crime with `witnessed` false counts nothing. False when it did not
// count (no Wanted3D, no heat, not witnessed). From a FixedUpdate_Pre system or between ticks; the level is
// recomputed at once and WantedChanged3D pushed.
bool report_crime(World& world, Entity offender, f32 heat, math::vec3 position, Entity victim = {}, bool witnessed = true);

// Clears the heat and level of `offender` (a pay-and-spray, a mission end). Units go Idle on the next tick.
void clear_wanted(World& world, Entity offender);

// Registers the components and events and adds "kit3d_wanted" (after install_combat3d, which it calls). Idempotent.
void install_wanted3d(World& world);

}  // namespace mce::kit
