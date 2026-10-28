// .mcroads -> CHNK_ROADS: a road network (M-OPENWORLD B1, ADR-0046). The source names junctions
// (nodes) and the roads between them; the cook builds every lane, every connector across every
// junction, who yields to whom and when each connector has green. Traffic (kit/traffic.h) and crowds
// drive on the result; nothing at run time re-derives the graph.
//
// {
//   "format": "mce-roads", "version": 1, "guid": "<uuid>", "name": "downtown",
//   "drive": "right",                  // or "left": the side traffic keeps to
//   "lane_width": 3.5, "speed": 13.9,  // defaults for every road: metres, m/s
//   "nodes": [
//     {"name": "a", "at": [0, 0, 0]},
//     {"name": "b", "at": [120, 0, 0], "control": "signal", "radius": 9}
//   ],
//   "roads": [
//     {"name": "main", "from": "a", "to": "b", "lanes": [2, 2], "points": [[60, 0, 10]],
//      "speed": 16, "lane_width": 3.5, "sidewalk": 2.5, "priority": 1}
//   ],
//   "signals": [
//     {"node": "b", "amber": 3, "all_red": 1, "offset": 0,
//      "phases": [{"green": ["main", "side"], "seconds": 20}, {"green": ["cross", "main>cross"], "seconds": 12}]}
//   ],
//   "no_turns": [{"node": "b", "from": "main", "to": "cross"}]
// }
//
// A road runs from its `from` node through its optional shape `points` to its `to` node. `lanes` is
// [forward, backward] (forward runs from -> to) or one number for that many each way. Lanes keep to
// the `drive` side; lane 0 is the innermost. Junction `control`: "none" (bends, dead ends), "priority"
// (the default where three or more roads meet: the roads with the highest priority go, the rest
// yield), "stop" (all-way stop) or "signal" (a `signals` entry; set by one). A junction's `radius` is
// how far back from it the lanes stop (default from the roads' widths and angles).
//
// Turns are made from the lanes a driver would use: straight on from lane i to the lane at the same
// place from the kerb, the turn towards the kerb from the outermost lane, the turn across traffic
// from the innermost lane; a lane left with no turn borrows its neighbour's. A dead end of a two-way
// road gets a U-turn. A signal phase's `green` lists roads ("main": every turn from it) or single
// turns ("main>cross"); every turn at a signalled junction must be green in some phase.

#include <cmath>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/roads_format.h"
#include "mce/string_id.h"

namespace mce::cook {

namespace {

using namespace asset;

constexpr const char* kStage = "cook.roads";
constexpr u32 kRoadsFormatVersion = 1;
constexpr u32 kBezierSegments = 8;
constexpr f32 kStraightDeg = 35.0f;
constexpr f32 kLateralAccel = 3.0f;  // m/s^2 a connector's speed allows in its tightest bend
constexpr f32 kPi = 3.14159265358979f;

struct V3 {
    f32 x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
f32 len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
f32 flat_len(V3 a) { return std::sqrt(a.x * a.x + a.z * a.z); }
V3 flat_dir(V3 a) {
    const f32 l = flat_len(a);
    return l > 1e-6f ? V3{a.x / l, 0.0f, a.z / l} : V3{0.0f, 0.0f, -1.0f};
}
f32 dot_xz(V3 a, V3 b) { return a.x * b.x + a.z * b.z; }
f32 cross_y(V3 a, V3 b) { return a.z * b.x - a.x * b.z; }  // < 0: b turns right of a (forward -Z, right +X)
V3 right_of(V3 d) { return {-d.z, 0.0f, d.x}; }
f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

struct SrcNode {
    StringView name;
    V3 at;
    RoadControl control = RoadControl::None;
    bool control_set = false;
    f32 radius = -1.0f;  // < 0: computed
    JsonValue where;
    // Incident roads: road index and whether the road's `from` end is here.
    Array<u32> roads;
    Array<u8> at_from;
    u32 signal = kRoadNone;
};

struct SrcRoad {
    StringView name;
    u32 from = 0, to = 0;
    u32 forward = 1, backward = 1;
    f32 speed = 0, lane_width = 0, sidewalk = 0;
    u32 priority = 0;
    Array<V3> line;  // untrimmed centreline from -> to
    JsonValue where;
};

struct Turn {
    u32 node, from_road, to_road;
};

bool read_vec3(JsonValue v, V3& out, StringView file, DiagList& diags) {
    if (!v.is_array() || v.size() != 3 || !v.at(0).is_number() || !v.at(1).is_number() || !v.at(2).is_number()) {
        detail::error_at(diags, Errc::ParseError, file, v.valid() ? v : JsonValue(), kStage, "expected [x, y, z]");
        return false;
    }
    out = {static_cast<f32>(v.at(0).as_number()), static_cast<f32>(v.at(1).as_number()), static_cast<f32>(v.at(2).as_number())};
    if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z)) {
        detail::error_at(diags, Errc::ParseError, file, v, kStage, "a position must be finite");
        return false;
    }
    return true;
}

bool read_name(JsonValue obj, StringView& out, StringView file, DiagList& diags) {
    const JsonValue v = obj.get("name");
    if (!v.is_string() || v.as_string().empty() || v.as_string().size() > 63 || v.as_string().find('>') != StringView::npos) {
        detail::error_at(diags, Errc::ParseError, file, v.valid() ? v : obj, kStage, "'name' must be a string of 1 to 63 bytes without '>'");
        return false;
    }
    out = v.as_string();
    return true;
}

// The point `d` metres along `line` (by arc length in 3D), with the index of the segment it lies on.
V3 point_at(const Array<V3>& line, f32 d, u32& seg) {
    f32 run = 0.0f;
    for (u32 i = 0; i + 1 < line.size(); ++i) {
        const f32 l = len(line[i + 1] - line[i]);
        if (run + l >= d || i + 2 == line.size()) {
            seg = i;
            const f32 t = l > 1e-6f ? clamp01((d - run) / l) : 0.0f;
            return line[i] + (line[i + 1] - line[i]) * t;
        }
        run += l;
    }
    seg = 0;
    return line[0];
}

f32 polyline_length(const Array<V3>& line) {
    f32 l = 0.0f;
    for (u32 i = 0; i + 1 < line.size(); ++i) l += len(line[i + 1] - line[i]);
    return l;
}

// `line` with `a` metres cut from its start and `b` from its end.
Array<V3> trimmed(const Array<V3>& line, f32 a, f32 b) {
    const f32 total = polyline_length(line);
    u32 sa = 0, sb = 0;
    const V3 pa = point_at(line, a, sa);
    const V3 pb = point_at(line, total - b, sb);
    Array<V3> out;
    out.push_back(pa);
    for (u32 i = sa + 1; i <= sb; ++i)
        if (len(line[i] - out.back()) > 1e-3f) out.push_back(line[i]);
    if (len(pb - out.back()) > 1e-3f || out.size() == 1) out.push_back(pb);
    return out;
}

// `line` moved `offset` metres to the right of its direction (flat), mitred at the bends.
Array<V3> offset_line(const Array<V3>& line, f32 offset) {
    Array<V3> out;
    const u32 n = static_cast<u32>(line.size());
    for (u32 i = 0; i < n; ++i) {
        const V3 din = flat_dir(line[i > 0 ? i : 1] - line[i > 0 ? i - 1 : 0]);
        const V3 dout = flat_dir(line[i + 1 < n ? i + 1 : i] - line[i + 1 < n ? i : i - 1]);
        const V3 rin = right_of(din), rout = right_of(dout);
        V3 r = flat_dir(rin + rout);
        if (flat_len(rin + rout) < 1e-4f) r = rout;
        const f32 c = dot_xz(r, rout);
        const f32 scale = c > 0.5f ? 1.0f / c : 2.0f;
        out.push_back(line[i] + r * (offset * scale));
    }
    return out;
}

// Appends `line` as RoadPoints with s from 0; returns its length.
f32 append_points(Array<RoadPoint>& points, const Array<V3>& line) {
    f32 s = 0.0f;
    for (u32 i = 0; i < line.size(); ++i) {
        if (i > 0) s += len(line[i] - line[i - 1]);
        points.push_back(RoadPoint{line[i].x, line[i].y, line[i].z, s});
    }
    return s;
}

// Proper crossing of segments ab and cd in the XZ plane (touching ends do not count).
bool segments_cross(V3 a, V3 b, V3 c, V3 d) {
    const auto orient = [](V3 p, V3 q, V3 r) { return (q.x - p.x) * (r.z - p.z) - (q.z - p.z) * (r.x - p.x); };
    const f32 o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
    const f32 e = 1e-5f;
    return ((o1 > e && o2 < -e) || (o1 < -e && o2 > e)) && ((o3 > e && o4 < -e) || (o3 < -e && o4 > e));
}

bool lines_cross(const RoadPoint* p, u32 np, const RoadPoint* q, u32 nq) {
    for (u32 i = 0; i + 1 < np; ++i)
        for (u32 j = 0; j + 1 < nq; ++j)
            if (segments_cross({p[i].x, 0, p[i].z}, {p[i + 1].x, 0, p[i + 1].z}, {q[j].x, 0, q[j].z}, {q[j + 1].x, 0, q[j + 1].z}))
                return true;
    return false;
}

class RoadsImporter final : public IImporter {
public:
    StringView id() const override { return "roads"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::RoadNetwork; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcroads"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        Array<byte> payload;
        if (!build(in.settings, in.display_path, payload, diags)) return false;
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::RoadNetwork;
        d.version = asset::chunk_version(asset::ROAD_NETWORK_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }

private:
    static bool build(JsonValue doc, StringView file, Array<byte>& payload, DiagList& diags);
};

u32 find_node(const Array<SrcNode>& nodes, StringView name) {
    for (u32 i = 0; i < nodes.size(); ++i)
        if (nodes[i].name == name) return i;
    return kRoadNone;
}

u32 find_road(const Array<SrcRoad>& roads, StringView name) {
    for (u32 i = 0; i < roads.size(); ++i)
        if (roads[i].name == name) return i;
    return kRoadNone;
}

void name_error(DiagList& diags, StringView file, JsonValue at, const char* what, StringView name) {
    String msg;
    appendf(msg, "no %s named '%.*s'", what, static_cast<int>(name.size()), name.data());
    detail::error_at(diags, Errc::ParseError, file, at, kStage, msg);
}

// The road's direction leaving `node` along it (flat).
V3 away_dir(const SrcRoad& r, bool from_end) {
    const u32 n = static_cast<u32>(r.line.size());
    return from_end ? flat_dir(r.line[1] - r.line[0]) : flat_dir(r.line[n - 2] - r.line[n - 1]);
}

f32 half_width(const SrcRoad& r) { return 0.5f * static_cast<f32>(r.forward + r.backward) * r.lane_width; }

bool RoadsImporter::build(JsonValue doc, StringView file, Array<byte>& payload, DiagList& diags) {
    const u32 errors_before = diags.error_count();
    if (!detail::check_keys(doc, {"format", "version", "guid", "name", "drive", "lane_width", "speed", "nodes", "roads", "signals", "no_turns"},
                            file, kStage, diags))
        return false;
    const JsonValue fmt = doc.get("format");
    if (!fmt.is_string() || fmt.as_string() != "mce-roads") {
        detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, "\"format\" must be \"mce-roads\"");
        return false;
    }
    if (!detail::check_format_version(doc, kRoadsFormatVersion, "roads", file, kStage, diags)) return false;
    RoadTables tables;
    RoadHeader& header = tables.header;
    if (const JsonValue name = doc.get("name"); name.valid()) {
        if (!name.is_string() || name.as_string().size() >= sizeof(header.name))
            detail::error_at(diags, Errc::ParseError, file, name, kStage, "'name' must be a string of at most 31 bytes");
        else std::memcpy(header.name, name.as_string().data(), name.as_string().size());
    }
    bool left = false;
    if (const JsonValue drive = doc.get("drive"); drive.valid()) {
        if (!drive.is_string() || (drive.as_string() != "right" && drive.as_string() != "left"))
            detail::error_at(diags, Errc::ParseError, file, drive, kStage, "'drive' must be \"right\" or \"left\"");
        else left = drive.as_string() == "left";
    }
    if (left) header.flags |= RoadFlags::LeftHand;
    const f32 side = left ? -1.0f : 1.0f;  // forward lanes lie this way of the centreline's right
    f64 def_width = 3.5, def_speed = 13.9;
    detail::read_number(doc, "lane_width", 3.5, def_width, file, kStage, diags);
    detail::read_number(doc, "speed", 13.9, def_speed, file, kStage, diags);
    if (!(def_width > 0.5 && def_width <= 20.0)) detail::error_at(diags, Errc::ParseError, file, doc.get("lane_width"), kStage, "'lane_width' must be 0.5 to 20 metres");
    if (!(def_speed > 0.0 && def_speed <= 150.0)) detail::error_at(diags, Errc::ParseError, file, doc.get("speed"), kStage, "'speed' must be above 0 and at most 150 m/s");

    // ---- nodes
    Array<SrcNode> nodes;
    const JsonValue jnodes = doc.get("nodes");
    if (!jnodes.is_array() || jnodes.size() == 0 || jnodes.size() > kRoadMaxNodes) {
        detail::error_at(diags, Errc::ParseError, file, jnodes.valid() ? jnodes : doc, kStage, "'nodes' must be an array of 1 to 65535 nodes");
        return false;
    }
    for (u32 i = 0; i < jnodes.size(); ++i) {
        const JsonValue jn = jnodes.at(i);
        if (!jn.is_object()) {
            detail::error_at(diags, Errc::ParseError, file, jn, kStage, "a node must be an object");
            continue;
        }
        if (!detail::check_keys(jn, {"name", "at", "control", "radius"}, file, kStage, diags)) continue;
        SrcNode n;
        n.where = jn;
        if (!read_name(jn, n.name, file, diags) || !read_vec3(jn.get("at"), n.at, file, diags)) continue;
        if (find_node(nodes, n.name) != kRoadNone) {
            detail::error_at(diags, Errc::ParseError, file, jn.get("name"), kStage, "two nodes share this name");
            continue;
        }
        if (const JsonValue c = jn.get("control"); c.valid()) {
            const StringView s = c.is_string() ? c.as_string() : StringView();
            n.control_set = true;
            if (s == "none") n.control = RoadControl::None;
            else if (s == "priority") n.control = RoadControl::Priority;
            else if (s == "stop") n.control = RoadControl::Stop;
            else if (s == "signal") n.control = RoadControl::Signal;
            else detail::error_at(diags, Errc::ParseError, file, c, kStage, "'control' must be \"none\", \"priority\", \"stop\" or \"signal\"");
        }
        if (const JsonValue r = jn.get("radius"); r.valid()) {
            if (!r.is_number() || !(r.as_number() >= 0.0 && r.as_number() <= 200.0))
                detail::error_at(diags, Errc::ParseError, file, r, kStage, "'radius' must be 0 to 200 metres");
            else n.radius = static_cast<f32>(r.as_number());
        }
        nodes.push_back(static_cast<SrcNode&&>(n));
    }

    // ---- roads
    Array<SrcRoad> roads;
    const JsonValue jroads = doc.get("roads");
    if (!jroads.is_array() || jroads.size() == 0 || jroads.size() >= kRoadNone16) {
        detail::error_at(diags, Errc::ParseError, file, jroads.valid() ? jroads : doc, kStage, "'roads' must be an array of 1 to 65534 roads");
        return false;
    }
    for (u32 i = 0; i < jroads.size(); ++i) {
        const JsonValue jr = jroads.at(i);
        if (!jr.is_object()) {
            detail::error_at(diags, Errc::ParseError, file, jr, kStage, "a road must be an object");
            continue;
        }
        if (!detail::check_keys(jr, {"name", "from", "to", "lanes", "points", "speed", "lane_width", "sidewalk", "priority"}, file, kStage, diags))
            continue;
        SrcRoad r;
        r.where = jr;
        if (!read_name(jr, r.name, file, diags)) continue;
        if (find_road(roads, r.name) != kRoadNone) {
            detail::error_at(diags, Errc::ParseError, file, jr.get("name"), kStage, "two roads share this name");
            continue;
        }
        const JsonValue jf = jr.get("from"), jt = jr.get("to");
        r.from = jf.is_string() ? find_node(nodes, jf.as_string()) : kRoadNone;
        r.to = jt.is_string() ? find_node(nodes, jt.as_string()) : kRoadNone;
        if (r.from == kRoadNone) {
            name_error(diags, file, jf.valid() ? jf : jr, "node", jf.is_string() ? jf.as_string() : StringView("(from)"));
            continue;
        }
        if (r.to == kRoadNone) {
            name_error(diags, file, jt.valid() ? jt : jr, "node", jt.is_string() ? jt.as_string() : StringView("(to)"));
            continue;
        }
        if (r.from == r.to) {
            detail::error_at(diags, Errc::ParseError, file, jt, kStage, "a road must join two different nodes (split a loop with a node)");
            continue;
        }
        if (const JsonValue jl = jr.get("lanes"); jl.valid()) {
            if (jl.is_integer()) {
                r.forward = r.backward = static_cast<u32>(jl.as_number() < 0 ? 99 : jl.as_number());
            } else if (jl.is_array() && jl.size() == 2 && jl.at(0).is_integer() && jl.at(1).is_integer() && jl.at(0).as_number() >= 0 &&
                       jl.at(1).as_number() >= 0) {
                r.forward = static_cast<u32>(std::fmin(jl.at(0).as_number(), 99.0));
                r.backward = static_cast<u32>(std::fmin(jl.at(1).as_number(), 99.0));
            } else {
                r.forward = 99;
            }
            if (r.forward > kRoadMaxLanesPerSide || r.backward > kRoadMaxLanesPerSide || r.forward + r.backward == 0) {
                detail::error_at(diags, Errc::ParseError, file, jl, kStage, "'lanes' must be n (each way) or [forward, backward], 0 to 8 each, not both 0");
                continue;
            }
        }
        f64 speed = def_speed, width = def_width, sidewalk = 0.0, priority = 0.0;
        detail::read_number(jr, "speed", def_speed, speed, file, kStage, diags);
        detail::read_number(jr, "lane_width", def_width, width, file, kStage, diags);
        detail::read_number(jr, "sidewalk", 0.0, sidewalk, file, kStage, diags);
        detail::read_number(jr, "priority", 0.0, priority, file, kStage, diags);
        if (!(speed > 0.0 && speed <= 150.0)) detail::error_at(diags, Errc::ParseError, file, jr.get("speed"), kStage, "'speed' must be above 0 and at most 150 m/s");
        if (!(width > 0.5 && width <= 20.0)) detail::error_at(diags, Errc::ParseError, file, jr.get("lane_width"), kStage, "'lane_width' must be 0.5 to 20 metres");
        if (!(sidewalk >= 0.0 && sidewalk <= 20.0)) detail::error_at(diags, Errc::ParseError, file, jr.get("sidewalk"), kStage, "'sidewalk' must be 0 to 20 metres");
        if (!(priority >= 0.0 && priority <= 15.0 && priority == std::floor(priority)))
            detail::error_at(diags, Errc::ParseError, file, jr.get("priority"), kStage, "'priority' must be an integer 0 to 15");
        r.speed = static_cast<f32>(speed);
        r.lane_width = static_cast<f32>(width);
        r.sidewalk = static_cast<f32>(sidewalk);
        r.priority = static_cast<u32>(priority);
        r.line.push_back(nodes[r.from].at);
        if (const JsonValue jp = jr.get("points"); jp.valid()) {
            if (!jp.is_array() || jp.size() > 4096) {
                detail::error_at(diags, Errc::ParseError, file, jp, kStage, "'points' must be an array of up to 4096 [x, y, z]");
                continue;
            }
            for (u32 k = 0; k < jp.size(); ++k) {
                V3 p;
                if (!read_vec3(jp.at(k), p, file, diags)) continue;
                if (len(p - r.line.back()) > 1e-3f) r.line.push_back(p);
            }
        }
        if (len(nodes[r.to].at - r.line.back()) > 1e-3f) r.line.push_back(nodes[r.to].at);
        else r.line.back() = nodes[r.to].at;
        if (r.line.size() < 2 || flat_len(r.line.back() - r.line[0]) < 1e-3f) {
            detail::error_at(diags, Errc::ParseError, file, jr, kStage, "a road must be longer than 0 across the ground");
            continue;
        }
        for (u32 k = 0; k + 1 < r.line.size(); ++k)
            if (flat_len(r.line[k + 1] - r.line[k]) < 1e-3f) {
                detail::error_at(diags, Errc::ParseError, file, jr, kStage, "a road may not run straight up (two points above each other)");
                break;
            }
        const u32 index = static_cast<u32>(roads.size());
        SrcNode& nf = nodes[r.from];
        SrcNode& nt = nodes[r.to];
        if (nf.roads.size() >= kRoadMaxRoadsPerNode || nt.roads.size() >= kRoadMaxRoadsPerNode) {
            detail::error_at(diags, Errc::ParseError, file, jr, kStage, "at most 16 roads meet at a node");
            continue;
        }
        nf.roads.push_back(index);
        nf.at_from.push_back(1);
        nt.roads.push_back(index);
        nt.at_from.push_back(0);
        roads.push_back(static_cast<SrcRoad&&>(r));
    }

    // ---- no_turns
    Array<Turn> no_turns;
    if (const JsonValue jt = doc.get("no_turns"); jt.valid()) {
        if (!jt.is_array()) detail::error_at(diags, Errc::ParseError, file, jt, kStage, "'no_turns' must be an array");
        for (u32 i = 0; jt.is_array() && i < jt.size(); ++i) {
            const JsonValue t = jt.at(i);
            if (!t.is_object() || !detail::check_keys(t, {"node", "from", "to"}, file, kStage, diags)) {
                if (!t.is_object()) detail::error_at(diags, Errc::ParseError, file, t, kStage, "a no_turns entry is {\"node\", \"from\", \"to\"}");
                continue;
            }
            const JsonValue n = t.get("node"), f = t.get("from"), o = t.get("to");
            Turn turn{n.is_string() ? find_node(nodes, n.as_string()) : kRoadNone, f.is_string() ? find_road(roads, f.as_string()) : kRoadNone,
                      o.is_string() ? find_road(roads, o.as_string()) : kRoadNone};
            if (turn.node == kRoadNone || turn.from_road == kRoadNone || turn.to_road == kRoadNone) {
                detail::error_at(diags, Errc::ParseError, file, t, kStage, "a no_turns entry names an unknown node or road");
                continue;
            }
            no_turns.push_back(turn);
        }
    }

    // ---- signals (parsed against the nodes; their turns are resolved after the connectors exist)
    const JsonValue jsignals = doc.get("signals");
    if (jsignals.valid() && (!jsignals.is_array() || jsignals.size() > kRoadMaxNodes)) {
        detail::error_at(diags, Errc::ParseError, file, jsignals, kStage, "'signals' must be an array");
        return false;
    }
    for (u32 i = 0; jsignals.valid() && i < jsignals.size(); ++i) {
        const JsonValue js = jsignals.at(i);
        if (!js.is_object() || !detail::check_keys(js, {"node", "phases", "amber", "all_red", "offset"}, file, kStage, diags)) {
            if (!js.is_object()) detail::error_at(diags, Errc::ParseError, file, js, kStage, "a signal must be an object");
            continue;
        }
        const JsonValue jn = js.get("node");
        const u32 n = jn.is_string() ? find_node(nodes, jn.as_string()) : kRoadNone;
        if (n == kRoadNone) {
            name_error(diags, file, jn.valid() ? jn : js, "node", jn.is_string() ? jn.as_string() : StringView("(node)"));
            continue;
        }
        if (nodes[n].signal != kRoadNone) {
            detail::error_at(diags, Errc::ParseError, file, jn, kStage, "this node already has a signal");
            continue;
        }
        if (nodes[n].control_set && nodes[n].control != RoadControl::Signal) {
            detail::error_at(diags, Errc::ParseError, file, jn, kStage, "a signal's node must have control \"signal\" (or none set)");
            continue;
        }
        nodes[n].signal = i;
        nodes[n].control = RoadControl::Signal;
        nodes[n].control_set = true;
    }
    for (const SrcNode& n : nodes)
        if (n.control == RoadControl::Signal && n.signal == kRoadNone)
            detail::error_at(diags, Errc::ParseError, file, n.where, kStage, "a node with control \"signal\" needs an entry in 'signals'");
    if (diags.error_count() != errors_before) return false;

    // ---- junction radii and default controls
    Array<f32> radius;
    for (u32 ni = 0; ni < nodes.size(); ++ni) {
        SrcNode& n = nodes[ni];
        const u32 count = static_cast<u32>(n.roads.size());
        if (!n.control_set) n.control = count >= 3 ? RoadControl::Priority : RoadControl::None;
        f32 r = 0.0f;
        if (n.radius >= 0.0f) {
            r = n.radius;
        } else if (count >= 2) {
            f32 widest = 0.0f, min_sin = 1.0f;
            bool bent = false;
            for (u32 a = 0; a < count; ++a) {
                widest = std::fmax(widest, half_width(roads[n.roads[a]]));
                for (u32 b = a + 1; b < count; ++b) {
                    const V3 da = away_dir(roads[n.roads[a]], n.at_from[a] != 0), db = away_dir(roads[n.roads[b]], n.at_from[b] != 0);
                    const f32 angle = std::acos(std::fmax(-1.0f, std::fmin(1.0f, dot_xz(da, db))));
                    if (angle < 150.0f * kPi / 180.0f) {  // the two roads overlap near the node
                        bent = true;
                        min_sin = std::fmin(min_sin, std::fmax(std::sin(angle), 0.35f));
                    }
                }
            }
            r = bent || count >= 3 ? widest / min_sin + 1.0f : 0.5f;
        }
        radius.push_back(r);
    }

    // ---- road lanes
    Array<RoadRoad> out_roads;
    Array<RoadLane> lanes;
    Array<RoadPoint> points;
    Array<Array<V3>> lane_lines;  // the road lanes' polylines, for the connectors' ends
    for (u32 ri = 0; ri < roads.size(); ++ri) {
        const SrcRoad& r = roads[ri];
        const f32 total = polyline_length(r.line);
        const f32 ta = radius[r.from], tb = radius[r.to];
        if (total < ta + tb + 0.5f) {
            String msg;
            appendf(msg, "road '%.*s' is %.1f m long; its junctions need %.1f m of it (move the nodes apart or set a smaller 'radius')",
                    static_cast<int>(r.name.size()), r.name.data(), f64(total), f64(ta + tb + 0.5f));
            detail::error_at(diags, Errc::ParseError, file, r.where, kStage, msg);
            continue;
        }
        RoadRoad rr{};
        rr.name = StringId(r.name).value;
        rr.from = static_cast<u16>(r.from);
        rr.to = static_cast<u16>(r.to);
        rr.forward = static_cast<u8>(r.forward);
        rr.backward = static_cast<u8>(r.backward);
        rr.priority = static_cast<u8>(r.priority);
        rr.lane_width = r.lane_width;
        rr.speed = r.speed;
        rr.sidewalk = r.sidewalk;
        rr.first_point = static_cast<u32>(points.size());
        rr.point_count = static_cast<u32>(r.line.size());
        rr.length = append_points(points, r.line);
        rr.first_lane = static_cast<u32>(lanes.size());
        out_roads.push_back(rr);
        const Array<V3> core = trimmed(r.line, ta, tb);
        Array<V3> back;
        for (u32 k = static_cast<u32>(core.size()); k-- > 0;) back.push_back(core[k]);
        const f32 w = r.lane_width;
        for (u32 dir = 0; dir < 2; ++dir) {
            const u32 count = dir == 0 ? r.forward : r.backward;
            const u32 other = dir == 0 ? r.backward : r.forward;
            for (u32 i = 0; i < count; ++i) {
                const f32 across = other > 0 ? (static_cast<f32>(i) + 0.5f) * w : (static_cast<f32>(i) + 0.5f) * w - 0.5f * static_cast<f32>(count) * w;
                Array<V3> line = offset_line(dir == 0 ? core : back, side * across);
                RoadLane ln{};
                ln.first_point = static_cast<u32>(points.size());
                ln.point_count = static_cast<u32>(line.size());
                ln.length = append_points(points, line);
                ln.speed = r.speed;
                ln.width = w;
                ln.prev = kRoadNone;
                ln.road = static_cast<u16>(ri);
                ln.node = static_cast<u16>(dir == 0 ? r.to : r.from);
                ln.kind = RoadLaneKind::Road;
                ln.index = static_cast<u8>(i);
                ln.flags = dir == 0 ? 0 : RoadLaneFlags::Backward;
                lanes.push_back(ln);
                lane_lines.push_back(static_cast<Array<V3>&&>(line));
            }
        }
    }
    if (diags.error_count() != errors_before) return false;
    const u32 road_lane_count = static_cast<u32>(lanes.size());

    // The lanes of road `ri` that end (incoming) or start (outgoing) at node `ni`, innermost first.
    const auto lanes_at = [&](u32 ri, u32 ni, bool incoming, Array<u32>& out) {
        out.clear();
        const RoadRoad& rr = out_roads[ri];
        // Forward lanes end at `to` and start at `from`.
        const bool fwd = incoming ? rr.to == ni : rr.from == ni;
        const u32 first = rr.first_lane + (fwd ? 0u : rr.forward);
        const u32 count = fwd ? rr.forward : rr.backward;
        for (u32 i = 0; i < count; ++i) out.push_back(first + i);
    };
    const auto lane_dir_end = [&](u32 lane) {
        const Array<V3>& l = lane_lines[lane];
        return flat_dir(l[l.size() - 1] - l[l.size() - 2]);
    };
    const auto lane_dir_start = [&](u32 lane) {
        const Array<V3>& l = lane_lines[lane];
        return flat_dir(l[1] - l[0]);
    };
    const auto forbidden = [&](u32 ni, u32 a, u32 b) {
        for (const Turn& t : no_turns)
            if (t.node == ni && t.from_road == a && t.to_road == b) return true;
        return false;
    };

    // ---- connectors, junction by junction, source lane by source lane
    Array<RoadNode> out_nodes;
    Array<u32> conflicts;
    struct Pending {
        u32 to;
        RoadTurn turn;
    };
    for (u32 ni = 0; ni < nodes.size(); ++ni) {
        const SrcNode& n = nodes[ni];
        RoadNode rn{};
        rn.position[0] = n.at.x;
        rn.position[1] = n.at.y;
        rn.position[2] = n.at.z;
        rn.control = n.control;
        rn.road_count = static_cast<u8>(n.roads.size());
        rn.signal = kRoadNone16;
        rn.name = StringId(n.name).value;
        rn.first_connector = static_cast<u32>(lanes.size());
        // The highest priority among the roads that bring traffic in.
        u32 top = 0;
        Array<u32> in_lanes, out_lanes;
        for (u32 a = 0; a < n.roads.size(); ++a) {
            lanes_at(n.roads[a], ni, true, in_lanes);
            if (!in_lanes.empty()) top = top > roads[n.roads[a]].priority ? top : roads[n.roads[a]].priority;
        }
        for (u32 a = 0; a < n.roads.size(); ++a) {
            const u32 ra = n.roads[a];
            if (a > 0 && n.roads[a] == n.roads[a - 1]) continue;  // (a road meets a node once: from != to)
            lanes_at(ra, ni, true, in_lanes);
            const u32 nin = static_cast<u32>(in_lanes.size());
            if (nin == 0) continue;
            Array<Array<Pending>> per;
            per.resize(nin);
            for (u32 b = 0; b < n.roads.size(); ++b) {
                const u32 rb = n.roads[b];
                const bool uturn = rb == ra;
                if (uturn && n.roads.size() != 1) continue;  // U-turns only at dead ends
                if (forbidden(ni, ra, rb)) continue;
                lanes_at(rb, ni, false, out_lanes);
                const u32 nout = static_cast<u32>(out_lanes.size());
                if (nout == 0) continue;
                const V3 din = lane_dir_end(in_lanes[0]), dout = lane_dir_start(out_lanes[0]);
                const f32 angle = std::atan2(cross_y(din, dout), dot_xz(din, dout)) * 180.0f / kPi;
                RoadTurn turn = RoadTurn::Straight;
                if (uturn) turn = RoadTurn::UTurn;
                else if (std::fabs(angle) > kStraightDeg) turn = angle < 0.0f ? RoadTurn::Right : RoadTurn::Left;
                const bool kerb = (turn == RoadTurn::Right && !left) || (turn == RoadTurn::Left && left);
                if (turn == RoadTurn::Straight) {
                    for (u32 i = 0; i < nin; ++i) {
                        const u32 k_in = nin - 1 - i;  // from the kerb
                        const u32 o = nout - 1 - (k_in < nout - 1 ? k_in : nout - 1);
                        per[i].push_back(Pending{out_lanes[o], turn});
                        if (i == 0)
                            for (u32 extra = 0; extra + nin < nout && extra < o; ++extra) per[0].push_back(Pending{out_lanes[extra], turn});
                    }
                } else if (kerb) {
                    per[nin - 1].push_back(Pending{out_lanes[nout - 1], turn});
                } else {
                    per[0].push_back(Pending{out_lanes[0], turn});
                }
            }
            // A lane with no turn borrows the nearest lane's turns.
            for (u32 i = 0; i < nin; ++i) {
                if (!per[i].empty()) continue;
                for (u32 step = 1; step < nin; ++step) {
                    const u32 cand[2] = {i >= step ? i - step : kRoadNone, i + step < nin ? i + step : kRoadNone};
                    for (u32 c : cand)
                        if (c != kRoadNone && per[i].empty() && !per[c].empty())
                            for (const Pending& p : per[c]) per[i].push_back(p);
                    if (!per[i].empty()) break;
                }
            }
            for (u32 i = 0; i < nin; ++i) {
                // By value: the connectors pushed below grow `lanes`, which moves its elements.
                lanes[in_lanes[i]].first_next = static_cast<u32>(lanes.size());
                lanes[in_lanes[i]].next_count = static_cast<u32>(per[i].size());
                const RoadLane src = lanes[in_lanes[i]];
                for (const Pending& p : per[i]) {
                    const Array<V3>& li = lane_lines[in_lanes[i]];
                    const Array<V3>& lo = lane_lines[p.to];
                    const V3 p0 = li[li.size() - 1], p3 = lo[0];
                    const V3 d0 = li[li.size() - 1] - li[li.size() - 2], d3 = lo[1] - lo[0];
                    const f32 dist = len(p3 - p0);
                    const f32 handle = p.turn == RoadTurn::UTurn ? std::fmax(2.0f * dist, 6.0f)
                                       : p.turn == RoadTurn::Straight ? dist / 3.0f
                                                                      : 0.55f * dist;
                    const V3 c1 = p0 + d0 * (handle / std::fmax(len(d0), 1e-6f));
                    const V3 c2 = p3 - d3 * (handle / std::fmax(len(d3), 1e-6f));
                    Array<V3> curve;
                    for (u32 k = 0; k <= kBezierSegments; ++k) {
                        const f32 t = static_cast<f32>(k) / static_cast<f32>(kBezierSegments), u = 1.0f - t;
                        curve.push_back(p0 * (u * u * u) + c1 * (3.0f * u * u * t) + c2 * (3.0f * u * t * t) + p3 * (t * t * t));
                    }
                    // The tightest bend: the circle through three consecutive points.
                    f32 tight = 1e9f;
                    for (u32 k = 1; k + 1 < curve.size(); ++k) {
                        const V3 q0 = curve[k - 1], q1 = curve[k], q2 = curve[k + 1];
                        const f32 l01 = flat_len(q1 - q0), l12 = flat_len(q2 - q1), l20 = flat_len(q0 - q2);
                        const f32 area2 = std::fabs(cross_y(q1 - q0, q2 - q0));
                        if (area2 > 1e-6f) tight = std::fmin(tight, l01 * l12 * l20 / (2.0f * area2));
                    }
                    const RoadLane dst = lanes[p.to];
                    RoadLane c{};
                    c.first_point = static_cast<u32>(points.size());
                    c.point_count = static_cast<u32>(curve.size());
                    c.length = append_points(points, curve);
                    c.speed = std::fmax(2.0f, std::fmin(std::fmin(src.speed, dst.speed), std::sqrt(kLateralAccel * tight)));
                    c.width = std::fmin(src.width, dst.width);
                    c.first_next = p.to;
                    c.next_count = 1;
                    c.prev = in_lanes[i];
                    c.road = kRoadNone16;
                    c.node = static_cast<u16>(ni);
                    c.kind = RoadLaneKind::Connector;
                    c.index = static_cast<u8>(p.turn);
                    const bool kerb = (p.turn == RoadTurn::Right && !left) || (p.turn == RoadTurn::Left && left);
                    const u8 turn_rank = p.turn == RoadTurn::Straight ? 3 : kerb ? 2 : p.turn == RoadTurn::UTurn ? 0 : 1;
                    const bool major = roads[ra].priority >= top;
                    switch (n.control) {
                    case RoadControl::None: c.rank = turn_rank; break;
                    case RoadControl::Priority:
                        c.rank = static_cast<u8>((major ? 4 : 0) + turn_rank);
                        if (!major) c.flags |= RoadLaneFlags::Yield;
                        break;
                    case RoadControl::Stop:
                        c.rank = turn_rank;
                        c.flags |= RoadLaneFlags::Yield | RoadLaneFlags::Stop;
                        break;
                    case RoadControl::Signal: c.rank = turn_rank; break;
                    }
                    lanes.push_back(c);
                    lane_lines.push_back(static_cast<Array<V3>&&>(curve));
                }
            }
        }
        rn.connector_count = static_cast<u32>(lanes.size()) - rn.first_connector;
        if (lanes.size() >= kRoadMaxItems) {
            detail::error_at(diags, Errc::ParseError, file, n.where, kStage, "the network has too many lanes");
            return false;
        }
        // Conflicts: connectors from different lanes that merge into one lane or cross.
        for (u32 c = rn.first_connector; c < lanes.size(); ++c) {
            RoadLane& lc = lanes[c];
            lc.first_conflict = static_cast<u32>(conflicts.size());
            for (u32 k = rn.first_connector; k < lanes.size(); ++k) {
                const RoadLane& lk = lanes[k];
                if (k == c || lk.prev == lc.prev) continue;
                if (lk.first_next == lc.first_next ||
                    lines_cross(points.data() + lc.first_point, lc.point_count, points.data() + lk.first_point, lk.point_count))
                    conflicts.push_back(k);
            }
            lc.conflict_count = static_cast<u32>(conflicts.size()) - lc.first_conflict;
        }
        out_nodes.push_back(rn);
    }

    // ---- signals: phases and each connector's green bits
    Array<RoadSignal> signals;
    Array<RoadPhase> phases;
    for (u32 si = 0; jsignals.valid() && si < jsignals.size(); ++si) {
        const JsonValue js = jsignals.at(si);
        const u32 ni = find_node(nodes, js.get("node").as_string());
        RoadNode& rn = out_nodes[ni];
        rn.signal = static_cast<u16>(signals.size());
        RoadSignal sig{};
        sig.node = static_cast<u16>(ni);
        f64 amber = 3.0, all_red = 1.0, offset = 0.0;
        detail::read_number(js, "amber", 3.0, amber, file, kStage, diags);
        detail::read_number(js, "all_red", 1.0, all_red, file, kStage, diags);
        detail::read_number(js, "offset", 0.0, offset, file, kStage, diags);
        if (!(amber >= 0.0 && amber <= 30.0)) detail::error_at(diags, Errc::ParseError, file, js.get("amber"), kStage, "'amber' must be 0 to 30 s");
        if (!(all_red >= 0.0 && all_red <= 30.0)) detail::error_at(diags, Errc::ParseError, file, js.get("all_red"), kStage, "'all_red' must be 0 to 30 s");
        if (!std::isfinite(offset)) detail::error_at(diags, Errc::ParseError, file, js.get("offset"), kStage, "'offset' must be a number");
        sig.amber = static_cast<f32>(amber);
        sig.all_red = static_cast<f32>(all_red);
        sig.first_phase = static_cast<u32>(phases.size());
        const JsonValue jp = js.get("phases");
        if (!jp.is_array() || jp.size() == 0 || jp.size() > kRoadMaxPhases) {
            detail::error_at(diags, Errc::ParseError, file, jp.valid() ? jp : js, kStage, "'phases' must be an array of 1 to 16 phases");
            continue;
        }
        f32 start = 0.0f;
        for (u32 p = 0; p < jp.size(); ++p) {
            const JsonValue ph = jp.at(p);
            if (!ph.is_object() || !detail::check_keys(ph, {"green", "seconds"}, file, kStage, diags)) {
                if (!ph.is_object()) detail::error_at(diags, Errc::ParseError, file, ph, kStage, "a phase is {\"green\": [...], \"seconds\": s}");
                continue;
            }
            f64 seconds = 0.0;
            detail::read_number(ph, "seconds", 0.0, seconds, file, kStage, diags);
            if (!(seconds > 0.0 && seconds <= 600.0)) {
                detail::error_at(diags, Errc::ParseError, file, ph.get("seconds").valid() ? ph.get("seconds") : ph, kStage,
                                 "'seconds' must be above 0 and at most 600");
                continue;
            }
            const JsonValue green = ph.get("green");
            if (!green.is_array() || green.size() == 0) {
                detail::error_at(diags, Errc::ParseError, file, green.valid() ? green : ph, kStage, "'green' must be an array of road names or \"road>road\" turns");
                continue;
            }
            for (u32 g = 0; g < green.size(); ++g) {
                const JsonValue e = green.at(g);
                const StringView s = e.is_string() ? e.as_string() : StringView();
                const usize gt = s.find('>');
                const StringView from_name = gt == StringView::npos ? s : s.substr(0, gt);
                const StringView to_name = gt == StringView::npos ? StringView() : s.substr(gt + 1);
                const u32 ra = find_road(roads, from_name);
                const u32 rb = to_name.empty() ? kRoadNone : find_road(roads, to_name);
                if (ra == kRoadNone || (!to_name.empty() && rb == kRoadNone)) {
                    detail::error_at(diags, Errc::ParseError, file, e, kStage, "a green entry is a road's name or \"from>to\" of two roads");
                    continue;
                }
                u32 matched = 0;
                for (u32 c = rn.first_connector; c < rn.first_connector + rn.connector_count; ++c) {
                    RoadLane& lc = lanes[c];
                    if (lanes[lc.prev].road != ra) continue;
                    if (rb != kRoadNone && lanes[lc.first_next].road != rb) continue;
                    lc.green |= 1u << p;
                    ++matched;
                }
                if (matched == 0) detail::error_at(diags, Errc::ParseError, file, e, kStage, "no turn at this node matches this green entry");
            }
            phases.push_back(RoadPhase{static_cast<f32>(seconds), start});
            start += static_cast<f32>(seconds) + sig.amber + sig.all_red;
        }
        sig.phase_count = static_cast<u16>(phases.size() - sig.first_phase);
        sig.cycle = start;
        sig.offset = static_cast<f32>(offset);
        for (u32 c = rn.first_connector; c < rn.first_connector + rn.connector_count; ++c)
            if (lanes[c].green == 0) {
                const SrcRoad& sa = roads[lanes[lanes[c].prev].road];
                const SrcRoad& sb = roads[lanes[lanes[c].first_next].road];
                String msg;
                appendf(msg, "the turn \"%.*s>%.*s\" at this signal is green in no phase", static_cast<int>(sa.name.size()), sa.name.data(),
                        static_cast<int>(sb.name.size()), sb.name.data());
                detail::error_at(diags, Errc::ParseError, file, js, kStage, msg);
                break;
            }
        signals.push_back(sig);
    }
    if (diags.error_count() != errors_before) return false;

    // ---- bounds
    f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    const auto grow = [&](f32 x, f32 y, f32 z) {
        const f32 v[3] = {x, y, z};
        for (u32 i = 0; i < 3; ++i) {
            lo[i] = std::fmin(lo[i], v[i]);
            hi[i] = std::fmax(hi[i], v[i]);
        }
    };
    for (const RoadPoint& p : points) grow(p.x, p.y, p.z);
    for (const RoadNode& n : out_nodes) grow(n.position[0], n.position[1], n.position[2]);
    std::memcpy(header.bounds_min, lo, sizeof(lo));
    std::memcpy(header.bounds_max, hi, sizeof(hi));
    (void)road_lane_count;

    tables.nodes = out_nodes.span();
    tables.roads = out_roads.span();
    tables.lanes = lanes.span();
    tables.points = points.span();
    tables.conflicts = conflicts.span();
    tables.signals = signals.span();
    tables.phases = phases.span();
    write_road_network(tables, payload);
    return true;
}

}  // namespace

mem::UniquePtr<IImporter> make_roads_importer() { return mem::make_unique<RoadsImporter>(mem::heap()); }

}  // namespace mce::cook
