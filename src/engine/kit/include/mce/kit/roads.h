#pragma once
// Road networks at run time (M-OPENWORLD B1, ADR-0046). A .mcroads asset (asset::RoadView) holds the
// lanes, the connectors through each junction, who yields and the traffic-light phases; a
// RoadNetwork3D component places one at its entity and runs its lights. Traffic (kit/traffic.h) and
// crowds drive on it through the queries below.
//
//   kit::install_roads(world, &assets);          // install_stock does it
//   ... an entity with LocalTransform and RoadNetwork3D{roads = guid of downtown.mcroads}
//   kit::RoadNet net;
//   if (kit::road_net(world, network_entity, net)) {
//       kit::RoadSpot at;
//       if (kit::road_nearest(net, car_position, at)) ... at.lane, at.s
//   }
//
// The network lies in its entity's space, translated (rotation and scale are ignored), so moving
// the entity moves every lane. Positions in and out of the queries are world space.
//
// Lights: "roads_clock" (FixedUpdate_Pre, first) advances RoadNetwork3D::time by the tick while
// `lights` is set. A signal's state is a pure function of that time (asset::RoadSignal), and the
// time is saved and hashed with the component, so lights replay and resume exactly.

#include "mce/asset/handle.h"
#include "mce/asset/roads_format.h"
#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct RoadNetwork3D {
    asset::AssetRef<asset::RoadNetwork> roads;
    f32 time = 0.0f;  // s the lights have run (engine-advanced while `lights` is set)
    u8 lights = 1;    // 0: the lights hold where they are
    u8 pad_[3] = {};
};
MCE_COMPONENT(RoadNetwork3D, (roads, Help{"the .mcroads road network, placed at this entity (translation only)"}),
              (time, Save, Help{"seconds the traffic lights have run"}), (lights, Help{"0: the traffic lights hold"}))

enum class RoadLight : u8 { Green, Amber, Red };
MCE_ENUM(RoadLight, Green, Amber, Red)

class RoadRegistry {
public:
    RoadRegistry() = default;
    explicit RoadRegistry(asset::AssetSystem* assets) : assets_(assets) {}
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }

    // A network copied out of its chunk, with a grid of its lane segments for nearest-lane queries.
    struct Def {
        asset::Guid guid;
        asset::AssetKey key = asset::kInvalidAssetKey;
        u32 generation = 0;
        u32 hash = 0;  // of the cooked payload
        FixedString<32> name;
        u32 flags = 0;  // asset::RoadFlags
        Array<asset::RoadNode> nodes;
        Array<asset::RoadRoad> roads;
        Array<asset::RoadLane> lanes;
        Array<asset::RoadPoint> points;
        Array<u32> conflicts;
        Array<asset::RoadSignal> signals;
        Array<asset::RoadPhase> phases;
        // Segment grid: cell (x, z) of `cell` metres from (grid_x, grid_z) holds segments
        // cell_items[cell_start[i] .. cell_start[i + 1]), each a lane's first point index of a segment.
        f32 cell = 32.0f;
        f32 grid_x = 0.0f, grid_z = 0.0f;
        u32 grid_w = 0, grid_h = 0;
        Array<u32> cell_start;
        Array<u32> cell_items;
        Array<u32> point_lane;  // the lane a point belongs to (kRoadNone: a road's centreline)

        // The road named `road_name` (its source name) or kRoadNone; the node likewise.
        u32 find_road(StringView road_name) const;
        u32 find_node(StringView node_name) const;
    };

    // The network a component places: its cooked asset (copied out once, re-read on hot reload) or one
    // registered with define. Null when it does not resolve (warned once).
    const Def* resolve(const RoadNetwork3D& n);
    // Registers (or replaces) the network for `guid` from a CHNK_ROADS payload, ahead of the asset
    // system: tools preview unsaved edits through it, tests skip the cook.
    Result<void> define(asset::Guid guid, Span<const byte> payload);

private:
    asset::AssetSystem* assets_ = nullptr;
    Array<Def> defs_;
    Array<asset::Guid> warned_;
};

// A placed network: its data, where it sits and its lights' clock.
struct RoadNet {
    const RoadRegistry::Def* def = nullptr;
    Entity entity;
    math::vec3 origin;  // world position of the network's (0, 0, 0)
    f32 time = 0.0f;
};
// False when `network` has no RoadNetwork3D or its asset does not resolve.
bool road_net(World& world, Entity network, RoadNet& out);
// The first live RoadNetwork3D in ECS order that resolves (the usual one-network game).
bool road_net_first(World& world, RoadNet& out);

// A place on a lane.
struct RoadSpot {
    u32 lane = asset::kRoadNone;
    f32 s = 0.0f;         // metres along the lane
    f32 distance = 0.0f;  // metres from the query point to the lane (flat)
    math::vec3 position;  // the closest point on the lane, world space
    math::vec3 direction; // the lane's direction there (flat, unit)
};

struct RoadNearestQuery {
    f32 max_distance = 50.0f;  // metres
    bool connectors = false;   // also consider lanes through junctions
    // When not zero, only lanes running within 90 degrees of this direction (flat), so a car on the
    // right-hand lane does not snap onto the oncoming one.
    math::vec3 heading;
};
// The lane closest to `position` (flat distance) within the query's reach; false when none.
bool road_nearest(const RoadNet& net, math::vec3 position, RoadSpot& out, const RoadNearestQuery& q = {});

// The world point `s` metres along `lane` (clamped to the lane) and the lane's flat direction there.
math::vec3 road_lane_point(const RoadNet& net, u32 lane, f32 s, math::vec3* direction = nullptr);

// A connector's light at the network's time: Green for a connector of an unsignalled junction or a
// road lane. `seconds_left` (when given) gets how long it stays this colour (a large number when it
// never changes): red counts down to the start of its next green.
RoadLight road_light(const RoadNet& net, u32 lane, f32* seconds_left = nullptr);
RoadLight road_light(const RoadRegistry::Def& def, f32 time, u32 lane, f32* seconds_left = nullptr);

// The quickest lane route (by length / speed) from `from_lane` to `to_lane`, both included, in driving
// order (road lanes and connectors alternate). False when `to_lane` cannot be reached.
bool road_route(const RoadRegistry::Def& def, u32 from_lane, u32 to_lane, Array<u32>& out);

// Debug lines (the editor's road overlay): every lane's polyline, road lanes grey, connectors by their
// light (green, amber, red; white where nothing signals), offset by the network's position.
struct RoadLine {
    math::vec3 a;
    math::vec3 b;
    math::Color color;
};
void road_debug_lines(World& world, Array<RoadLine>& out);

// Registers RoadNetwork3D, sets the RoadRegistry resource and adds "roads_clock" (FixedUpdate_Pre).
// Idempotent (re-points the assets).
void install_roads(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
