#pragma once
// Missions and objectives as data (M-MISSION step 1): a chain of goals watched by one system, with the
// state in components so a save mid-mission resumes. kit::install_stock installs it. Scenes without
// these components are unchanged.
//
//   Mission    on an entity: the chain's first objective, who plays it, its state, an optional time
//              limit and an optional entity whose death fails it
//   Objective  on its own entity: what to do (kind), to what (target entity or tag), how many, and
//              the objective that follows (`next`; none: the mission is complete when this one is done)
//
// Lifecycle. A Mission is idle until `start` is set to 1 (the engine clears it) or, with `start_on_load`,
// on the first tick. It then goes active, its first objective goes active, and MissionStarted is sent.
// Exactly one objective of a mission is active at a time (`Mission.current`). When it is done
// (ObjectiveDone) the next one goes active; when the last is done the mission is complete
// (MissionCompleted). An active mission fails (MissionFailed) when `time_limit` seconds of ticks have
// passed, or `fail_on_death` died. Complete and failed missions are left alone.
//
// Objective kinds, evaluated once per tick for the active objective (an objective that has just gone
// active is first looked at on the next tick, so no event from before it counts):
//   reach_zone  Mission.actor is within `radius` of the target
//   kill        `count` Died3D events: of the target, or of anything when there is no target or tag
//   collect     `count` TriggerEnter3D events of the target trigger (Mission.actor, when set, must be
//               the one entering): pickups as Trigger3D volumes
//   interact    the Interact action is pressed while Mission.actor is within `radius` of the target
//   signal      `count` MissionSignal events with this objective's `signal` name: the game, a script
//               or another system pushes them
//   timer       `duration` seconds after it went active
//
// The target is the `target` entity, or, when that is null, the entity named `tag` in the loaded scene
// (resolved when the objective goes active, and retried each tick while nothing matches).
//
// Time is counted in ticks (`ticks`, saved); `elapsed` is ticks * dt, for display. The same inputs
// give the same state hash. Missions and objectives are saved with the entity's SaveKey. The entity
// references they hold are saved as handles, so they resolve in the same base scene a load rebuilds
// (the scene's own entities, spawned in the same order).
//
// Events (producer FixedUpdate_Pre): MissionStarted, ObjectiveDone, MissionCompleted, MissionFailed,
// and the input MissionSignal.
//
// System: "kit_missions" (FixedUpdate_Pre, after "kit3d_health").

#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/string_id.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

enum class MissionState : u8 { Idle, Active, Complete, Failed };
MCE_ENUM(MissionState, Idle, Active, Complete, Failed)

enum class ObjectiveKind : u8 { ReachZone, Kill, Collect, Interact, Signal, Timer };
MCE_ENUM(ObjectiveKind, ReachZone, Kill, Collect, Interact, Signal, Timer)

enum class ObjectiveState : u8 { Pending, Active, Done };
MCE_ENUM(ObjectiveState, Pending, Active, Done)

struct Mission {
    FixedString<32> id;             // the mission's name, for the HUD and scripts
    Entity objective;               // the first objective; the rest follow by Objective.next
    Entity actor;                   // who plays it (reach_zone, interact, collect); the player's character
    Entity fail_on_death;           // optional: the mission fails when this entity dies or is gone
    MissionState state = MissionState::Idle;
    u8 start = 0;                   // set to 1 to start (the engine clears it)
    u8 start_on_load = 0;           // 1: starts on the first tick
    u8 pad_ = 0;
    f32 time_limit = 0.0f;          // s since it started; 0: none
    f32 elapsed = 0.0f;             // engine-written: s since it started
    u32 ticks = 0;                  // engine-written: ticks since it started
    u32 done = 0;                   // engine-written: objectives done
    Entity current;                 // engine-written: the active objective
};
MCE_COMPONENT(Mission, (id, Save, Help{"the mission's name"}), (objective, Save, Help{"the first objective entity; the others follow by Objective.next"}),
              (actor, Save, Help{"who plays it: the player's character (reach_zone, interact, collect)"}),
              (fail_on_death, Save, Help{"optional: the mission fails when this entity dies or is destroyed"}),
              (state, Save, ReadOnly), (start, Help{"set to 1 to start the mission (the engine clears it)"}),
              (start_on_load, Save, Help{"1: starts on the first tick"}),
              (time_limit, Save, Range{0, 100000}, Help{"seconds since it started before it fails; 0: none"}),
              (elapsed, ReadOnly, Transient), (ticks, Save, ReadOnly), (done, Save, ReadOnly), (current, Save, ReadOnly))

struct Objective {
    ObjectiveKind kind = ObjectiveKind::ReachZone;
    ObjectiveState state = ObjectiveState::Pending;  // engine-written
    u16 pad_ = 0;
    Entity target;                  // what to reach, kill, collect or use; or null and `tag`
    FixedString<32> tag;            // the entity name to look for when `target` is null
    FixedString<32> signal;         // signal: the MissionSignal name
    u32 count = 1;                  // kill, collect, signal: how many
    u32 progress = 0;               // engine-written
    f32 radius = 3.0f;              // reach_zone, interact: metres
    f32 duration = 5.0f;            // timer: seconds
    u32 ticks = 0;                  // engine-written: ticks since it went active
    Entity next;                    // the objective that follows; null: the mission ends with this one
};
MCE_COMPONENT(Objective, (kind, Save), (state, Save, ReadOnly), (target, Save, Help{"what to reach, kill, collect or use"}),
              (tag, Save, Help{"the entity name to look for when target is empty"}),
              (signal, Save, Help{"signal: the MissionSignal name that counts"}), (count, Save, Range{1, 100000}, Help{"kill, collect, signal: how many"}),
              (progress, Save, ReadOnly), (radius, Save, Range{0.1f, 10000}, Help{"reach_zone, interact: metres"}),
              (duration, Save, Range{0, 100000}, Help{"timer: seconds"}), (ticks, Save, ReadOnly),
              (next, Save, Help{"the objective that follows; empty: the mission ends with this one"}))

// Events (producer FixedUpdate_Pre).
struct MissionStarted {
    Entity mission;
};
struct ObjectiveDone {
    Entity mission;
    Entity objective;
    u32 index = 0;                  // 0 for the first
};
struct MissionCompleted {
    Entity mission;
};
struct MissionFailed {
    Entity mission;
    u32 reason = 0;                 // 0: time limit, 1: fail_on_death
};
// The input: a named signal for the active `signal` objectives (the game, Luau or a system pushes it).
struct MissionSignal {
    FixedString<32> name;
    u32 amount = 1;
};
MCE_EVENT(MissionStarted, mission)
MCE_EVENT(ObjectiveDone, mission, objective, index)
MCE_EVENT(MissionCompleted, mission)
MCE_EVENT(MissionFailed, mission, reason)
MCE_EVENT(MissionSignal, name, amount)

// Registers the components and events and adds the system (after install_combat3d, which it calls).
// Idempotent.
void install_missions(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
