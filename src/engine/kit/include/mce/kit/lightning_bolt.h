#pragma once
// A place for every lightning strike (M-LIGHTNINGBOLT 11a). One optional LightningBolt per scene (the first in ECS
// order drives). install_weather registers the component; the system "kit_lightning" (precipitation.h) reads it.
//
//   LightningBolt  authoring: `min_range_m` (150) and `max_range_m` (900) bound the strike distance on the ground
//                  plane, Range 10..5000, swapped when inverted, NaN falls back to the defaults. `ground_y` (0) the
//                  height of the strike point, `height_m` (600, Range 50..3000) the top of the bolt above it, `width_m`
//                  (1.5, Range 0.05..20 at 100 m), `segments_log2` (5, Range 2..7), `jitter` (0.25, Range 0..0.6),
//                  `fork` (1), `light_intensity` (0 = no point light), `light_radius_m` (400), `enabled` (1). The
//                  geometry fields are read by the drawing step (11b); this step only uses the ranges, `ground_y` and
//                  `enabled`. Engine-written and saved: `strike_x`, `strike_z` (the last strike's ground position),
//                  `strike_dist_m` (its 3D distance from the camera), `strike_id` (the Lightning.counter value the
//                  strike was hashed with, the first strike is 0) and `active` (flash ticks left, Lightning.remaining
//                  while a flash lasts), then an explicit pad.
//
// Position. When kit_lightning raises a strike and an enabled LightningBolt exists, the strike gets a bearing in
// [0, 2 pi) from hash_mix(Lightning.seed, counter * 4 + 2) and a range uniform in [min_range, max_range] from
// hash_mix(seed, counter * 4 + 3). Those are a separate stream from the strike time (counter * 2) and the delay
// (counter * 2 + 1), so the times of an existing scene do not move. The origin is the position of the primary
// Camera3DComponent (the first camera when none is primary) at the strike tick, (0, 0, 0) with no camera. The strike
// point is (origin.x + range sin(bearing), ground_y, origin.z + range cos(bearing)); it stays fixed afterwards. All
// trigonometry is in f64, the results are stored as f32. The LightningStruck event then carries has_pos 1, the point
// and distance_m (3D, camera to point), and thunder_delay_ticks = max(1, ceil(distance_m / 343 / dt)) (not clamped to
// 30..300). Without a LightningBolt, or with `enabled` 0, the event and every byte of the world are as before.
//
// `active` follows Lightning.remaining each tick while a bolt exists. A scene without a LightningBolt is unchanged.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct LightningBolt {
    f32 min_range_m = 150.0f;
    f32 max_range_m = 900.0f;
    f32 ground_y = 0.0f;
    f32 height_m = 600.0f;
    f32 width_m = 1.5f;
    u32 segments_log2 = 5;
    f32 jitter = 0.25f;
    u8 fork = 1;
    u8 enabled = 1;
    u8 pad_[2] = {};            // explicit pad (state_hash hashes whole columns); reserved
    f32 light_intensity = 0.0f;
    f32 light_radius_m = 400.0f;
    f32 strike_x = 0.0f;        // engine-written: the last strike's ground position
    f32 strike_z = 0.0f;
    f32 strike_dist_m = 0.0f;   // engine-written: camera to strike point
    u32 strike_id = 0;          // engine-written: Lightning.counter of that strike
    u32 active = 0;             // engine-written: flash ticks left
};
MCE_COMPONENT(LightningBolt, (min_range_m, Save, Range{10, 5000}, Help{"nearest strike distance from the camera, m"}),
              (max_range_m, Save, Range{10, 5000}, Help{"farthest strike distance from the camera, m"}),
              (ground_y, Save, Help{"height of the strike point"}),
              (height_m, Save, Range{50, 3000}, Help{"top of the bolt above the strike point, m"}),
              (width_m, Save, Range{0.05f, 20}, Help{"bolt width at 100 m"}),
              (segments_log2, Save, Range{2, 7}, Help{"the bolt has 2^n segments"}),
              (jitter, Save, Range{0, 0.6f}, Help{"midpoint displacement as a fraction of segment length"}),
              (fork, Save, Help{"1: one side fork"}), (enabled, Save, Help{"0: strikes get no position"}),
              (light_intensity, Save, Range{0, 100000}, Help{"point light at the strike; 0 = none"}),
              (light_radius_m, Save, Range{1, 5000}), (strike_x, Save, ReadOnly), (strike_z, Save, ReadOnly),
              (strike_dist_m, Save, ReadOnly), (strike_id, Save, ReadOnly), (active, Save, ReadOnly))

// A strike's place: the ground point and the 3D distance from `origin` (the camera).
struct BoltStrike {
    f32 x = 0.0f;
    f32 z = 0.0f;
    f32 distance_m = 0.0f;
};

// The pure position of strike `counter` of a Lightning with `seed`, seen from `origin` (see above).
BoltStrike bolt_strike_position(const LightningBolt& b, u32 seed, u32 counter, const math::vec3& origin);

// thunder_delay_ticks for a positioned strike: max(1, ceil(distance_m / 343 / dt)), capped at 2^31.
u32 bolt_delay_ticks(f32 distance_m, f64 dt);

}  // namespace mce::kit
