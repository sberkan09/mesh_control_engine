#pragma once
// Traffic on a road network (M-OPENWORLD B2, ADR-0046): cars that keep to the lanes of a placed
// .mcroads (kit/roads.h), follow the car ahead, stop at red lights and stop signs, give way at
// junctions, and pick their turns; and a spawner that keeps a number of them alive around a focus.
//
//   TrafficDriver3D   on an entity with a Vehicle3D (and its RigidBody3D): writes the car's throttle,
//                     steer and brake every tick, like VehicleDriver3D
//   TrafficSpawner3D  spawns cars (a prefab, or a game's spawn hook) on lanes around `focus` and
//                     removes the ones that drift out of range
//
// Each tick "traffic_drivers" (FixedUpdate_Pre, ahead of "kit3d_vehicles") does, for every driver:
//   1. Where it is: the car's position is projected onto its lane (and on onto the lanes it chose
//      next once it passes the end); a car more than `lost_distance` off its lanes re-snaps to the
//      nearest lane going its way.
//   2. Where it goes: at the end of a road lane it takes one of the connectors leaving it, chosen
//      by a hash of (seed, junctions passed), so a replay turns the same way; it keeps two lanes chosen
//      ahead so it sees through a junction.
//   3. How fast: the intelligent driver model (IDM) against the nearest thing ahead in its lanes:
//      the car ahead (its rear, by `length`), the stop line of a connector it may not enter yet, or
//      a bend's speed (the connector's cooked limit, braked to at `decel`).
//      It may not enter a connector when its light is red, or amber while it can still stop at
//      `decel`; at a Stop connector until it has stood at the line; or while a conflicting connector
//      is occupied, or a car will reach a conflicting connector of a higher rank (or of the same
//      rank, sooner) within `yield_time` seconds of when it would itself.
//   4. Steering: pure pursuit at a point `look_ahead` metres (at least 0.6 s) down its lanes, as
//      VehicleDriver3D steers, no sharper than its speed allows.
// Everything a driver knows is in its component, so traffic replays, hashes and saves.

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
}

namespace mce::kit {

enum class TrafficState : u8 {
    Idle,      // no network or no lane yet
    Driving,
    Following, // held back by the car ahead
    Waiting,   // at a red or amber light
    Yielding,  // giving way at a junction (or standing at a stop line)
    Lost,      // off the road: re-snapping
};
MCE_ENUM(TrafficState, Idle, Driving, Following, Waiting, Yielding, Lost)

struct TrafficDriver3D {
    Entity network;              // the RoadNetwork3D it drives on; null = the first one
    f32 speed_factor = 1.0f;     // x the lanes' limits (a driver's temper)
    f32 max_speed = 0.0f;        // m/s cap; 0 = the lanes' limits alone
    f32 accel = 2.0f;            // m/s^2 IDM comfortable acceleration
    f32 decel = 3.0f;            // m/s^2 IDM comfortable braking
    f32 headway = 1.4f;          // s IDM time gap to the car ahead
    f32 min_gap = 2.5f;          // m IDM gap when stopped
    f32 length = 4.5f;           // m of its own car (how far behind it the next car stops)
    f32 look_ahead = 6.0f;       // m pure pursuit
    f32 yield_time = 3.0f;       // s a junction must be clear of higher-priority arrivals
    f32 lost_distance = 6.0f;    // m off its lane before it re-snaps
    u32 seed = 0;                // its turns: hash(seed, turns)
    u8 drive = 1;                // 0: lets go of the car
    TrafficState state = TrafficState::Idle;  // engine-written
    u16 turns = 0;               // engine-written: junctions it chose a way through
    u32 lane = 0xFFFFFFFFu;      // engine-written (a spawner sets it): the lane it is on
    u32 next = 0xFFFFFFFFu;      // engine-written: the lane after it
    u32 after = 0xFFFFFFFFu;     // engine-written: and the one after that
    u32 home = 0xFFFFFFFFu;      // engine-written: the lane it left to go round a wreck (it returns there; none = not dodging)
    f32 s = 0.0f;                // engine-written: metres along `lane`
    f32 target_speed = 0.0f;     // engine-written: m/s it wants now
    f32 stopped_for = 0.0f;      // engine-written: s it has stood at a stop line
    f32 stuck_for = 0.0f;        // engine-written: s it has been lost or flipped (its spawner removes it)
    Entity leader;               // engine-written: the car it follows (null = none in range)
    Entity spawner;              // the TrafficSpawner3D that made it (null = placed by hand)
};
MCE_COMPONENT(TrafficDriver3D, (network, Help{"the RoadNetwork3D it drives on; null = the first"}),
              (speed_factor, Range{0.1f, 3}, Help{"x the lanes' speed limits"}), (max_speed, Range{0, 100}, Help{"m/s cap; 0 = the limits alone"}),
              (accel, Range{0.1f, 20}, Help{"m/s^2"}), (decel, Range{0.1f, 20}, Help{"m/s^2 comfortable braking"}),
              (headway, Range{0, 10}, Help{"seconds behind the car ahead"}), (min_gap, Range{0, 20}, Help{"m to the car ahead when stopped"}),
              (length, Range{0.5f, 30}, Help{"m of its own car"}), (look_ahead, Range{1, 50}, Help{"m to the steering aim point"}),
              (yield_time, Range{0, 20}, Help{"s a junction must be clear of cars that go first"}),
              (lost_distance, Range{0.5f, 100}, Help{"m off its lane before it re-snaps"}), (seed, Help{"picks its turns"}),
              (drive, Help{"0: lets go of the car"}), (state, ReadOnly, Save), (turns, ReadOnly, Save), (lane, ReadOnly, Save),
              (next, ReadOnly, Save), (after, ReadOnly, Save), (home, ReadOnly, Save), (s, ReadOnly, Save), (target_speed, ReadOnly, Transient),
              (stopped_for, ReadOnly, Save), (stuck_for, ReadOnly, Save), (leader, ReadOnly, Transient),
              (spawner, Help{"the TrafficSpawner3D that made it"}))

// Keeps up to `max_cars` traffic cars alive between `min_radius` and `spawn_radius` of `focus` (the
// player; null = the network's origin), on road lanes, at least `spacing` metres from any other car
// on that lane; removes its cars beyond `despawn_radius` (and ones that flipped or got lost for
// `stuck_after` seconds). At most `per_tick` cars spawn a tick. A car is `prefab` (its root needs
// Vehicle3D; it gets a TrafficDriver3D seeded from `seed`), or, with no prefab, whatever the game's
// TrafficSpawnHook resource spawns.
struct TrafficSpawner3D {
    Entity network;              // null = the first RoadNetwork3D
    Entity focus;                // null = the network's origin
    asset::AssetRef<asset::Prefab> prefab;
    u32 max_cars = 24;
    f32 min_radius = 40.0f;      // m: no car appears closer to the focus
    f32 spawn_radius = 160.0f;   // m
    f32 despawn_radius = 220.0f; // m
    f32 spacing = 15.0f;         // m to the nearest car on the lane
    f32 stuck_after = 20.0f;     // s lost or flipped before it is removed (0 = never)
    f32 speed_spread = 0.15f;    // speed_factor is 1 +- this, per car
    u32 seed = 1;
    u16 per_tick = 2;
    u8 active = 1;
    u8 pad_ = 0;
    u32 alive = 0;               // engine-written: its cars now
    u32 spawned = 0;             // engine-written: ever
    u32 removed = 0;             // engine-written: ever
};
MCE_COMPONENT(TrafficSpawner3D, (network, Help{"null = the first RoadNetwork3D"}), (focus, Help{"cars live around it (the player); null = the network's origin"}),
              (prefab, Help{"the car; its root needs Vehicle3D. None: the game's TrafficSpawnHook"}), (max_cars, Range{0, 1024}),
              (min_radius, Range{0, 10000}, Help{"m: no car appears closer"}), (spawn_radius, Range{0, 10000}, Help{"m"}),
              (despawn_radius, Range{0, 10000}, Help{"m: its cars beyond go"}), (spacing, Range{0, 500}, Help{"m to the nearest car"}),
              (stuck_after, Range{0, 600}, Help{"s lost or flipped before removal; 0 = never"}), (speed_spread, Range{0, 1}),
              seed, (per_tick, Range{0, 64}), (active, Help{"0: spawns nothing (still removes)"}), (alive, ReadOnly, Transient),
              (spawned, ReadOnly, Save), (removed, ReadOnly, Save))

// The game's own car spawner (TrafficSpawner3D without a prefab): spawn a car at `position` facing
// `rotation` through `commands` and return its root (it must end up with Vehicle3D and RigidBody3D);
// the spawner adds the TrafficDriver3D. Return a null entity to skip.
struct TrafficSpawnHook {
    Entity (*spawn)(Frame& f, math::vec3 position, math::quat rotation, u32 index) = nullptr;
};

// Registers TrafficDriver3D and TrafficSpawner3D and adds "traffic_spawners" and "traffic_drivers"
// (FixedUpdate_Pre, after "kit3d_bodies"; installs roads and vehicles after them). Idempotent.
void install_traffic(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
