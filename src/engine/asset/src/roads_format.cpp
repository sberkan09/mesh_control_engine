#include "mce/asset/roads_format.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "mce/asset/container.h"

namespace mce::asset {

namespace {

constexpr const char* kStage = "asset.roads.read";

// The blocks after the header, in payload order, each 16-byte aligned.
struct Layout {
    u64 nodes, roads, lanes, points, conflicts, signals, phases, end;
};

u64 align16(u64 v) { return (v + (CHUNK_ALIGN - 1)) & ~u64(CHUNK_ALIGN - 1); }

Layout layout(const RoadHeader& h) {
    Layout l{};
    l.nodes = align16(sizeof(RoadHeader));
    l.roads = align16(l.nodes + u64(h.node_count) * sizeof(RoadNode));
    l.lanes = align16(l.roads + u64(h.road_count) * sizeof(RoadRoad));
    l.points = align16(l.lanes + u64(h.lane_count) * sizeof(RoadLane));
    l.conflicts = align16(l.points + u64(h.point_count) * sizeof(RoadPoint));
    l.signals = align16(l.conflicts + u64(h.conflict_count) * sizeof(u32));
    l.phases = align16(l.signals + u64(h.signal_count) * sizeof(RoadSignal));
    l.end = l.phases + u64(h.phase_count) * sizeof(RoadPhase);
    return l;
}

template <class T>
Span<const T> at(Span<const byte> payload, u64 offset, u32 count) {
    return Span<const T>(reinterpret_cast<const T*>(payload.data() + offset), count);
}

bool finite(f32 v) { return std::isfinite(v); }
bool range_ok(u64 first, u64 count, u64 size) { return first <= size && count <= size - first; }
Error bad(u32 detail) { return Error(Errc::InvalidArgument, kStage, {}, detail); }

}  // namespace

StringView RoadView::name() const {
    if (!header) return {};
    usize n = 0;
    while (n < sizeof(header->name) && header->name[n] != '\0') ++n;
    return StringView(header->name, n);
}

Result<RoadView> RoadView::read(Span<const byte> payload) {
    if (payload.size() < sizeof(RoadHeader)) return Error(Errc::Truncated, kStage);
    if (reinterpret_cast<std::uintptr_t>(payload.data()) % alignof(RoadHeader) != 0) return Error(Errc::Truncated, kStage);
    RoadView v;
    v.header = reinterpret_cast<const RoadHeader*>(payload.data());
    const RoadHeader& h = *v.header;
    if (h.node_count > kRoadMaxNodes || h.road_count > kRoadMaxItems || h.lane_count > kRoadMaxItems ||
        h.point_count > kRoadMaxItems || h.conflict_count > kRoadMaxItems || h.signal_count > kRoadMaxItems ||
        h.phase_count > kRoadMaxItems || (h.flags & ~u32(RoadFlags::LeftHand)) != 0)
        return bad(1);
    for (u32 i = 0; i < 3; ++i)
        if (!finite(h.bounds_min[i]) || !finite(h.bounds_max[i])) return bad(2);
    const Layout l = layout(h);
    if (l.end > payload.size()) return Error(Errc::Truncated, kStage);
    v.nodes = at<RoadNode>(payload, l.nodes, h.node_count);
    v.roads = at<RoadRoad>(payload, l.roads, h.road_count);
    v.lanes = at<RoadLane>(payload, l.lanes, h.lane_count);
    v.points = at<RoadPoint>(payload, l.points, h.point_count);
    v.conflicts = at<u32>(payload, l.conflicts, h.conflict_count);
    v.signals = at<RoadSignal>(payload, l.signals, h.signal_count);
    v.phases = at<RoadPhase>(payload, l.phases, h.phase_count);

    for (const RoadPoint& p : v.points)
        if (!finite(p.x) || !finite(p.y) || !finite(p.z) || !finite(p.s)) return bad(3);
    for (const RoadNode& n : v.nodes) {
        if (!finite(n.position[0]) || !finite(n.position[1]) || !finite(n.position[2])) return bad(4);
        if (n.control > RoadControl::Signal || n.road_count > kRoadMaxRoadsPerNode) return bad(4);
        if ((n.control == RoadControl::Signal) != (n.signal != kRoadNone16)) return bad(4);
        if (n.signal != kRoadNone16 && n.signal >= h.signal_count) return bad(4);
        if (!range_ok(n.first_connector, n.connector_count, h.lane_count)) return bad(4);
    }
    for (const RoadRoad& r : v.roads) {
        if (r.from >= h.node_count || r.to >= h.node_count || r.forward + r.backward == 0 ||
            r.forward > kRoadMaxLanesPerSide || r.backward > kRoadMaxLanesPerSide)
            return bad(5);
        if (!finite(r.lane_width) || r.lane_width <= 0.0f || !finite(r.speed) || r.speed < 0.0f || !finite(r.sidewalk) ||
            r.sidewalk < 0.0f || !finite(r.length) || r.length < 0.0f)
            return bad(5);
        if (r.point_count < 2 || !range_ok(r.first_point, r.point_count, h.point_count)) return bad(5);
        if (!range_ok(r.first_lane, u64(r.forward) + r.backward, h.lane_count)) return bad(5);
    }
    for (u32 i = 0; i < h.lane_count; ++i) {
        const RoadLane& ln = v.lanes[i];
        if (ln.kind > RoadLaneKind::Connector || ln.point_count < 2 || !range_ok(ln.first_point, ln.point_count, h.point_count))
            return bad(6);
        if (!finite(ln.length) || ln.length < 0.0f || !finite(ln.speed) || ln.speed < 0.0f || !finite(ln.width) || ln.width < 0.0f)
            return bad(6);
        if (ln.node >= h.node_count || !range_ok(ln.first_next, ln.next_count, h.lane_count)) return bad(6);
        // s rises from 0 to the length along the polyline.
        const Span<const RoadPoint> pts(v.points.data() + ln.first_point, ln.point_count);
        if (pts[0].s != 0.0f || std::fabs(pts[ln.point_count - 1].s - ln.length) > 1e-3f * (1.0f + ln.length)) return bad(6);
        for (u32 k = 1; k < ln.point_count; ++k)
            if (pts[k].s < pts[k - 1].s) return bad(6);
        if (ln.kind == RoadLaneKind::Road) {
            if (ln.road >= h.road_count || ln.prev != kRoadNone || ln.conflict_count != 0 || ln.index >= kRoadMaxLanesPerSide)
                return bad(7);
            for (u32 k = 0; k < ln.next_count; ++k) {
                const RoadLane& c = v.lanes[ln.first_next + k];
                if (c.kind != RoadLaneKind::Connector || c.prev != i || c.node != ln.node) return bad(7);
            }
        } else {
            if (ln.road != kRoadNone16 || ln.next_count != 1 || ln.prev >= h.lane_count || ln.index > u8(RoadTurn::UTurn))
                return bad(8);
            if (v.lanes[ln.first_next].kind != RoadLaneKind::Road || v.lanes[ln.prev].kind != RoadLaneKind::Road) return bad(8);
            if (!range_ok(ln.first_conflict, ln.conflict_count, h.conflict_count)) return bad(8);
            for (u32 k = 0; k < ln.conflict_count; ++k) {
                const u32 c = v.conflicts[ln.first_conflict + k];
                if (c >= h.lane_count || c == i || v.lanes[c].kind != RoadLaneKind::Connector) return bad(8);
            }
        }
    }
    for (const RoadSignal& s : v.signals) {
        if (s.node >= h.node_count || v.nodes[s.node].control != RoadControl::Signal || s.phase_count == 0 ||
            s.phase_count > kRoadMaxPhases || !range_ok(s.first_phase, s.phase_count, h.phase_count))
            return bad(9);
        if (!finite(s.amber) || s.amber < 0.0f || !finite(s.all_red) || s.all_red < 0.0f || !finite(s.offset) || !finite(s.cycle) ||
            s.cycle <= 0.0f)
            return bad(9);
        for (u32 k = 0; k < s.phase_count; ++k) {
            const RoadPhase& p = v.phases[s.first_phase + k];
            if (!finite(p.seconds) || p.seconds < 0.0f || !finite(p.start) || p.start < 0.0f || p.start > s.cycle) return bad(9);
        }
    }
    return v;
}

void write_road_network(const RoadTables& t, Array<byte>& out) {
    RoadHeader h = t.header;
    h.node_count = static_cast<u32>(t.nodes.size());
    h.road_count = static_cast<u32>(t.roads.size());
    h.lane_count = static_cast<u32>(t.lanes.size());
    h.point_count = static_cast<u32>(t.points.size());
    h.conflict_count = static_cast<u32>(t.conflicts.size());
    h.signal_count = static_cast<u32>(t.signals.size());
    h.phase_count = static_cast<u32>(t.phases.size());
    const Layout l = layout(h);
    out.clear();
    out.resize(static_cast<usize>(align16(l.end)), byte{0});
    std::memcpy(out.data(), &h, sizeof(h));
    const auto put = [&](u64 offset, const void* data, usize bytes) {
        if (bytes) std::memcpy(out.data() + offset, data, bytes);
    };
    put(l.nodes, t.nodes.data(), t.nodes.size() * sizeof(RoadNode));
    put(l.roads, t.roads.data(), t.roads.size() * sizeof(RoadRoad));
    put(l.lanes, t.lanes.data(), t.lanes.size() * sizeof(RoadLane));
    put(l.points, t.points.data(), t.points.size() * sizeof(RoadPoint));
    put(l.conflicts, t.conflicts.data(), t.conflicts.size() * sizeof(u32));
    put(l.signals, t.signals.data(), t.signals.size() * sizeof(RoadSignal));
    put(l.phases, t.phases.data(), t.phases.size() * sizeof(RoadPhase));
}

}  // namespace mce::asset
