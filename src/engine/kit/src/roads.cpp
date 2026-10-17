#include "mce/kit/roads.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/asset/asset_system.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/hash.h"
#include "mce/log.h"
#include "mce/string_id.h"

namespace mce::kit {

using asset::kRoadNone;
using asset::RoadLane;
using asset::RoadLaneKind;
using asset::RoadPoint;

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(RoadNetwork3D) == 32);

namespace {

constexpr u32 kMaxGridCells = 1u << 20;

math::vec3 at(const RoadPoint& p) { return {p.x, p.y, p.z}; }

// Builds the segment grid: every lane segment is listed in each cell its bounding box touches.
void build_grid(RoadRegistry::Def& d) {
    d.point_lane.clear();
    d.point_lane.resize(d.points.size(), kRoadNone);
    for (u32 i = 0; i < d.lanes.size(); ++i)
        for (u32 k = 0; k < d.lanes[i].point_count; ++k) d.point_lane[d.lanes[i].first_point + k] = i;
    f32 lo_x = 1e30f, lo_z = 1e30f, hi_x = -1e30f, hi_z = -1e30f;
    for (const RoadLane& l : d.lanes)
        for (u32 k = 0; k < l.point_count; ++k) {
            const RoadPoint& p = d.points[l.first_point + k];
            lo_x = math::min(lo_x, p.x);
            lo_z = math::min(lo_z, p.z);
            hi_x = math::max(hi_x, p.x);
            hi_z = math::max(hi_z, p.z);
        }
    d.cell_start.clear();
    d.cell_items.clear();
    d.grid_w = d.grid_h = 0;
    if (d.lanes.empty()) return;
    d.cell = 32.0f;
    // A huge network gets coarser cells so the grid stays bounded.
    while ((static_cast<f64>(hi_x - lo_x) / d.cell + 1.0) * (static_cast<f64>(hi_z - lo_z) / d.cell + 1.0) > kMaxGridCells) d.cell *= 2.0f;
    d.grid_x = lo_x;
    d.grid_z = lo_z;
    d.grid_w = static_cast<u32>((hi_x - lo_x) / d.cell) + 1;
    d.grid_h = static_cast<u32>((hi_z - lo_z) / d.cell) + 1;
    const u32 cells = d.grid_w * d.grid_h;
    Array<u32> counts;
    counts.resize(cells + 1, 0u);
    const auto each_cell = [&](const RoadPoint& a, const RoadPoint& b, auto&& fn) {
        const u32 x0 = static_cast<u32>((math::min(a.x, b.x) - d.grid_x) / d.cell), x1 = static_cast<u32>((math::max(a.x, b.x) - d.grid_x) / d.cell);
        const u32 z0 = static_cast<u32>((math::min(a.z, b.z) - d.grid_z) / d.cell), z1 = static_cast<u32>((math::max(a.z, b.z) - d.grid_z) / d.cell);
        for (u32 z = z0; z <= z1 && z < d.grid_h; ++z)
            for (u32 x = x0; x <= x1 && x < d.grid_w; ++x) fn(z * d.grid_w + x);
    };
    for (const RoadLane& l : d.lanes)
        for (u32 k = 0; k + 1 < l.point_count; ++k)
            each_cell(d.points[l.first_point + k], d.points[l.first_point + k + 1], [&](u32 c) { ++counts[c + 1]; });
    for (u32 c = 0; c < cells; ++c) counts[c + 1] += counts[c];
    d.cell_start = counts;
    d.cell_items.resize(counts[cells], 0u);
    for (const RoadLane& l : d.lanes)
        for (u32 k = 0; k + 1 < l.point_count; ++k)
            each_cell(d.points[l.first_point + k], d.points[l.first_point + k + 1], [&](u32 c) { d.cell_items[counts[c]++] = l.first_point + k; });
}

Result<void> copy(RoadRegistry::Def& d, asset::Guid guid, Span<const byte> payload) {
    Result<asset::RoadView> view = asset::RoadView::read(payload);
    if (!view) return view.error();
    const asset::RoadView& v = view.value();
    d.guid = guid;
    d.name.assign(v.name());
    d.flags = v.header->flags;
    d.hash = static_cast<u32>(fnv1a64(payload.data(), payload.size()));
    const auto fill = [](auto& out, auto in) {
        out.clear();
        for (const auto& x : in) out.push_back(x);
    };
    fill(d.nodes, v.nodes);
    fill(d.roads, v.roads);
    fill(d.lanes, v.lanes);
    fill(d.points, v.points);
    fill(d.conflicts, v.conflicts);
    fill(d.signals, v.signals);
    fill(d.phases, v.phases);
    build_grid(d);
    return {};
}

// The closest point to `p` on segment ab, flat; t along it.
f32 closest_flat(math::vec3 p, math::vec3 a, math::vec3 b, f32& t) {
    const f32 dx = b.x - a.x, dz = b.z - a.z;
    const f32 l2 = dx * dx + dz * dz;
    t = l2 > 1e-12f ? math::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / l2, 0.0f, 1.0f) : 0.0f;
    const f32 ex = a.x + dx * t - p.x, ez = a.z + dz * t - p.z;
    return std::sqrt(ex * ex + ez * ez);
}

math::vec3 flat_unit(math::vec3 v) {
    const f32 l = std::sqrt(v.x * v.x + v.z * v.z);
    return l > 1e-6f ? math::vec3{v.x / l, 0.0f, v.z / l} : math::vec3{0.0f, 0.0f, -1.0f};
}

// FixedUpdate_Pre: the lights' clock.
void roads_clock(Frame& f) {
    f.query<RoadNetwork3D>().each([&](Entity, RoadNetwork3D& n) {
        if (n.lights) n.time += f.dt;
    });
}

}  // namespace

u32 RoadRegistry::Def::find_road(StringView road_name) const {
    const u64 id = StringId(road_name).value;
    for (u32 i = 0; i < roads.size(); ++i)
        if (roads[i].name == id) return i;
    return kRoadNone;
}

u32 RoadRegistry::Def::find_node(StringView node_name) const {
    const u64 id = StringId(node_name).value;
    for (u32 i = 0; i < nodes.size(); ++i)
        if (nodes[i].name == id) return i;
    return kRoadNone;
}

Result<void> RoadRegistry::define(asset::Guid guid, Span<const byte> payload) {
    Def d;
    if (Result<void> r = copy(d, guid, payload); !r) return r;
    for (Def& existing : defs_)
        if (existing.guid == guid) {
            existing = static_cast<Def&&>(d);
            return {};
        }
    defs_.push_back(static_cast<Def&&>(d));
    return {};
}

const RoadRegistry::Def* RoadRegistry::resolve(const RoadNetwork3D& n) {
    const asset::Guid guid = n.roads.guid;
    if (guid == asset::Guid{}) return nullptr;
    Def* cached = nullptr;
    for (Def& d : defs_)
        if (d.guid == guid) cached = &d;
    if (cached && (!assets_ || cached->key == asset::kInvalidAssetKey || assets_->generation(cached->key) == cached->generation))
        return cached;
    if (!assets_) return nullptr;
    const bool fresh = cached == nullptr;
    if (fresh) {
        defs_.push_back(Def{});
        cached = &defs_.back();
    }
    const asset::AssetKey key = assets_->find(guid);
    bool loaded = false;
    if (key != asset::kInvalidAssetKey) {
        if (Result<asset::AssetHandle<asset::RoadNetwork>> h = assets_->load_sync<asset::RoadNetwork>(key)) {
            loaded = static_cast<bool>(copy(*cached, guid, assets_->resolve(h.value())));
            cached->key = key;
            cached->generation = h.value().generation;
            assets_->release(asset::AnyHandle(h.value()));
        }
    }
    if (loaded) return cached;
    if (fresh) defs_.pop_back();
    bool warned = false;
    for (const asset::Guid& g : warned_) warned |= g == guid;
    if (!warned) {
        MCE_LOG_WARN("kit: road network {} cannot be loaded; nothing drives on it", asset::to_string(guid).view());
        warned_.push_back(guid);
    }
    return nullptr;
}

bool road_net(World& w, Entity e, RoadNet& out) {
    out = RoadNet{};
    if (e.is_null() || !w.alive(e)) return false;
    const RoadNetwork3D* n = w.try_get<RoadNetwork3D>(e);
    RoadRegistry* reg = w.try_resource<RoadRegistry>();
    if (!n || !reg) return false;
    out.def = reg->resolve(*n);
    if (!out.def) return false;
    out.entity = e;
    out.time = n->time;
    if (const LocalTransform* t = w.try_get<LocalTransform>(e)) {
        math::quat rot;
        math::vec3 scale;
        world_pose(w, e, *t, out.origin, rot, scale);
    }
    return true;
}

bool road_net_first(World& w, RoadNet& out) {
    bool found = false;
    w.query<const RoadNetwork3D>().each([&](Entity e, const RoadNetwork3D&) {
        if (!found) found = road_net(w, e, out);
    });
    if (!found) out = RoadNet{};
    return found;
}

bool road_nearest(const RoadNet& net, math::vec3 position, RoadSpot& out, const RoadNearestQuery& q) {
    if (!net.def || net.def->grid_w == 0) return false;
    const RoadRegistry::Def& d = *net.def;
    const math::vec3 p = position - net.origin;
    const bool headed = q.heading.x * q.heading.x + q.heading.z * q.heading.z > 1e-8f;
    f32 best = q.max_distance;
    u32 best_seg = kRoadNone;
    f32 best_t = 0.0f;
    // Cells within reach of the point; a segment spanning several cells is tested more than once.
    const i64 cx0 = static_cast<i64>(std::floor((p.x - q.max_distance - d.grid_x) / d.cell));
    const i64 cx1 = static_cast<i64>(std::floor((p.x + q.max_distance - d.grid_x) / d.cell));
    const i64 cz0 = static_cast<i64>(std::floor((p.z - q.max_distance - d.grid_z) / d.cell));
    const i64 cz1 = static_cast<i64>(std::floor((p.z + q.max_distance - d.grid_z) / d.cell));
    for (i64 z = math::max<i64>(cz0, 0); z <= math::min<i64>(cz1, i64(d.grid_h) - 1); ++z)
        for (i64 x = math::max<i64>(cx0, 0); x <= math::min<i64>(cx1, i64(d.grid_w) - 1); ++x) {
            const u32 c = static_cast<u32>(z) * d.grid_w + static_cast<u32>(x);
            for (u32 k = d.cell_start[c]; k < d.cell_start[c + 1]; ++k) {
                const u32 seg = d.cell_items[k];
                const RoadLane& l = d.lanes[d.point_lane[seg]];
                if (l.kind == RoadLaneKind::Connector && !q.connectors) continue;
                const math::vec3 a = at(d.points[seg]), b = at(d.points[seg + 1]);
                if (headed && (b.x - a.x) * q.heading.x + (b.z - a.z) * q.heading.z <= 0.0f) continue;
                f32 t = 0.0f;
                const f32 dist = closest_flat(p, a, b, t);
                // Ties go to the lower segment index, so the answer does not depend on cell order.
                if (dist < best || (dist == best && seg < best_seg)) {
                    best = dist;
                    best_seg = seg;
                    best_t = t;
                }
            }
        }
    if (best_seg == kRoadNone) return false;
    const u32 lane = d.point_lane[best_seg];
    const RoadPoint& a = d.points[best_seg];
    const RoadPoint& b = d.points[best_seg + 1];
    out.lane = lane;
    out.s = a.s + (b.s - a.s) * best_t;
    out.distance = best;
    out.position = net.origin + at(a) + (at(b) - at(a)) * best_t;
    out.direction = flat_unit(at(b) - at(a));
    return true;
}

math::vec3 road_lane_point(const RoadNet& net, u32 lane, f32 s, math::vec3* direction) {
    if (!net.def || lane >= net.def->lanes.size()) {
        if (direction) *direction = {0.0f, 0.0f, -1.0f};
        return net.origin;
    }
    const RoadRegistry::Def& d = *net.def;
    const RoadLane& l = d.lanes[lane];
    const RoadPoint* pts = d.points.data() + l.first_point;
    s = math::clamp(s, 0.0f, l.length);
    // Binary search for the segment holding s.
    u32 lo = 0, hi = l.point_count - 1;
    while (hi - lo > 1) {
        const u32 mid = (lo + hi) / 2;
        if (pts[mid].s <= s) lo = mid;
        else hi = mid;
    }
    const f32 span = pts[hi].s - pts[lo].s;
    const f32 t = span > 1e-6f ? (s - pts[lo].s) / span : 0.0f;
    if (direction) *direction = flat_unit(at(pts[hi]) - at(pts[lo]));
    return net.origin + at(pts[lo]) + (at(pts[hi]) - at(pts[lo])) * t;
}

RoadLight road_light(const RoadRegistry::Def& d, f32 time, u32 lane, f32* seconds_left) {
    constexpr f32 kNever = 1e9f;
    if (seconds_left) *seconds_left = kNever;
    if (lane >= d.lanes.size()) return RoadLight::Green;
    const RoadLane& l = d.lanes[lane];
    if (l.kind != RoadLaneKind::Connector) return RoadLight::Green;
    const asset::RoadNode& n = d.nodes[l.node];
    if (n.signal == asset::kRoadNone16 || n.signal >= d.signals.size()) return RoadLight::Green;
    const asset::RoadSignal& s = d.signals[n.signal];
    f32 u = std::fmod(time + s.offset, s.cycle);
    if (u < 0.0f) u += s.cycle;
    const asset::RoadPhase* ph = d.phases.data() + s.first_phase;
    // Seconds from u to the start of the next phase (cyclically) that is green for this lane.
    const auto to_next_green = [&](f32 from) {
        f32 best = kNever;
        for (u32 p = 0; p < s.phase_count; ++p) {
            if ((l.green & (1u << p)) == 0) continue;
            f32 dt = ph[p].start - from;
            if (dt <= 0.0f) dt += s.cycle;
            best = math::min(best, dt);
        }
        return best;
    };
    for (u32 p = 0; p < s.phase_count; ++p) {
        const f32 g0 = ph[p].start, g1 = g0 + ph[p].seconds, a1 = g1 + s.amber;
        if (u < g0 || u >= a1 + s.all_red) continue;
        const bool mine = (l.green & (1u << p)) != 0;
        if (mine && u < g1) {
            if (seconds_left) *seconds_left = g1 - u;
            return RoadLight::Green;
        }
        if (mine && u < a1) {
            if (seconds_left) *seconds_left = a1 - u;
            return RoadLight::Amber;
        }
        break;
    }
    if (seconds_left) *seconds_left = to_next_green(u);
    return RoadLight::Red;
}

RoadLight road_light(const RoadNet& net, u32 lane, f32* seconds_left) {
    if (!net.def) {
        if (seconds_left) *seconds_left = 1e9f;
        return RoadLight::Green;
    }
    return road_light(*net.def, net.time, lane, seconds_left);
}

bool road_route(const RoadRegistry::Def& d, u32 from, u32 to, Array<u32>& out) {
    out.clear();
    const u32 n = static_cast<u32>(d.lanes.size());
    if (from >= n || to >= n) return false;
    if (from == to) {
        out.push_back(from);
        return true;
    }
    // A* over lanes: the cost of a lane is its travel time; the heuristic is the flat distance from
    // its end to the goal's start at the network's top speed.
    f32 top = 1.0f;
    for (const RoadLane& l : d.lanes) top = math::max(top, l.speed);
    const RoadPoint& goal = d.points[d.lanes[to].first_point];
    const auto h = [&](u32 lane) {
        const RoadLane& l = d.lanes[lane];
        const RoadPoint& e = d.points[l.first_point + l.point_count - 1];
        const f32 dx = e.x - goal.x, dz = e.z - goal.z;
        return std::sqrt(dx * dx + dz * dz) / top;
    };
    const auto cost = [&](u32 lane) { return d.lanes[lane].length / math::max(d.lanes[lane].speed, 0.5f); };
    Array<f32> g;
    g.resize(n, 1e30f);
    Array<u32> came;
    came.resize(n, kRoadNone);
    struct Open {
        f32 f;
        u32 lane;
    };
    Array<Open> heap;  // a binary min-heap on f, ties on the lane index (deterministic)
    const auto less = [](const Open& a, const Open& b) { return a.f < b.f || (a.f == b.f && a.lane < b.lane); };
    const auto push = [&](Open o) {
        heap.push_back(o);
        usize i = heap.size() - 1;
        while (i > 0) {
            const usize parent = (i - 1) / 2;
            if (!less(heap[i], heap[parent])) break;
            const Open t = heap[i];
            heap[i] = heap[parent];
            heap[parent] = t;
            i = parent;
        }
    };
    const auto pop = [&]() {
        const Open top_item = heap[0];
        heap[0] = heap.back();
        heap.pop_back();
        usize i = 0;
        for (;;) {
            const usize l = 2 * i + 1, r = l + 1;
            usize m = i;
            if (l < heap.size() && less(heap[l], heap[m])) m = l;
            if (r < heap.size() && less(heap[r], heap[m])) m = r;
            if (m == i) break;
            const Open t = heap[i];
            heap[i] = heap[m];
            heap[m] = t;
            i = m;
        }
        return top_item;
    };
    g[from] = cost(from);
    push(Open{g[from] + h(from), from});
    while (!heap.empty()) {
        const Open o = pop();
        if (o.lane == to) break;
        if (o.f > g[o.lane] + h(o.lane) + 1e-4f) continue;  // a stale entry
        const RoadLane& l = d.lanes[o.lane];
        for (u32 k = 0; k < l.next_count; ++k) {
            const u32 next = l.first_next + k;
            const f32 cand = g[o.lane] + cost(next);
            if (cand < g[next]) {
                g[next] = cand;
                came[next] = o.lane;
                push(Open{cand + h(next), next});
            }
        }
    }
    if (came[to] == kRoadNone) return false;
    for (u32 c = to; c != kRoadNone; c = c == from ? kRoadNone : came[c]) out.push_back(c);
    for (usize i = 0, j = out.size() - 1; i < j; ++i, --j) {
        const u32 t = out[i];
        out[i] = out[j];
        out[j] = t;
    }
    return true;
}

void road_debug_lines(World& w, Array<RoadLine>& out) {
    out.clear();
    const math::Color road{0.6f, 0.6f, 0.65f, 1.0f}, free{0.95f, 0.95f, 0.95f, 1.0f}, green{0.2f, 0.9f, 0.3f, 1.0f},
        amber{1.0f, 0.75f, 0.1f, 1.0f}, red{0.95f, 0.2f, 0.15f, 1.0f};
    w.query<const RoadNetwork3D>().each([&](Entity e, const RoadNetwork3D&) {
        RoadNet net;
        if (!road_net(w, e, net)) return;
        const RoadRegistry::Def& d = *net.def;
        const math::vec3 lift{0.0f, 0.05f, 0.0f};
        for (u32 i = 0; i < d.lanes.size(); ++i) {
            const RoadLane& l = d.lanes[i];
            math::Color c = road;
            if (l.kind == RoadLaneKind::Connector) {
                const bool signalled = d.nodes[l.node].signal != asset::kRoadNone16;
                const RoadLight light = road_light(net, i);
                c = !signalled ? free : light == RoadLight::Green ? green : light == RoadLight::Amber ? amber : red;
            }
            for (u32 k = 0; k + 1 < l.point_count; ++k)
                out.push_back(RoadLine{net.origin + at(d.points[l.first_point + k]) + lift, net.origin + at(d.points[l.first_point + k + 1]) + lift, c});
        }
    });
}

void install_roads(World& world, asset::AssetSystem* assets) {
    if (RoadRegistry* r = world.try_resource<RoadRegistry>()) {
        r->set_assets(assets);
        return;
    }
    world.register_components<RoadNetwork3D>();
    world.set_resource(RoadRegistry(assets));
    world.add_system(Phase::FixedUpdate_Pre, "roads_clock", &roads_clock);
}

}  // namespace mce::kit
