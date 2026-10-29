#pragma once
// Police roadblocks that appear with the wanted level (M-OPENWORLD-POLICE 3). A `Roadblock3D` marker entity sits where the
// block goes: its local X runs across the road (the line of cars), its local +Z is the side the officers stand on (behind the cars,
// seen from the oncoming offender). When an offender's wanted level reaches `min_level` and the offender is between `spawn_min`
// and `spawn_max` metres away, the marker spawns `cars` cars (the `car` prefab, a static body that blocks the road) side by side
// `car_spacing` apart and `officers` officers (the `officer` prefab: Police3D, Health3D, a weapon) `officer_back` behind the line,
// each tagged with a `RoadblockPiece3D` naming the marker. When no offender is at `min_level` any more for `hold` seconds, and
// the nearest offender is at least `spawn_min` away (nothing pops out of existence in sight), the pieces are destroyed (their
// children with them). Without a prefab the game's `RoadblockSpawnHook` makes the pieces (kind 0: car, 1: officer).
//
// "kit3d_roadblocks" (FixedUpdate_Pre, after "kit3d_wanted"). The marker's state (`up`, `low_time`) and the pieces are saved.

#include "mce/asset/handle.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
class Frame;
}  // namespace mce

namespace mce::asset {
class AssetSystem;
struct Prefab;
}  // namespace mce::asset

namespace mce::kit {

struct Roadblock3D {
    asset::AssetRef<asset::Prefab> car;      // one barricade car: its root should be a static body
    asset::AssetRef<asset::Prefab> officer;  // one officer: its root should carry Police3D
    u8 min_level = 3;                        // up while an offender is at this wanted level or above
    u8 cars = 2;
    u8 officers = 2;
    u8 up = 0;                               // engine-written: 1 while its pieces stand
    f32 car_spacing = 3.6f;                  // m between cars (and between officers) across the road
    f32 officer_back = 3.0f;                 // m behind the line of cars (local +Z)
    f32 spawn_min = 60.0f;                   // m: it does not appear (or vanish) closer to the offender than this
    f32 spawn_max = 300.0f;                  // m: nor farther
    f32 hold = 10.0f;                        // s below min_level before it is taken down
    f32 low_time = 0.0f;                     // engine-written: s the wanted level has been below min_level
    u32 pad_ = 0;
};
MCE_COMPONENT(Roadblock3D, (car, Help{"a barricade car; its root should be a static body. None: the game's RoadblockSpawnHook (kind 0)"}),
              (officer, Help{"an officer; its root should carry Police3D. None: the hook (kind 1)"}),
              (min_level, Range{1, 10}, Help{"the block stands while an offender is at this wanted level or above"}),
              (cars, Range{0, 8}), (officers, Range{0, 8}), (up, ReadOnly, Save),
              (car_spacing, Range{0.5f, 20}, Help{"m between cars across the road (marker local X)"}),
              (officer_back, Range{0, 50}, Help{"m behind the line of cars (marker local +Z)"}),
              (spawn_min, Range{0, 2000}, Help{"m: it does not appear or vanish closer to the offender"}), (spawn_max, Range{0, 5000}, Help{"m"}),
              (hold, Range{0, 600}, Help{"s below min_level before it goes"}), (low_time, ReadOnly, Save))

// On every entity a Roadblock3D spawned.
struct RoadblockPiece3D {
    Entity owner;
};
MCE_COMPONENT(RoadblockPiece3D, (owner, Help{"the Roadblock3D that spawned it"}))

// The game's own pieces (Roadblock3D without prefabs): spawn kind 0 (a car) or 1 (an officer) at `position` facing `rotation` and
// return its root; the system adds the RoadblockPiece3D. Return a null entity to skip.
struct RoadblockSpawnHook {
    Entity (*spawn)(Frame& f, u32 kind, math::vec3 position, math::quat rotation) = nullptr;
};

// Registers Roadblock3D and RoadblockPiece3D and adds "kit3d_roadblocks". `assets` loads the prefabs. Idempotent. install_stock does it.
void install_roadblocks(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
