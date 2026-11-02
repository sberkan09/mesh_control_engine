#pragma once
// Progression and goals (M-PROGRESS). kit::install_stock installs it.
//
//   Progress3D  on the player: counters the engine fills in from this tick's events.
//   Goal3D      on its own entity: "reach `target` of `stat`", paid once to `owner`'s Wallet3D.
//
// Counters: `kills` (a Died3D whose killer is the entity and whose victim is another entity), `missions` (a MissionCompleted
// of a Mission whose `actor` is the entity), `rides` (a VehicleEntered3D of the entity), `melee_hits` (the hits of its
// MeleeSwung3D). A goal whose counter has reached `target` is marked `done`, pays `reward` through give_cash (when the owner has a
// Wallet3D), raises Progress3D.goals_done, shows the caption "Goal: <name>" when captions are installed and pushes GoalDone3D.
// A goal with a null or gone `owner` stays as it is. Goals are looked at in entity order. System: "kit_progress"
// (FixedUpdate_Pre, after "kit_missions" and "kit_melee").

#include "mce/fixed_string.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

enum class GoalStat : u8 { Kills, Missions, Rides, MeleeHits };
MCE_ENUM(GoalStat, Kills, Missions, Rides, MeleeHits)

struct Progress3D {
    u32 kills = 0;
    u32 missions = 0;
    u32 rides = 0;
    u32 melee_hits = 0;
    u32 goals_done = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Progress3D, (kills, Save, ReadOnly), (missions, Save, ReadOnly), (rides, Save, ReadOnly),
              (melee_hits, Save, ReadOnly), (goals_done, Save, ReadOnly))

struct Goal3D {
    FixedString<32> name;               // for the caption and the HUD
    GoalStat stat = GoalStat::Kills;
    u8 done = 0;                        // engine-written
    u16 pad_ = 0;
    u32 target = 1;
    u32 reward = 0;                     // cash
    Entity owner;                       // the entity with the Progress3D (and the Wallet3D)
};
MCE_COMPONENT(Goal3D, (name, Save), (stat, Save), (done, Save, ReadOnly), (target, Save, Range{1, 1000000}),
              (reward, Save, Range{0, 1000000}), (owner, Help{"the entity with the Progress3D"}))

// Pushed when a goal is reached.
struct GoalDone3D {
    Entity goal;
    Entity owner;
    u32 reward = 0;
    u32 pad_ = 0;
};
MCE_EVENT(GoalDone3D, goal, owner, reward)

// Registers the components and event and adds the system (installs missions, seats, melee and wanted3d first). Idempotent.
void install_progress3d(World& world);

}  // namespace mce::kit
