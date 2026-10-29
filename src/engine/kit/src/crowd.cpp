#include "mce/kit/crowd.h"

#include <cmath>

#include "mce/asset/asset_system.h"
#include "mce/ecs/prefab.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/hash.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/roads.h"
#include "mce/kit/vehicles3d.h"
#include "mce/log.h"

namespace mce::kit {

using asset::kRoadNone;
using asset::RoadLane;
using asset::RoadLaneKind;
using asset::RoadPoint;
using asset::RoadRoad;
using math::vec3;

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(Pedestrian3D) == 44);
static_assert(sizeof(CrowdSpawner3D) == 88);

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;
constexpr f32 kCell = 32.0f;       // metres: the spawn candidate list is a function of the focus's cell
constexpr f32 kMaxHop = 80.0f;     // metres: no hop across a bigger junction
constexpr f32 kTurnRate = 8.0f;    // radians a second a pedestrian turns at
constexpr f32 kReverseWeight = 0.15f;
constexpr f32 kPi = 3.14159265358979f;

// ---- the pavement graph ---------------------------------------------------------------------------------------

struct Edge {
    u32 road = 0;
    u8 side = 0;       // 0: right of the road's from->to direction, 1: left
    u16 node0 = 0;     // the node at point 0
    u16 node1 = 0;
    u32 first_point = 0;
    u32 point_count = 0;
    f32 length = 0.0f;
    f32 lo_x = 0, lo_z = 0, hi_x = 0, hi_z = 0;
};

struct Hop {
    u32 to = 0;     // edge end (edge * 2 + (0: its first point, 1: its last))
    f32 length = 0.0f;
    u16 crossed[3] = {};
    u8 n_crossed = 0;
};

// A road's carriageway as it leaves a junction: a strip along `dir` from the node.
struct Strip {
    u32 road = 0;
    f32 dx = 0, dz = 0;
    f32 left = 0, right = 0;  // metres of carriageway to each side of `dir`
};

struct Graph {
    asset::Guid guid;
    u32 hash = 0;
    u32 generation = 0;
    Array<Edge> edges;
    Array<RoadPoint> pts;
    Array<u32> hop_start;  // per edge end
    Array<Hop> hops;
    Array<u32> strip_start;  // per node
    Array<Strip> strips;
};

f32 flat_len(f32 x, f32 z) { return std::sqrt(x * x + z * z); }

vec3 pt(const RoadPoint& p) { return {p.x, p.y, p.z}; }

// The carriageway's metres to the right and left of the road's from->to direction.
void road_extents(const RoadRoad& r, bool left_hand, f32& right, f32& left) {
    const f32 w = r.lane_width;
    if (r.forward == 0 || r.backward == 0) {
        right = left = 0.5f * static_cast<f32>(r.forward + r.backward) * w;
    } else if (!left_hand) {
        right = static_cast<f32>(r.forward) * w;
        left = static_cast<f32>(r.backward) * w;
    } else {
        left = static_cast<f32>(r.forward) * w;
        right = static_cast<f32>(r.backward) * w;
    }
}

// `line` moved `offset` metres to its right (flat), mitred at the bends.
void offset_line(const Array<vec3>& line, f32 offset, Array<vec3>& out) {
    out.clear();
    const u32 n = static_cast<u32>(line.size());
    const auto right_dir = [](vec3 a, vec3 b, vec3& r) {
        const f32 l = flat_len(b.x - a.x, b.z - a.z);
        if (l < 1e-6f) return false;
        r = vec3{-(b.z - a.z) / l, 0.0f, (b.x - a.x) / l};
        return true;
    };
    for (u32 i = 0; i < n; ++i) {
        vec3 rin{1, 0, 0}, rout{1, 0, 0};
        const bool has_in = i > 0 && right_dir(line[i - 1], line[i], rin);
        const bool has_out = i + 1 < n && right_dir(line[i], line[i + 1], rout);
        if (!has_in) rin = rout;
        if (!has_out) rout = rin;
        vec3 r{rin.x + rout.x, 0.0f, rin.z + rout.z};
        const f32 rl = flat_len(r.x, r.z);
        if (rl < 1e-4f) {
            r = rout;
        } else {
            r = vec3{r.x / rl, 0.0f, r.z / rl};
        }
        const f32 c = r.x * rout.x + r.z * rout.z;
        const f32 scale = c > 0.5f ? 1.0f / c : 2.0f;
        out.push_back(line[i] + r * (offset * scale));
    }
}

// The part of `line` from arclength `a` to `b`, with its arclength from 0 appended to `pts`; returns the length.
f32 append_clipped(const Array<vec3>& line, f32 a, f32 b, Array<RoadPoint>& pts) {
    Array<f32> cum;
    cum.push_back(0.0f);
    for (u32 i = 1; i < line.size(); ++i) cum.push_back(cum[i - 1] + flat_len(line[i].x - line[i - 1].x, line[i].z - line[i - 1].z));
    const auto at_s = [&](f32 s) {
        u32 k = 0;
        while (k + 2 < line.size() && cum[k + 1] < s) ++k;
        const f32 span = cum[k + 1] - cum[k];
        const f32 t = span > 1e-6f ? math::clamp((s - cum[k]) / span, 0.0f, 1.0f) : 0.0f;
        return line[k] + (line[k + 1] - line[k]) * t;
    };
    f32 s = 0.0f;
    const auto push = [&](vec3 p) { pts.push_back(RoadPoint{p.x, p.y, p.z, s}); };
    push(at_s(a));
    for (u32 i = 1; i + 1 < line.size(); ++i) {
        if (cum[i] <= a + 1e-3f || cum[i] >= b - 1e-3f) continue;
        const vec3 p = line[i];
        const RoadPoint& last = pts.back();
        s += flat_len(p.x - last.x, p.z - last.z);
        push(p);
    }
    const vec3 e = at_s(b);
    const RoadPoint& last = pts.back();
    s += flat_len(e.x - last.x, e.z - last.z);
    push(e);
    return s;
}

void build_graph(const RoadRegistry::Def& d, Graph& g) {
    g.edges.clear();
    g.pts.clear();
    g.hop_start.clear();
    g.hops.clear();
    g.strip_start.clear();
    g.strips.clear();
    const bool left_hand = (d.flags & asset::RoadFlags::LeftHand) != 0;
    const u32 node_count = static_cast<u32>(d.nodes.size());

    // Carriageway strips per node.
    Array<u32> strip_count;
    strip_count.resize(node_count + 1, 0u);
    const auto road_dir = [&](const RoadRoad& r, bool at_from, f32& dx, f32& dz) {
        const RoadPoint& p0 = d.points[at_from ? r.first_point : r.first_point + r.point_count - 1];
        const RoadPoint& p1 = d.points[at_from ? r.first_point + 1 : r.first_point + r.point_count - 2];
        const f32 l = flat_len(p1.x - p0.x, p1.z - p0.z);
        dx = l > 1e-6f ? (p1.x - p0.x) / l : 1.0f;
        dz = l > 1e-6f ? (p1.z - p0.z) / l : 0.0f;
    };
    for (const RoadRoad& r : d.roads) {
        ++strip_count[r.from];
        ++strip_count[r.to];
    }
    g.strip_start.resize(node_count + 1, 0u);
    for (u32 n = 0; n < node_count; ++n) g.strip_start[n + 1] = g.strip_start[n] + strip_count[n];
    g.strips.resize(g.strip_start[node_count]);
    Array<u32> fill;
    fill.resize(node_count, 0u);
    for (u32 ri = 0; ri < d.roads.size(); ++ri) {
        const RoadRoad& r = d.roads[ri];
        if (r.point_count < 2) continue;
        f32 right = 0, left = 0;
        road_extents(r, left_hand, right, left);
        for (u32 end = 0; end < 2; ++end) {
            const u32 n = end == 0 ? r.from : r.to;
            Strip& s = g.strips[g.strip_start[n] + fill[n]++];
            s.road = ri;
            road_dir(r, end == 0, s.dx, s.dz);
            s.right = end == 0 ? right : left;  // seen from the far node the sides swap
            s.left = end == 0 ? left : right;
        }
    }

    // Edges: one beside each kerb of a road with a sidewalk.
    for (u32 ri = 0; ri < d.roads.size(); ++ri) {
        const RoadRoad& r = d.roads[ri];
        if (r.sidewalk <= 0.0f || r.point_count < 2 || r.forward + r.backward == 0) continue;
        f32 right = 0, left = 0;
        road_extents(r, left_hand, right, left);
        // How far the junctions took off the lanes: a lane's end projected onto the road's direction.
        const RoadLane& lane = d.lanes[r.first_lane];
        const bool backward = (lane.flags & asset::RoadLaneFlags::Backward) != 0;
        const RoadPoint& lp0 = d.points[lane.first_point];
        const RoadPoint& lp1 = d.points[lane.first_point + lane.point_count - 1];
        const RoadPoint& at_from = backward ? lp1 : lp0;
        const RoadPoint& at_to = backward ? lp0 : lp1;
        f32 fdx = 0, fdz = 0, tdx = 0, tdz = 0;
        road_dir(r, true, fdx, fdz);
        road_dir(r, false, tdx, tdz);
        const RoadPoint& nf = d.points[r.first_point];
        const RoadPoint& nt = d.points[r.first_point + r.point_count - 1];
        f32 trim0 = (at_from.x - nf.x) * fdx + (at_from.z - nf.z) * fdz;
        f32 trim1 = (at_to.x - nt.x) * tdx + (at_to.z - nt.z) * tdz;
        trim0 = math::clamp(trim0, 0.0f, r.length * 0.5f);
        trim1 = math::clamp(trim1, 0.0f, r.length * 0.5f);
        Array<vec3> centre;
        for (u32 k = 0; k < r.point_count; ++k) centre.push_back(pt(d.points[r.first_point + k]));
        for (u32 side = 0; side < 2; ++side) {
            const f32 lateral = (side == 0 ? right : left) + 0.5f * r.sidewalk;
            Array<vec3> line;
            offset_line(centre, side == 0 ? lateral : -lateral, line);
            f32 total = 0.0f;
            for (u32 i = 1; i < line.size(); ++i) total += flat_len(line[i].x - line[i - 1].x, line[i].z - line[i - 1].z);
            const f32 a = trim0, b = total - trim1;
            if (b - a < 1.0f) continue;
            Edge e;
            e.road = ri;
            e.side = static_cast<u8>(side);
            e.node0 = r.from;
            e.node1 = r.to;
            e.first_point = static_cast<u32>(g.pts.size());
            e.length = append_clipped(line, a, b, g.pts);
            e.point_count = static_cast<u32>(g.pts.size()) - e.first_point;
            e.lo_x = e.lo_z = 1e30f;
            e.hi_x = e.hi_z = -1e30f;
            for (u32 k = 0; k < e.point_count; ++k) {
                const RoadPoint& p = g.pts[e.first_point + k];
                e.lo_x = math::min(e.lo_x, p.x);
                e.lo_z = math::min(e.lo_z, p.z);
                e.hi_x = math::max(e.hi_x, p.x);
                e.hi_z = math::max(e.hi_z, p.z);
            }
            g.edges.push_back(e);
        }
    }

    // Hops: from each edge end to every end at the same node (itself: turn round).
    const u32 ends = static_cast<u32>(g.edges.size()) * 2;
    const auto end_pos = [&](u32 id) {
        const Edge& e = g.edges[id >> 1];
        return pt(g.pts[e.first_point + ((id & 1) ? e.point_count - 1 : 0)]);
    };
    const auto end_node = [&](u32 id) { return (id & 1) ? g.edges[id >> 1].node1 : g.edges[id >> 1].node0; };
    g.hop_start.resize(ends + 1, 0u);
    for (u32 a = 0; a < ends; ++a) {
        g.hop_start[a] = static_cast<u32>(g.hops.size());
        const u32 node = end_node(a);
        const vec3 pa = end_pos(a);
        for (u32 b = 0; b < ends; ++b) {
            if (end_node(b) != node) continue;
            Hop h;
            h.to = b;
            if (a != b) {
                const vec3 pb = end_pos(b);
                h.length = flat_len(pb.x - pa.x, pb.z - pa.z);
                if (h.length > kMaxHop) continue;
                const vec3 centre = pt(RoadPoint{d.nodes[node].position[0], d.nodes[node].position[1], d.nodes[node].position[2], 0.0f});
                for (u32 k = 1; k < 10 && h.n_crossed < 3; ++k) {
                    const f32 t = static_cast<f32>(k) / 10.0f;
                    const f32 rx = pa.x + (pb.x - pa.x) * t - centre.x, rz = pa.z + (pb.z - pa.z) * t - centre.z;
                    for (u32 si = g.strip_start[node]; si < g.strip_start[node + 1] && h.n_crossed < 3; ++si) {
                        const Strip& s = g.strips[si];
                        const f32 along = rx * s.dx + rz * s.dz;
                        const f32 lat = rx * -s.dz + rz * s.dx;  // to the right of dir
                        if (along < 0.5f || lat > s.right || lat < -s.left) continue;
                        bool seen = false;
                        for (u32 c = 0; c < h.n_crossed; ++c) seen |= h.crossed[c] == s.road;
                        if (!seen) h.crossed[h.n_crossed++] = static_cast<u16>(s.road);
                    }
                }
            }
            g.hops.push_back(h);
        }
    }
    g.hop_start[ends] = static_cast<u32>(g.hops.size());
}

// ---- resources ------------------------------------------------------------------------------------------------

struct Candidates {
    u32 crowd = 0;
    bool valid = false;
    bool focused = false;
    i32 cx = 0, cz = 0;
    f32 radius = 0.0f;
    u32 graph = kNone;  // generation + hash it was built for
    Array<u32> edges;
};

// A pedestrian scared by a shot (M-PEDFLEE 2): where the shot came from and how long it keeps running.
struct Fear {
    Entity ped;
    vec3 threat;  // world space
    vec3 pos;     // where the pedestrian was when last seen (world space): its neighbours catch the panic from here
    f32 left = 0.0f;
};
constexpr usize kMaxFears = 512;
constexpr f32 kCrashSpeed = 5.0f;     // m/s a vehicle must close at for pedestrians to hear it as a crash (M-PEDCRASH)
constexpr f32 kRunDownSpeed = 6.0f;   // m/s a vehicle must go for pedestrians in its path to jump clear of it (M-PEDCRASH 2)
constexpr f32 kRunDownTime = 2.0f;    // s ahead of it that counts as its path
constexpr f32 kRunDownWidth = 2.5f;   // m either side of its line
constexpr f32 kRunDownFlee = 3.0f;    // s a pedestrian runs from a vehicle that nearly hit it
constexpr usize kMaxCars = 64;
constexpr usize kMaxSources = 32;     // scare sources (shots and crashes) read in one tick

struct CrowdState {
    Array<Fear> fears;
    asset::AssetSystem* assets = nullptr;
    Array<Graph> graphs;
    Array<Candidates> candidates;
    CrowdStats stats;
};

const Graph* graph_for(CrowdState& st, const RoadNet& net) {
    for (Graph& g : st.graphs)
        if (g.guid == net.def->guid) {
            if (g.hash != net.def->hash || g.generation != net.def->generation) {
                g.hash = net.def->hash;
                g.generation = net.def->generation;
                build_graph(*net.def, g);
            }
            return &g;
        }
    st.graphs.emplace_back();
    Graph& g = st.graphs.back();
    g.guid = net.def->guid;
    g.hash = net.def->hash;
    g.generation = net.def->generation;
    build_graph(*net.def, g);
    return &g;
}

// ---- stepping -------------------------------------------------------------------------------------------------

u32 next_rand(u32& s) {
    if (s == 0) s = 1;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

f32 unit_rand(u32& s) { return static_cast<f32>(next_rand(s) >> 8) * (1.0f / 16777216.0f); }

vec3 end_position(const Graph& g, u32 id) {
    const Edge& e = g.edges[id >> 1];
    return pt(g.pts[e.first_point + ((id & 1) ? e.point_count - 1 : 0)]);
}

u16 end_node_of(const Graph& g, u32 id) { return (id & 1) ? g.edges[id >> 1].node1 : g.edges[id >> 1].node0; }

// The point `s` metres along an edge and the direction of increasing s.
vec3 edge_point(const Graph& g, const Edge& e, f32 s, f32& tx, f32& tz) {
    u32 k = 0;
    while (k + 2 < e.point_count && g.pts[e.first_point + k + 1].s < s) ++k;
    const RoadPoint& a = g.pts[e.first_point + k];
    const RoadPoint& b = g.pts[e.first_point + k + 1];
    const f32 span = b.s - a.s;
    const f32 t = span > 1e-6f ? math::clamp((s - a.s) / span, 0.0f, 1.0f) : 0.0f;
    const f32 l = flat_len(b.x - a.x, b.z - a.z);
    tx = l > 1e-6f ? (b.x - a.x) / l : 0.0f;
    tz = l > 1e-6f ? (b.z - a.z) / l : 0.0f;
    return vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// Whether `road` can be crossed at `node` for `need` seconds: no straight-on traffic entering from it or
// leaving onto it has a green or amber light, or a red that ends before the crossing does.
bool crossing_clear(const RoadRegistry::Def& d, f32 time, u32 node, u32 road, f32 need) {
    const asset::RoadNode& n = d.nodes[node];
    if (n.signal == asset::kRoadNone16) return true;
    for (u32 k = 0; k < n.connector_count; ++k) {
        const u32 lane = n.first_connector + k;
        const RoadLane& l = d.lanes[lane];
        if (l.kind != RoadLaneKind::Connector || l.index != static_cast<u8>(asset::RoadTurn::Straight)) continue;
        if (l.prev == kRoadNone || l.next_count == 0) continue;
        if (d.lanes[l.prev].road != road && d.lanes[l.first_next].road != road) continue;
        f32 left = 0.0f;
        if (road_light(d, time, lane, &left) != RoadLight::Red || left < need) return false;
    }
    return true;
}

struct Crowd {
    const CrowdSpawner3D* params = nullptr;
    const RoadNet* net = nullptr;
    const Graph* graph = nullptr;
    f32 speed_scale = 1.0f;  // M-PEDFLEE: > 1 while fleeing
    bool flee = false;       // does not wait for lights
    bool turn = false;       // on this step, head for the edge end farther from `threat`
    vec3 threat;             // network space
};

// The nearest pavement point to `p` (network space); false when the graph has none.
bool snap_to_pavement(const Graph& g, vec3 p, u32& edge, f32& s) {
    f32 best = 1e30f;
    for (u32 i = 0; i < g.edges.size(); ++i) {
        const Edge& e = g.edges[i];
        for (u32 k = 0; k + 1 < e.point_count; ++k) {
            const RoadPoint& a = g.pts[e.first_point + k];
            const RoadPoint& b = g.pts[e.first_point + k + 1];
            const f32 dx = b.x - a.x, dz = b.z - a.z;
            const f32 l2 = dx * dx + dz * dz;
            const f32 t = l2 > 1e-9f ? math::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / l2, 0.0f, 1.0f) : 0.0f;
            const f32 qx = a.x + dx * t - p.x, qz = a.z + dz * t - p.z;
            const f32 d2 = qx * qx + qz * qz;
            if (d2 < best) {
                best = d2;
                edge = i;
                s = a.s + (b.s - a.s) * t;
            }
        }
    }
    return best < 1e29f;
}

// One step of `dt` seconds.
void step_pedestrian(Pedestrian3D& p, LocalTransform& t, const Crowd& c, f32 dt) {
    const Graph& g = *c.graph;
    const RoadRegistry::Def& d = *c.net->def;
    if (p.speed < 0.05f) p.speed = 0.05f;
    const f32 spd = p.speed * c.speed_scale;
    bool fresh = false;
    if (p.edge >= g.edges.size()) {
        const vec3 local = t.position - c.net->origin;
        if (!snap_to_pavement(g, local, p.edge, p.s)) return;
        p.target = kNone;
        p.mode = PedestrianMode::Walk;
        fresh = true;
    }
    const Edge& e = g.edges[p.edge];
    p.s = math::clamp(p.s, 0.0f, e.length);
    f32 hx = 0.0f, hz = 0.0f;
    vec3 where;

    if (c.turn && p.target == kNone) {  // M-PEDFLEE: run from the threat along this edge
        f32 tx = 0.0f, tz = 0.0f;
        const vec3 a0 = edge_point(g, e, 0.0f, tx, tz), a1 = edge_point(g, e, e.length, tx, tz);
        p.dir = flat_len(a1.x - c.threat.x, a1.z - c.threat.z) >= flat_len(a0.x - c.threat.x, a0.z - c.threat.z) ? 1 : 0;
    }

    if (p.target == kNone) {
        p.pace = spd;
        p.s += (p.dir ? 1.0f : -1.0f) * spd * dt;
        const bool at_end = p.dir ? p.s >= e.length : p.s <= 0.0f;
        if (at_end) {
            p.s = p.dir ? e.length : 0.0f;
            // At a junction: pick where to go next.
            const u32 from = p.edge * 2 + (p.dir ? 1u : 0u);
            f32 total = 0.0f;
            for (u32 h = g.hop_start[from]; h < g.hop_start[from + 1]; ++h)
                total += g.hops[h].to == from ? kReverseWeight : (g.hops[h].n_crossed ? c.params->cross : 1.0f);
            f32 pick = unit_rand(p.rng) * total;
            u32 chosen = from;
            for (u32 h = g.hop_start[from]; h < g.hop_start[from + 1]; ++h) {
                const f32 w = g.hops[h].to == from ? kReverseWeight : (g.hops[h].n_crossed ? c.params->cross : 1.0f);
                chosen = g.hops[h].to;
                if (pick < w) break;
                pick -= w;
            }
            p.target = chosen;
            p.hop = 0.0f;
            p.mode = PedestrianMode::Walk;
        }
    }

    if (p.target != kNone) {
        const u32 from = p.edge * 2 + (p.dir ? 1u : 0u);
        const Hop* hop = nullptr;
        for (u32 h = g.hop_start[from]; h < g.hop_start[from + 1]; ++h)
            if (g.hops[h].to == p.target) hop = &g.hops[h];
        if (!hop) {  // the network was reloaded under it: carry on along the edge
            p.target = kNone;
            p.mode = PedestrianMode::Walk;
            p.dir = static_cast<u8>(!p.dir);
        } else {
            const f32 hop_time = hop->length / spd;
            if (hop->n_crossed && p.hop <= 0.0f && p.mode != PedestrianMode::Cross) {
                bool clear = true;
                const u16 node = end_node_of(g, from);
                for (u32 k = 0; k < hop->n_crossed && clear; ++k) clear = c.flee || crossing_clear(d, c.net->time, node, hop->crossed[k], hop_time + 1.0f);
                p.mode = clear ? PedestrianMode::Cross : PedestrianMode::Wait;
            }
            if (p.mode == PedestrianMode::Wait) {
                p.pace = 0.0f;
            } else {
                p.pace = spd;
                p.hop += hop->length > 0.01f ? spd * dt / hop->length : 1.0f;
            }
            const vec3 a = end_position(g, from), b = end_position(g, p.target);
            if (p.hop >= 1.0f) {  // arrived: walk the new edge away from this end
                p.edge = p.target >> 1;
                p.dir = (p.target & 1) ? 0 : 1;
                p.s = (p.target & 1) ? g.edges[p.edge].length : 0.0f;
                p.target = kNone;
                p.hop = 0.0f;
                p.mode = PedestrianMode::Walk;
            } else {
                where = vec3{a.x + (b.x - a.x) * p.hop, a.y + (b.y - a.y) * p.hop, a.z + (b.z - a.z) * p.hop};
                const f32 l = flat_len(b.x - a.x, b.z - a.z);
                hx = l > 1e-4f ? (b.x - a.x) / l : 0.0f;
                hz = l > 1e-4f ? (b.z - a.z) / l : 0.0f;
            }
        }
    }
    if (p.target == kNone) {
        const Edge& ne = g.edges[p.edge];
        f32 tx = 0.0f, tz = 0.0f;
        where = edge_point(g, ne, p.s, tx, tz);
        hx = p.dir ? tx : -tx;
        hz = p.dir ? tz : -tz;
    }
    t.position = c.net->origin + where;
    if (hx != 0.0f || hz != 0.0f) {
        const f32 want = std::atan2(-hx, -hz);
        if (fresh) {
            p.yaw = want;
        } else {
            f32 delta = want - p.yaw;
            while (delta > kPi) delta -= 2.0f * kPi;
            while (delta < -kPi) delta += 2.0f * kPi;
            const f32 maxd = kTurnRate * dt;
            p.yaw += math::clamp(delta, -maxd, maxd);
        }
    }
    t.rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, p.yaw);
}

// ---- the system -----------------------------------------------------------------------------------------------

struct Info {
    Entity entity;
    u32 id = 0;  // entity index + 1
    CrowdSpawner3D params;
    RoadNet net;
    const Graph* graph = nullptr;
    bool has_net = false;
    u32 count = 0;
};

u32 lod_period(u32 lod) { return lod == 0 ? 1u : lod == 1 ? 4u : 16u; }

void refresh_candidates(Candidates& c, const Info& in, bool focused, const vec3& focus_local) {
    const u32 key = in.net.def->generation * 31u + in.net.def->hash;
    i32 cx = 0, cz = 0;
    if (focused) {
        cx = static_cast<i32>(std::floor(focus_local.x / kCell));
        cz = static_cast<i32>(std::floor(focus_local.z / kCell));
    }
    if (c.valid && c.focused == focused && c.cx == cx && c.cz == cz && c.graph == key && c.radius == in.params.spawn_max) return;
    c.valid = true;
    c.focused = focused;
    c.cx = cx;
    c.cz = cz;
    c.graph = key;
    c.radius = in.params.spawn_max;
    c.edges.clear();
    const f32 mx = (static_cast<f32>(cx) + 0.5f) * kCell, mz = (static_cast<f32>(cz) + 0.5f) * kCell;
    const f32 reach = in.params.spawn_max + kCell;
    for (u32 i = 0; i < in.graph->edges.size(); ++i) {
        const Edge& e = in.graph->edges[i];
        if (focused) {
            const f32 dx = math::max(math::max(e.lo_x - mx, mx - e.hi_x), 0.0f), dz = math::max(math::max(e.lo_z - mz, mz - e.hi_z), 0.0f);
            if (dx * dx + dz * dz > reach * reach) continue;
        }
        c.edges.push_back(i);
    }
}

Candidates& candidates_for(CrowdState& st, u32 crowd) {
    for (Candidates& c : st.candidates)
        if (c.crowd == crowd) return c;
    st.candidates.emplace_back();
    st.candidates.back().crowd = crowd;
    return st.candidates.back();
}

void kit_crowd(Frame& f) {
    World& w = f.world();
    CrowdState& st = *w.try_resource<CrowdState>();
    st.stats = CrowdStats{};
    Physics3D* phys = w.try_resource<Physics3D>();

    // M-PEDFLEE 2: scared pedestrians count down; this tick's shots are the new scares
    {
        Array<Fear> keep;
        for (Fear& fr : st.fears) {
            fr.left -= f.dt;
            if (fr.left > 0.0f && w.alive(fr.ped)) keep.push_back(fr);
        }
        st.fears = static_cast<Array<Fear>&&>(keep);
    }
    // What scares this tick: the muzzles of shots, and the points where a vehicle hit something hard (M-PEDCRASH).
    Array<vec3> sources;
    for (const WeaponFired3D& sh : f.events().read<WeaponFired3D>())
        if (sources.size() < kMaxSources) sources.push_back(sh.origin);
    for (const ContactBegin3D& cb : f.events().read<ContactBegin3D>()) {
        if (cb.approach_speed < kCrashSpeed || sources.size() >= kMaxSources) continue;
        const bool car = (!cb.a.is_null() && w.alive(cb.a) && w.try_get<Vehicle3D>(cb.a)) || (!cb.b.is_null() && w.alive(cb.b) && w.try_get<Vehicle3D>(cb.b));
        if (car) sources.push_back(cb.point);
    }
    // M-PEDFLEE 3: those who ran last tick and still have time to run pass the panic on (one second less per hop, so it dies out)
    Array<Fear> spreaders;
    for (const Fear& fr : st.fears)
        if (fr.left > kPedestrianPanicHop + 0.5f) spreaders.push_back(fr);

    // M-PEDCRASH 2: vehicles moving fast enough to run somebody down, with the line each travels along.
    struct Mover {
        vec3 pos, dir;
        f32 speed;
    };
    Array<Mover> movers;
    w.query<const Vehicle3D&, const LocalTransform&>().each([&](Entity, const Vehicle3D& v, const LocalTransform& t) {
        if (std::fabs(v.speed) < kRunDownSpeed || movers.size() >= kMaxCars) return;
        const vec3 fwd = math::rotate(t.rotation, vec3{0.0f, 0.0f, -1.0f});
        const f32 len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
        if (len < 1e-3f) return;
        const f32 sign = v.speed < 0.0f ? -1.0f : 1.0f;
        movers.push_back(Mover{t.position, vec3{sign * fwd.x / len, 0.0f, sign * fwd.z / len}, std::fabs(v.speed)});
    });

    // The spawners, with the network each walks on.
    Array<Info> infos;
    w.query<const CrowdSpawner3D&>().each([&](Entity e, const CrowdSpawner3D& s) {
        Info in;
        in.entity = e;
        in.id = e.index + 1;
        in.params = s;
        in.has_net = s.network.is_null() ? road_net_first(w, in.net) : road_net(w, s.network, in.net);
        if (in.has_net) in.graph = graph_for(st, in.net);
        infos.push_back(in);
    });
    // Pedestrians with no spawner use the defaults on the first network.
    Info loose;
    loose.params = CrowdSpawner3D{};
    loose.has_net = road_net_first(w, loose.net);
    if (loose.has_net) loose.graph = graph_for(st, loose.net);

    // The focus: a spawner's own entity, else the first camera (the primary one if there are several).
    bool camera_found = false;
    vec3 camera;
    {
        bool primary = false;
        w.query<const Camera3DComponent&, const LocalTransform&>().each([&](Entity, const Camera3DComponent& c, const LocalTransform& t) {
            if (camera_found && (primary || !c.primary)) return;
            camera_found = true;
            primary = c.primary != 0;
            camera = t.position;
        });
    }
    const auto focus_of = [&](const Info& in, vec3& out) {
        if (!in.params.focus.is_null()) {
            if (const LocalTransform* t = w.try_get<LocalTransform>(in.params.focus)) {
                out = t->position;
                return true;
            }
            return false;
        }
        out = camera;
        return camera_found;
    };
    Array<vec3> focus_pos;
    Array<u8> focus_ok;
    for (const Info& in : infos) {
        vec3 p;
        const bool ok = focus_of(in, p);
        focus_pos.push_back(p);
        focus_ok.push_back(ok);
    }
    vec3 loose_focus;
    const bool loose_ok = focus_of(loose, loose_focus);

    // Step the pedestrians.
    f.query<Pedestrian3D, LocalTransform>().each([&](Entity e, Pedestrian3D& p, LocalTransform& t) {
        Info* in = nullptr;
        usize index = 0;
        if (p.crowd != 0) {
            for (usize i = 0; i < infos.size(); ++i)
                if (infos[i].id == p.crowd) {
                    in = &infos[i];
                    index = i;
                }
            if (!in) {  // its spawner is gone
                f.commands().destroy(e);
                ++st.stats.despawned;
                return;
            }
        } else {
            in = &loose;
        }
        // A pedestrian whose Health3D is dead stops where it fell and is removed after kPedestrianCorpseSeconds; it does not count
        // toward the crowd's population, so the spawner refills the street. A revived one walks again.
        const Health3D* hp = w.try_get<Health3D>(e);
        if (hp && hp->dead) {
            if (p.mode != PedestrianMode::Down) {
                p.mode = PedestrianMode::Down;
                p.pace = 0.0f;
                p.lag = 0.0f;
            } else {
                p.lag += f.dt;
            }
            if (p.lag >= kPedestrianCorpseSeconds) {
                f.commands().destroy(e);
                ++st.stats.despawned;
                return;
            }
            ++st.stats.down;
            return;
        }
        if (p.mode == PedestrianMode::Down) p.mode = PedestrianMode::Walk;
        // M-PEDFLEE: hurt and alive: run from the attacker for kPedestrianFleeSeconds
        bool scared = hp && hp->hits > 0 && hp->since_hit < kPedestrianFleeSeconds;
        const bool was_fleeing = p.mode == PedestrianMode::Flee;
        Fear* fear = nullptr;
        for (Fear& fr : st.fears)
            if (fr.ped == e) fear = &fr;
        for (const vec3& origin : sources) {  // a shot or a crash nearby: run from it (the last close one wins)
            const f32 dx = t.position.x - origin.x, dz = t.position.z - origin.z;
            if (dx * dx + dz * dz > kPedestrianFearRadius * kPedestrianFearRadius) continue;
            if (!fear) {
                if (st.fears.size() >= kMaxFears) break;
                st.fears.push_back(Fear{e, origin, t.position, kPedestrianFleeSeconds});
                fear = &st.fears.back();
            }
            fear->threat = origin;
            fear->left = kPedestrianFleeSeconds;
        }
        for (const Mover& m : movers) {  // a vehicle bearing down on it: jump clear (run from where the vehicle is)
            const f32 rx = t.position.x - m.pos.x, rz = t.position.z - m.pos.z;
            const f32 along = rx * m.dir.x + rz * m.dir.z;
            const f32 across = std::fabs(rx * m.dir.z - rz * m.dir.x);
            if (along <= 0.0f || along > m.speed * kRunDownTime + 3.0f || across > kRunDownWidth) continue;
            if (!fear) {
                if (st.fears.size() >= kMaxFears) break;
                st.fears.push_back(Fear{e, m.pos, t.position, kRunDownFlee});
                fear = &st.fears.back();
            } else if (fear->left < kRunDownFlee) {
                fear->threat = m.pos;
                fear->left = kRunDownFlee;
            }
            break;
        }
        if (!fear && !scared)  // a neighbour is running: so do I (the shot's or the attacker's side)
            for (const Fear& sp : spreaders) {
                const f32 dx = t.position.x - sp.pos.x, dz = t.position.z - sp.pos.z;
                if (dx * dx + dz * dz > kPedestrianPanicRadius * kPedestrianPanicRadius || st.fears.size() >= kMaxFears) continue;
                st.fears.push_back(Fear{e, sp.threat, t.position, sp.left - kPedestrianPanicHop});
                fear = &st.fears.back();
                break;
            }
        if (!fear && scared && hp && !hp->last_attacker.is_null() && w.alive(hp->last_attacker) && st.fears.size() < kMaxFears) {
            if (const LocalTransform* at = w.try_get<LocalTransform>(hp->last_attacker)) {  // the hurt one also warns the others
                st.fears.push_back(Fear{e, at->position, t.position, kPedestrianFleeSeconds - hp->since_hit});
                fear = &st.fears.back();
            }
        }
        if (fear) {
            scared = true;
            fear->pos = t.position;
        }
        if (!scared && was_fleeing) p.mode = PedestrianMode::Walk;
        const bool focused = in == &loose ? loose_ok : focus_ok[index] != 0;
        const vec3 focus = in == &loose ? loose_focus : focus_pos[index];
        ++st.stats.pedestrians;
        ++in->count;
        u32 lod = 0;
        if (focused) {
            const f32 dx = t.position.x - focus.x, dz = t.position.z - focus.z;
            const f32 d2 = dx * dx + dz * dz;
            if (in->params.despawn_radius > 0.0f && d2 > in->params.despawn_radius * in->params.despawn_radius) {
                f.commands().destroy(e);
                ++st.stats.despawned;
                --in->count;
                --st.stats.pedestrians;
                return;
            }
            lod = d2 <= in->params.near_radius * in->params.near_radius ? 0u : d2 <= in->params.far_radius * in->params.far_radius ? 1u : 2u;
        }
        p.lod = static_cast<u8>(lod);
        (lod == 0 ? st.stats.near : lod == 1 ? st.stats.mid : st.stats.far) += 1;
        p.lag += f.dt;
        if (p.mode == PedestrianMode::Wait) ++st.stats.waiting;
        if (p.mode == PedestrianMode::Cross) ++st.stats.crossing;
        if (!in->has_net || !in->graph || in->graph->edges.empty()) return;
        if ((f.tick + e.index) % lod_period(lod) != 0) return;
        Crowd c;
        c.params = &in->params;
        c.net = &in->net;
        c.graph = in->graph;
        if (scared) {
            c.flee = true;
            c.speed_scale = kPedestrianFleeScale;
            if (!was_fleeing) {
                const LocalTransform* at = !hp || hp->last_attacker.is_null() || !w.alive(hp->last_attacker) ? nullptr : w.try_get<LocalTransform>(hp->last_attacker);
                if (fear) {  // a shot is the fresher news
                    c.turn = true;
                    c.threat = fear->threat - in->net.origin;
                } else if (at) {
                    c.turn = true;
                    c.threat = at->position - in->net.origin;
                }
            }
        }
        step_pedestrian(p, t, c, p.lag);
        // A walker with a body (a kinematic capsule, so cars and shots hit it): the body goes with it now. kit3d_bodies
        // already synced this tick's bodies, so without this the write-back after the step would put the old pose back
        // and the walker would never leave its spawn point.
        if (phys && w.try_get<RigidBody3D>(e) && phys->world.has_body(e)) phys->world.teleport(e, t.position, t.rotation);
        if (scared) p.mode = PedestrianMode::Flee;
        p.lag = 0.0f;
        ++st.stats.stepped;
    });

    // Spawn around each focus.
    for (usize i = 0; i < infos.size(); ++i) {
        Info& in = infos[i];
        if (!in.has_net || !in.graph || in.graph->edges.empty() || !in.params.enabled) continue;
        CrowdSpawner3D* sp = w.try_get_mut<CrowdSpawner3D>(in.entity);
        if (!sp) continue;
        const bool focused = focus_ok[i] != 0;
        const vec3 focus_local = focus_pos[i] - in.net.origin;
        Candidates& cand = candidates_for(st, in.id);
        refresh_candidates(cand, in, focused, focus_local);
        if (cand.edges.empty()) continue;
        asset::AssetSystem* assets = st.assets;
        bool prefab_failed = false;
        asset::AssetHandle<asset::Prefab> handle;
        bool have_handle = false;
        for (u32 attempt = 0; attempt < in.params.per_tick && in.count < in.params.max_count; ++attempt) {
            const u64 h = hash_combine(hash_combine(in.params.seed, in.id), sp->serial++);
            const Edge& e = in.graph->edges[cand.edges[static_cast<usize>(h & 0xFFFFFFFFull) % cand.edges.size()]];
            u32 rng = static_cast<u32>(mix64(h) >> 32) | 1u;
            const f32 s = e.length * unit_rand(rng);
            f32 tx = 0.0f, tz = 0.0f;
            const vec3 local = edge_point(*in.graph, e, s, tx, tz);
            const vec3 world_pos = in.net.origin + local;
            if (focused) {
                const f32 dx = world_pos.x - focus_pos[i].x, dz = world_pos.z - focus_pos[i].z;
                const f32 d2 = dx * dx + dz * dz;
                if (d2 < in.params.spawn_min * in.params.spawn_min || d2 > in.params.spawn_max * in.params.spawn_max) continue;
            }
            Pedestrian3D p;
            p.crowd = in.id;
            p.edge = static_cast<u32>(&e - in.graph->edges.data());
            p.s = s;
            p.dir = (next_rand(rng) & 1u) ? 1 : 0;
            p.speed = in.params.speed_min + unit_rand(rng) * math::max(in.params.speed_max - in.params.speed_min, 0.0f);
            p.rng = rng;
            const f32 hx = p.dir ? tx : -tx, hz = p.dir ? tz : -tz;
            p.yaw = std::atan2(-hx, -hz);
            const math::quat rot = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, p.yaw);
            ++in.count;
            ++st.stats.spawned;
            ++st.stats.pedestrians;
            if (in.params.prefab.guid.is_null() || !assets || prefab_failed) {
                f.commands().spawn(LocalTransform{world_pos, rot, {1.0f, 1.0f, 1.0f}}, p);
                continue;
            }
            if (!have_handle) {
                const asset::AssetKey key = assets->find(in.params.prefab.guid);
                Result<asset::AssetHandle<asset::Prefab>> r = key == asset::kInvalidAssetKey
                                                                  ? Result<asset::AssetHandle<asset::Prefab>>(Error(Errc::NotFound, "kit.crowd.prefab"))
                                                                  : assets->load_sync<asset::Prefab>(key);
                if (!r) {
                    MCE_LOG_WARN("kit: CrowdSpawner3D prefab {} cannot be loaded; spawning bare pedestrians", asset::to_string(in.params.prefab.guid).view());
                    prefab_failed = true;
                    f.commands().spawn(LocalTransform{world_pos, rot, {1.0f, 1.0f, 1.0f}}, p);
                    continue;
                }
                handle = r.value();
                have_handle = true;
            }
            ecs::PrefabOverride overrides[9] = {
                ecs::PrefabOverride::of("Pedestrian3D.crowd", p.crowd), ecs::PrefabOverride::of("Pedestrian3D.edge", p.edge),
                ecs::PrefabOverride::of("Pedestrian3D.s", p.s),         ecs::PrefabOverride::of("Pedestrian3D.dir", p.dir),
                ecs::PrefabOverride::of("Pedestrian3D.speed", p.speed), ecs::PrefabOverride::of("Pedestrian3D.rng", p.rng),
                ecs::PrefabOverride::of("Pedestrian3D.yaw", p.yaw),     ecs::PrefabOverride::of("LocalTransform.position", world_pos),
                ecs::PrefabOverride::of("LocalTransform.rotation", rot)};
            ecs::SpawnParams sp_params;
            sp_params.overrides = Span<const ecs::PrefabOverride>(overrides, 9);
            if (Result<u32> r = ecs::instantiate(w, assets->resolve(handle), sp_params); !r) {
                FixedString<512> why;
                format_error(r.error(), why);
                MCE_LOG_WARN("kit: CrowdSpawner3D prefab {} failed: {} (it needs a LocalTransform and a Pedestrian3D)",
                             asset::to_string(in.params.prefab.guid).view(), why.view());
                prefab_failed = true;
            }
        }
        if (have_handle) assets->release(asset::AnyHandle(handle));
    }
}

}  // namespace

const CrowdStats* crowd_stats(World& world) {
    const CrowdState* st = world.try_resource<CrowdState>();
    return st ? &st->stats : nullptr;
}

void install_crowd(World& world, asset::AssetSystem* assets) {
    if (CrowdState* st = world.try_resource<CrowdState>()) {
        st->assets = assets;
        return;
    }
    world.register_components<Pedestrian3D, CrowdSpawner3D, LocalTransform, Camera3DComponent, Vehicle3D>();
    world.register_event<WeaponFired3D>(Phase::FixedUpdate_Pre);  // the combat kit's; registering twice is a no-op
    world.register_event<ContactBegin3D>(Phase::FixedUpdate_Physics);  // the bodies kit's, likewise
    CrowdState st;
    st.assets = assets;
    world.set_resource(static_cast<CrowdState&&>(st));
    world.add_system(Phase::FixedUpdate_Pre, "kit_crowd", &kit_crowd);
}

}  // namespace mce::kit
