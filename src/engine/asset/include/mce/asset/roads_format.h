#pragma once
// CHNK_ROADS: a cooked road network (M-OPENWORLD B1, ADR-0046). The .mcroads importer
// (cook/src/roads_importer.cpp) writes it from nodes, roads and signals; kit::RoadRegistry
// (kit/roads.h) copies it out at run time. Kept out of formats.h so the many TUs that include that
// header do not rebuild for the road kit.
//
// Payload layout, every block 16-byte aligned:
//   RoadHeader | RoadNode[node_count] | RoadRoad[road_count] | RoadLane[lane_count]
//   | RoadPoint[point_count] | u32 conflict[conflict_count] | RoadSignal[signal_count] | RoadPhase[phase_count]
//
// Everything a car drives on is a lane, a polyline of RoadPoints with the distance along it:
//   - a road lane runs along one road in one direction, trimmed back from the junctions at both
//     ends; its successors (next) are the connectors leaving its end, a contiguous run of lanes;
//   - a connector crosses one junction from the end of a road lane (prev) to the start of another
//     (its one next). Its conflicts are the other connectors of the junction that cross or merge
//     with it; one that shares its source lane (a fork) does not conflict.
// Who goes first is cooked into each connector: `rank` (higher goes first), the Stop and Yield flags
// and, at a signalled junction, `green`, a bit per phase of the junction's signal in which it may go.
// Positions are metres in the network's own space (kit places it at its entity), Y up.

#include "mce/containers/array.h"
#include "mce/result.h"

namespace mce::asset {

inline constexpr u8 ROAD_NETWORK_VERSION = 1;  // CHNK_ROADS: lanes, junction connectors, signals (M-OPENWORLD B1)

inline constexpr u32 kRoadNone = 0xFFFFFFFFu;
inline constexpr u16 kRoadNone16 = 0xFFFFu;
inline constexpr u32 kRoadMaxLanesPerSide = 8;  // lanes one way along a road
inline constexpr u32 kRoadMaxRoadsPerNode = 16;
inline constexpr u32 kRoadMaxPhases = 16;       // per signal
inline constexpr u32 kRoadMaxNodes = 65535;
inline constexpr u32 kRoadMaxItems = 1u << 24;  // roads, lanes, points, conflicts, signals, phases

namespace RoadFlags {
enum : u32 {
    LeftHand = 1u << 0,  // traffic keeps left (the default keeps right)
};
}

enum class RoadControl : u8 {
    None,      // a bend or a dead end: nobody yields, merging lanes take turns
    Priority,  // the roads with the highest priority go; the others yield
    Stop,      // every approach stops, then yields
    Signal,    // a RoadSignal decides; across turns yield to oncoming traffic on a shared green
};

enum class RoadLaneKind : u8 { Road, Connector };
enum class RoadTurn : u8 { Straight, Left, Right, UTurn };

namespace RoadLaneFlags {
enum : u8 {
    Backward = 1u << 0,  // road lane: runs from the road's `to` node to its `from` node
    Yield = 1u << 1,     // connector: gives way to conflicting connectors of a higher rank
    Stop = 1u << 2,      // connector: stops at the line first
};
}

struct RoadHeader {
    char name[32];
    u32 flags;  // RoadFlags
    u32 node_count;
    u32 road_count;
    u32 lane_count;
    u32 point_count;
    u32 conflict_count;
    u32 signal_count;
    u32 phase_count;
    f32 bounds_min[3];  // of every point and node
    f32 bounds_max[3];
    u32 reserved[2];
};
static_assert(sizeof(RoadHeader) == 96);

struct RoadNode {
    f32 position[3];
    RoadControl control;
    u8 road_count;
    u16 signal;           // index into the signals, kRoadNone16 when not signalled
    u32 first_connector;  // its connectors are lanes [first_connector, first_connector + connector_count)
    u32 connector_count;
    u64 name;  // StringId of the source name
};
static_assert(sizeof(RoadNode) == 32);

struct RoadRoad {
    u64 name;  // StringId of the source name
    u16 from;  // node indices
    u16 to;
    u8 forward;   // lanes from `from` to `to`
    u8 backward;  // lanes from `to` to `from`
    u8 priority;
    u8 reserved0;
    f32 lane_width;
    f32 speed;     // m/s limit
    f32 sidewalk;  // metres of sidewalk beside each kerb; 0 = none
    f32 length;    // of the untrimmed centreline
    u32 first_point;  // the untrimmed centreline, `from` to `to`
    u32 point_count;
    u32 first_lane;  // forward lanes (innermost first), then backward lanes: forward + backward of them
    u32 reserved1;
};
static_assert(sizeof(RoadRoad) == 48);

struct RoadLane {
    u32 first_point;  // the polyline; RoadPoint::s runs from 0 to length
    u32 point_count;
    f32 length;
    f32 speed;  // m/s: the road's limit; a connector's is also capped by its tightest bend
    f32 width;
    u32 first_next;  // successors: lanes [first_next, first_next + next_count)
    u32 next_count;
    u32 prev;  // connector: the road lane it leaves; road lane: kRoadNone
    u32 first_conflict;  // connector: indices into the conflict table
    u32 conflict_count;
    u32 green;  // connector at a signalled node: bit p set = it may go in phase p
    u16 road;   // road lane: its road; connector: kRoadNone16
    u16 node;   // road lane: the node it runs to; connector: the junction it crosses
    RoadLaneKind kind;
    u8 index;  // road lane: 0 = innermost (beside the centreline or the other direction); connector: a RoadTurn
    u8 flags;  // RoadLaneFlags
    u8 rank;   // connector: higher goes first
    u32 reserved[3];
};
static_assert(sizeof(RoadLane) == 64);

struct RoadPoint {
    f32 x, y, z;
    f32 s;  // metres along the lane (or the road's centreline) from its first point
};
static_assert(sizeof(RoadPoint) == 16);

// A junction's traffic light. A cycle runs each phase's green for its seconds, then amber, then
// all_red before the next phase: cycle = sum(seconds + amber + all_red). At time t the cycle is at
// (t + offset) mod cycle.
struct RoadSignal {
    u16 node;
    u16 phase_count;
    u32 first_phase;
    f32 amber;
    f32 all_red;
    f32 offset;
    f32 cycle;
    u32 reserved[2];
};
static_assert(sizeof(RoadSignal) == 32);

struct RoadPhase {
    f32 seconds;  // of green
    f32 start;    // s into the cycle the green starts
};
static_assert(sizeof(RoadPhase) == 8);

struct RoadView {
    const RoadHeader* header = nullptr;
    Span<const RoadNode> nodes;
    Span<const RoadRoad> roads;
    Span<const RoadLane> lanes;
    Span<const RoadPoint> points;
    Span<const u32> conflicts;
    Span<const RoadSignal> signals;
    Span<const RoadPhase> phases;
    StringView name() const;
    // Checks every index and range against the tables, the lane graph's shape (a road lane's
    // successors are connectors whose prev is that lane; a connector has one road-lane successor),
    // and that every number is finite and every lane's s rises from 0 to its length.
    static Result<RoadView> read(Span<const byte> payload);
};

struct RoadTables {
    RoadHeader header{};  // name, flags and bounds; the counts are taken from the spans
    Span<const RoadNode> nodes;
    Span<const RoadRoad> roads;
    Span<const RoadLane> lanes;
    Span<const RoadPoint> points;
    Span<const u32> conflicts;
    Span<const RoadSignal> signals;
    Span<const RoadPhase> phases;
};
void write_road_network(const RoadTables& t, Array<byte>& out);

}  // namespace mce::asset
