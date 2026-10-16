#pragma once
// AI perception (M-AI 3): what an AI sees and hears, written into its behavior tree's blackboard.
//
//   Perceiver  an AI's senses: a sight cone (range, angle, an eye offset), hearing, a memory that
//              forgets a lost target after `forget` seconds, and the blackboard keys it writes
//   Stimulus   something a perceiver can see: a team, how far it shows (`visibility`), the point
//              looked at (`offset`)
//   Noise      an event anyone may push (a footstep, a thrown stone, a shot): heard by perceivers
//              within `loudness` metres times their `hearing`. The stock task "make_noise" pushes
//              one from the tree's entity
//
// "ai_perception" (FixedUpdate_Pre, just before "ai_brains", so a tree reads this tick's senses)
// checks each perceiver's sight every `interval` ticks (staggered by entity index, so a crowd
// spreads its rays over the ticks): every Stimulus of another team inside the cone and range is
// tested for line of sight with a ray from the eye (World2D for `space` Plane2D, Physics3D's
// World3D for Space3D; without a physics world nothing blocks), and the nearest one seen becomes
// the target. Each tick a perceiver that sees nothing counts up `since_seen`; at `forget` seconds
// the target is dropped. Noises from the last tick within reach set `heard_at`.
//
// The blackboard keys (on the entity's AiBrain tree; a key the tree does not declare, or an empty
// name, is skipped): `target_key` (entity) holds the target while it is remembered, `sees_key`
// (bool) whether it is in sight now, `last_seen_key` (vec3) where it was last seen, `heard_key`
// (vec3) the last noise heard (left set: the tree clears it once it has investigated).
//
// Everything is in the components and the tick, so perception is deterministic, hashed and saved.

#include "mce/containers/array.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace mce {
class World;
class Frame;
}  // namespace mce

namespace mce::kit {

enum class PerceptionSpace : u8 { Space3D, Plane2D };
MCE_ENUM(PerceptionSpace, Space3D, Plane2D)

struct Perceiver {
    f32 sight_range = 12.0f;                 // metres
    f32 sight_angle = 120.0f;                // degrees: the whole cone (360: all around)
    math::vec3 eye{0.0f, 1.6f, 0.0f};        // entity space; forward is -Z (3D) or +X (2D)
    f32 hearing = 1.0f;                      // times a noise's loudness (0: deaf)
    f32 forget = 5.0f;                       // seconds a lost target is remembered
    u32 team = 0;                            // stimuli and noises of the same team are ignored (0: none are)
    u16 mask = 0xFFFF;                       // physics layers that block sight
    PerceptionSpace space = PerceptionSpace::Space3D;
    u8 interval = 1;                         // ticks between sight checks (1: every tick)
    FixedString<24> target_key{"target"};
    FixedString<24> sees_key{"sees"};
    FixedString<24> last_seen_key{"last_seen"};
    FixedString<24> heard_key{"heard_at"};
    Entity target;                           // engine-written: what it sees or remembers
    math::vec3 last_seen;                    // engine-written
    math::vec3 heard_at;                     // engine-written
    f32 since_seen = 0.0f;                   // engine-written: seconds since the target was in sight
    f32 awareness = 0.0f;                    // engine-written: 1 in sight, falling to 0 at `forget`
    u8 sees = 0;                             // engine-written
    u8 heard = 0;                            // engine-written: heard a noise this tick
    u8 pad_[2] = {};
    u32 sightings = 0;                       // engine-written: times a target came into sight
    u32 hearings = 0;                        // engine-written: noises heard
};
MCE_COMPONENT(Perceiver, (sight_range, Range{0, 1000}, Help{"metres"}), (sight_angle, Range{0, 360}, Help{"degrees, the whole cone"}),
              (eye, Help{"entity space; forward is -Z (3D) or +X (2D)"}), (hearing, Range{0, 100}, Help{"times a noise's loudness; 0 = deaf"}),
              (forget, Range{0, 3600}, Help{"seconds a lost target is remembered"}),
              (team, Help{"stimuli and noises of this team are ignored; 0 = none are"}), (mask, Help{"physics layers that block sight"}),
              space, (interval, Range{1, 60}, Help{"ticks between sight checks"}),
              (target_key, Help{"entity key: the target"}), (sees_key, Help{"bool key: in sight now"}),
              (last_seen_key, Help{"vec3 key: where it was last seen"}), (heard_key, Help{"vec3 key: the last noise heard"}),
              (target, Save, ReadOnly), (last_seen, Save, ReadOnly), (heard_at, Save, ReadOnly), (since_seen, Save, ReadOnly),
              (awareness, Save, ReadOnly), (sees, Save, ReadOnly), (heard, ReadOnly, Transient), (sightings, Save, ReadOnly),
              (hearings, Save, ReadOnly))

struct Stimulus {
    u32 team = 1;
    f32 visibility = 1.0f;               // times a perceiver's sight range (0: unseen)
    math::vec3 offset{0.0f, 1.0f, 0.0f};  // the point looked at, entity space
    u8 visible = 1;
    u8 pad_[3] = {};
};
MCE_COMPONENT(Stimulus, (team, Help{"perceivers of this team ignore it"}), (visibility, Range{0, 10}, Help{"times the sight range"}),
              (offset, Help{"the point looked at, entity space"}), visible)

// Push from FixedUpdate_Pre (or the tree's "make_noise"); heard on the next tick.
struct Noise {
    math::vec3 position;
    f32 loudness = 10.0f;  // metres a perceiver with hearing 1 hears it from
    Entity source;
    u32 team = 0;          // perceivers of this team ignore it (0: none do)
    u32 pad_ = 0;
};

// Per-tick counters (a resource): sight checks and rays cast this tick, noises heard.
struct PerceptionStats {
    u32 checks = 0;
    u32 rays = 0;
    u32 heard = 0;
};

// Registers Perceiver, Stimulus and Noise, sets PerceptionStats and adds "ai_perception" to
// FixedUpdate_Pre. install_ai calls it (before "ai_brains"). Idempotent.
void install_perception(World& world);

// The system itself (exposed for tests and custom pipelines).
void ai_perception(Frame& f);

// Debug lines (the editor's perception overlay): each perceiver's cone edges and range arc, and a
// line to the target it sees (green) or to where it last saw it (yellow).
struct PerceptionLine {
    math::vec3 a;
    math::vec3 b;
    math::Color color;
};
void perception_debug_lines(World& world, Array<PerceptionLine>& out);

}  // namespace mce::kit
