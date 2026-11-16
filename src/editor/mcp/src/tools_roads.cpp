// The road network tools (M-OPENWORLD B1, ADR-0046): roads_info cooks a .mcroads and reports what the
// cook built (lanes, connectors, conflicts, signals) and what traffic would trip on: one-way dead ends
// and lanes cut off from the rest of the network. roads_generate writes a street grid to start a city
// from; both go through the same cook the project uses, so what they report is what ships.

#include <algorithm>
#include <cmath>

#include "mce/asset/container.h"
#include "mce/asset/roads_format.h"
#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

using asset::kRoadNone;
using asset::RoadLane;
using asset::RoadLaneKind;
using asset::RoadView;

constexpr u32 kMaxListed = 64;  // nodes, cut-off lanes and dead ends listed by name
constexpr u32 kMaxGrid = 32;    // roads_generate: junctions along a side

bool is_mcroads(StringView rel) { return rel.size() > 8 && rel.substr(rel.size() - 8) == ".mcroads"; }

const char* control_name(asset::RoadControl c) {
    switch (c) {
        case asset::RoadControl::None: return "none";
        case asset::RoadControl::Priority: return "priority";
        case asset::RoadControl::Stop: return "stop";
        case asset::RoadControl::Signal: return "signal";
    }
    return "none";
}

// The source's node and road names by index (the cook keeps the source order; the chunk holds hashes).
void source_names(const cook::JsonValue& doc, const char* list, Array<cook::String>& out) {
    const cook::JsonValue a = doc.get(list);
    for (u32 i = 0; a.is_array() && i < a.size(); ++i) {
        const cook::JsonValue n = a.at(i).get("name");
        out.push_back(cook::String(n.is_string() ? n.as_string() : StringView()));
    }
}

// Strongly connected components of the lane graph (lane -> its successors), iterative Kosaraju.
// comp[i] is lane i's component; returns the index of the largest.
u32 lane_components(const RoadView& v, Array<u32>& comp) {
    const u32 n = static_cast<u32>(v.lanes.size());
    Array<u32> order, stack, it;
    order.reserve(n);
    Array<u8> seen;
    seen.resize(n);
    std::fill(seen.begin(), seen.end(), u8(0));
    it.resize(n);
    for (u32 s = 0; s < n; ++s) {
        if (seen[s]) continue;
        seen[s] = 1;
        stack.push_back(s);
        it[s] = 0;
        while (!stack.empty()) {
            const u32 u = stack.back();
            const RoadLane& l = v.lanes[u];
            if (it[u] < l.next_count) {
                const u32 w = l.first_next + it[u]++;
                if (!seen[w]) {
                    seen[w] = 1;
                    it[w] = 0;
                    stack.push_back(w);
                }
            } else {
                order.push_back(u);
                stack.pop_back();
            }
        }
    }
    // The reversed graph: predecessors in a flat table.
    Array<u32> rstart, rcount, redges;
    rstart.resize(n + 1);
    rcount.resize(n);
    std::fill(rcount.begin(), rcount.end(), 0u);
    for (u32 u = 0; u < n; ++u)
        for (u32 k = 0; k < v.lanes[u].next_count; ++k) ++rcount[v.lanes[u].first_next + k];
    rstart[0] = 0;
    for (u32 u = 0; u < n; ++u) rstart[u + 1] = rstart[u] + rcount[u];
    redges.resize(rstart[n]);
    std::fill(rcount.begin(), rcount.end(), 0u);
    for (u32 u = 0; u < n; ++u)
        for (u32 k = 0; k < v.lanes[u].next_count; ++k) {
            const u32 w = v.lanes[u].first_next + k;
            redges[rstart[w] + rcount[w]++] = u;
        }
    comp.resize(n);
    std::fill(comp.begin(), comp.end(), kRoadNone);
    Array<u32> sizes;
    for (u32 oi = n; oi-- > 0;) {
        const u32 s = order[oi];
        if (comp[s] != kRoadNone) continue;
        const u32 id = static_cast<u32>(sizes.size());
        sizes.push_back(0);
        stack.clear();
        stack.push_back(s);
        comp[s] = id;
        while (!stack.empty()) {
            const u32 u = stack.back();
            stack.pop_back();
            ++sizes[id];
            for (u32 e = rstart[u]; e < rstart[u + 1]; ++e)
                if (comp[redges[e]] == kRoadNone) {
                    comp[redges[e]] = id;
                    stack.push_back(redges[e]);
                }
        }
    }
    u32 best = 0;
    for (u32 i = 1; i < sizes.size(); ++i)
        if (sizes[i] > sizes[best]) best = i;
    return best;
}

// "main" lane 0 forward / "main" lane 1 backward.
void lane_label(const RoadView& v, const Array<cook::String>& roads, u32 lane, cook::String& out) {
    const RoadLane& l = v.lanes[lane];
    out.clear();
    const asset::RoadRoad& r = v.roads[l.road];
    out += l.road < roads.size() ? roads[l.road].view() : StringView("?");
    const bool backward = (l.flags & asset::RoadLaneFlags::Backward) != 0;
    cook::appendf(out, " lane %u %s", unsigned(l.index), backward ? "backward" : "forward");
    (void)r;
}

bool roads_info(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    if (!need_project(c) || !asset_tool_arg(c, a)) return false;
    if (a->type != asset::ChunkType::RoadNetwork) return fail(c, "that asset is not a road network (a .mcroads; roads_generate or asset_create makes one)");
    const cook::String rel = a->path;
    const asset::Guid guid = a->guid;
    editor::EditorProject& project = c.server.session().project();
    cook::String text;
    if (!cook::fs::read_text(project.asset_path(rel.view()).view(), text)) return fail(c, "the road network file cannot be read");
    const cook::IImporter* importer = project.importers().for_extension(".mcroads");
    if (!importer) return fail(c, "no importer reads .mcroads");
    cook::AssetRegistry registry;
    cook::DiagList scan, diags;
    registry.scan(project.root(), project.importers(), false, scan);
    Array<byte> cooked;
    if (!cook::cook_document(registry, *importer, guid, rel.view(), text.view(), cooked, diags)) {
        c.error += "the road network does not cook:";
        u32 n = 0;
        for (const cook::Diagnostic& d : diags.all()) {
            if (d.severity != cook::Severity::Error || n++ == 8) continue;
            c.error += '\n';
            cook::format_diagnostic(d, c.error);
        }
        return false;
    }
    Result<asset::ContainerView> view = asset::ContainerView::open(cooked.span(), rel.view());
    const u32 chunk = view ? view.value().find(asset::ChunkType::RoadNetwork, guid) : 0;
    if (!view || chunk == view.value().chunk_count()) return fail(c, "the cooked file has no road network chunk");
    Result<RoadView> rv = RoadView::read(view.value().payload(chunk));
    if (!rv) return fail(c, "the cooked road network does not read");
    const RoadView& v = rv.value();
    cook::JsonDoc doc;
    cook::JsonError jerr;
    Array<cook::String> node_names, road_names;
    if (doc.parse(text.view(), jerr)) {
        source_names(doc.root(), "nodes", node_names);
        source_names(doc.root(), "roads", road_names);
    }

    u32 road_lanes = 0, connectors = 0, uturns = 0;
    f64 road_metres = 0.0;
    for (const RoadLane& l : v.lanes) {
        if (l.kind == RoadLaneKind::Road) {
            ++road_lanes;
            road_metres += f64(l.length);
        } else {
            ++connectors;
            uturns += l.index == u8(asset::RoadTurn::UTurn) ? 1u : 0u;
        }
    }
    Array<u32> comp;
    const u32 main = v.lanes.empty() ? 0 : lane_components(v, comp);

    c.out.begin_object();
    c.out.key("path").str(rel.view()).key("guid").str(asset_guid_ref(guid).view()).key("name").str(v.name());
    c.out.key("drive").str((v.header->flags & asset::RoadFlags::LeftHand) ? "left" : "right");
    c.out.key("nodes").num_u(v.nodes.size()).key("roads").num_u(v.roads.size()).key("road_lanes").num_u(road_lanes);
    c.out.key("connectors").num_u(connectors).key("u_turns").num_u(uturns).key("conflicts").num_u(v.conflicts.size() / 2);
    c.out.key("signals").num_u(v.signals.size()).key("lane_km").num_f(std::round(road_metres) / 1000.0);
    c.out.key("bounds").begin_array();
    for (u32 k = 0; k < 3; ++k) c.out.num_f(v.header->bounds_min[k]);
    for (u32 k = 0; k < 3; ++k) c.out.num_f(v.header->bounds_max[k]);
    c.out.end_array();
    // The junctions (nodes with a connector), first kMaxListed.
    c.out.key("junctions").begin_array();
    u32 listed = 0;
    for (u32 i = 0; i < v.nodes.size() && listed < kMaxListed; ++i) {
        const asset::RoadNode& n = v.nodes[i];
        if (n.road_count < 2) continue;
        ++listed;
        c.out.begin_object().key("name").str(i < node_names.size() ? node_names[i].view() : StringView());
        c.out.key("control").str(control_name(n.control)).key("roads").num_u(n.road_count).key("connectors").num_u(n.connector_count);
        if (n.signal != asset::kRoadNone16) c.out.key("cycle").num_f(v.signals[n.signal].cycle);
        c.out.end_object();
    }
    c.out.end_array();
    // What traffic trips on.
    c.out.key("dead_ends").begin_array();
    for (u32 i = 0, k = 0; i < v.nodes.size() && k < kMaxListed; ++i)
        if (v.nodes[i].road_count == 1) {
            c.out.str(i < node_names.size() ? node_names[i].view() : StringView());
            ++k;
        }
    c.out.end_array();
    u32 stuck = 0, cut = 0;
    cook::String label;
    c.out.key("no_way_on").begin_array();
    for (u32 i = 0; i < v.lanes.size(); ++i) {
        const RoadLane& l = v.lanes[i];
        if (l.kind != RoadLaneKind::Road || l.next_count != 0) continue;
        if (stuck++ < kMaxListed) {
            lane_label(v, road_names, i, label);
            c.out.str(label.view());
        }
    }
    c.out.end_array();
    c.out.key("cut_off").begin_array();
    for (u32 i = 0; i < v.lanes.size(); ++i) {
        const RoadLane& l = v.lanes[i];
        if (l.kind != RoadLaneKind::Road || comp[i] == main) continue;
        if (cut++ < kMaxListed) {
            lane_label(v, road_names, i, label);
            c.out.str(label.view());
        }
    }
    c.out.end_array();
    c.out.key("no_way_on_count").num_u(stuck).key("cut_off_count").num_u(cut);
    c.out.key("ok").boolean(stuck == 0 && cut == 0);
    c.out.end_object();
    return true;
}

bool arg_f64(ToolCall& c, const char* name, f64& out, f64 lo, f64 hi) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid()) return true;
    if (!v.is_number() || !(v.as_number() >= lo && v.as_number() <= hi)) {
        cook::appendf(c.error, "argument '%s' must be a number from %g to %g", name, lo, hi);
        return false;
    }
    out = v.as_number();
    return true;
}

bool roads_generate(ToolCall& c) {
    StringView rel;
    if (!need_project(c) || !arg_string(c, "path", rel, true)) return false;
    if (!asset_rel_valid(rel) || !is_mcroads(rel))
        return fail(c, "argument 'path' must be a .mcroads path under assets/ like \"city/downtown.mcroads\" (forward slashes, no '..')");
    const cook::String kept(rel);
    bool replace = false;
    if (!arg_bool(c, "replace", replace)) return false;
    editor::EditorProject& project = c.server.session().project();
    asset::Guid guid = cook::generate_guid();
    if (cook::fs::exists(project.asset_path(rel).view())) {
        if (!replace) return fail(c, "a file already exists there: pass replace: true to overwrite it (its GUID stays)");
        const editor::ProjectAsset* a = nullptr;
        for (const editor::ProjectAsset& pa : project.assets())
            if (pa.path.view() == rel) a = &pa;
        if (!a || a->type != asset::ChunkType::RoadNetwork) return fail(c, "the file there is not a registered road network");
        guid = a->guid;
    }
    u64 columns = 4, rows = 4, lanes = 1, seconds = 20;
    f64 block = 100.0, speed = 13.9, sidewalk = 0.0;
    StringView control = "signal", name, drive = "right";
    if (!arg_uint(c, "columns", columns, 2, kMaxGrid, false) || !arg_uint(c, "rows", rows, 2, kMaxGrid, false) ||
        !arg_uint(c, "lanes", lanes, 1, 4, false) || !arg_uint(c, "seconds", seconds, 5, 120, false) ||
        !arg_f64(c, "block", block, 20.0, 1000.0) || !arg_f64(c, "speed", speed, 1.0, 60.0) ||
        !arg_f64(c, "sidewalk", sidewalk, 0.0, 10.0) || !arg_string(c, "control", control, false) ||
        !arg_string(c, "name", name, false) || !arg_string(c, "drive", drive, false))
        return false;
    if (control != "signal" && control != "stop" && control != "priority" && control != "none")
        return fail(c, "argument 'control' must be \"signal\", \"stop\", \"priority\" or \"none\"");
    if (drive != "right" && drive != "left") return fail(c, "argument 'drive' must be \"right\" or \"left\"");
    const u32 nx = static_cast<u32>(columns), nz = static_cast<u32>(rows);
    // Centred on the origin; columns along x, rows along z.
    const f64 x0 = -0.5 * block * f64(nx - 1), z0 = -0.5 * block * f64(nz - 1);
    const auto degree = [&](u32 i, u32 j) { return (i > 0) + (i + 1 < nx) + (j > 0) + (j + 1 < nz); };
    cook::String compact;
    JsonWriter w(compact);
    w.begin_object().key("format").str("mce-roads").key("version").num_u(1).key("guid").str(asset::to_string(guid).view());
    w.key("name").str(name.empty() ? StringView("grid") : name).key("drive").str(drive).key("speed").num_f(speed);
    w.key("nodes").begin_array();
    cook::String nm;
    for (u32 j = 0; j < nz; ++j)
        for (u32 i = 0; i < nx; ++i) {
            nm.clear();
            cook::appendf(nm, "n%u_%u", i, j);
            w.begin_object().key("name").str(nm.view()).key("at").begin_array().num_f(x0 + block * f64(i)).num_f(0).num_f(z0 + block * f64(j)).end_array();
            if (degree(i, j) >= 3 && control != "none") w.key("control").str(control);
            w.end_object();
        }
    w.end_array();
    // Roads: "x<i>_<j>" from (i, j) to (i + 1, j); "z<i>_<j>" from (i, j) to (i, j + 1). Under
    // "priority" the x roads are the major ones.
    w.key("roads").begin_array();
    cook::String from, to;
    for (u32 j = 0; j < nz; ++j)
        for (u32 i = 0; i < nx; ++i)
            for (u32 axis = 0; axis < 2; ++axis) {
                if ((axis == 0 && i + 1 >= nx) || (axis == 1 && j + 1 >= nz)) continue;
                nm.clear();
                from.clear();
                to.clear();
                cook::appendf(nm, "%c%u_%u", axis == 0 ? 'x' : 'z', i, j);
                cook::appendf(from, "n%u_%u", i, j);
                cook::appendf(to, "n%u_%u", axis == 0 ? i + 1 : i, axis == 0 ? j : j + 1);
                w.begin_object().key("name").str(nm.view()).key("from").str(from.view()).key("to").str(to.view()).key("lanes").num_u(lanes);
                if (sidewalk > 0.0) w.key("sidewalk").num_f(sidewalk);
                if (control == "priority" && axis == 0) w.key("priority").num_u(1);
                w.end_object();
            }
    w.end_array();
    // Signals: the x roads green, then the z roads.
    if (control == "signal") {
        w.key("signals").begin_array();
        for (u32 j = 0; j < nz; ++j)
            for (u32 i = 0; i < nx; ++i) {
                if (degree(i, j) < 3) continue;
                nm.clear();
                cook::appendf(nm, "n%u_%u", i, j);
                w.begin_object().key("node").str(nm.view()).key("phases").begin_array();
                for (u32 axis = 0; axis < 2; ++axis) {
                    w.begin_object().key("green").begin_array();
                    const auto road = [&](char a, u32 ri, u32 rj) {
                        nm.clear();
                        cook::appendf(nm, "%c%u_%u", a, ri, rj);
                        w.str(nm.view());
                    };
                    if (axis == 0) {
                        if (i > 0) road('x', i - 1, j);
                        if (i + 1 < nx) road('x', i, j);
                    } else {
                        if (j > 0) road('z', i, j - 1);
                        if (j + 1 < nz) road('z', i, j);
                    }
                    w.end_array().key("seconds").num_u(seconds).end_object();
                }
                w.end_array().end_object();
            }
        w.end_array();
    }
    w.end_object();
    cook::JsonDoc doc;
    cook::JsonError err;
    if (!doc.parse(compact.view(), err)) return fail(c, "internal: the generated network is not JSON");
    cook::String text;
    cook::write_json(doc.root(), text, false);
    text += '\n';
    if (!asset_validate_and_write(c, kept.view(), guid, text.view())) return false;
    c.out.begin_object();
    c.out.key("path").str(kept.view()).key("guid").str(asset_guid_ref(guid).view()).key("type").str("RoadNetwork");
    c.out.key("nodes").num_u(nx * nz).key("roads").num_u((nx - 1) * nz + nx * (nz - 1)).key("bytes").num_u(text.size());
    c.out.end_object();
    return true;
}

const ToolDef kRoadTools[] = {
    {"roads_info",
     "Cook a .mcroads road network (by path or guid) and report what the cook built: nodes, roads, road lanes, junction "
     "connectors (U-turns among them), conflicting connector pairs, signals, lane kilometres and bounds; each junction's "
     "control, roads, connectors and signal cycle (seconds); dead-end nodes; and what traffic would trip on: 'no_way_on' "
     "(one-way lanes that end with nowhere to go) and 'cut_off' (lanes from which the rest of the network cannot be reached, "
     "or that cannot be reached from it). 'ok' is true when both are empty.",
     R"json({"type":"object","properties":{"path":{"type":"string","description":"The .mcroads under assets/"},"guid":{"type":"string"}}})json",
     roads_info},
    {"roads_generate",
     "Write a street grid as a .mcroads to start a city from: 'columns' x 'rows' junctions (2-32 each) 'block' metres "
     "apart, centred on the origin, columns along +x and rows along +z. Nodes are n<i>_<j>; roads x<i>_<j> run +x and "
     "z<i>_<j> run +z, 'lanes' each way. Junctions with three or more roads get 'control' (signal: x roads green, then z "
     "roads, 'seconds' each; priority: the x roads are major). Refuses an existing file unless 'replace' (its GUID stays). "
     "Edit the file afterwards (asset_write) to bend roads or drop blocks.",
     R"json({"type":"object","properties":{"path":{"type":"string","description":"A .mcroads path under assets/"},"columns":{"type":"integer","default":4},"rows":{"type":"integer","default":4},"block":{"type":"number","default":100},"lanes":{"type":"integer","default":1},"speed":{"type":"number","default":13.9,"description":"m/s"},"sidewalk":{"type":"number","default":0,"description":"metres beside each kerb (crowds walk there)"},"control":{"type":"string","enum":["signal","stop","priority","none"],"default":"signal"},"seconds":{"type":"integer","default":20,"description":"green per phase"},"drive":{"type":"string","enum":["right","left"],"default":"right"},"name":{"type":"string"},"replace":{"type":"boolean","default":false}},"required":["path"]})json",
     roads_generate},
};

}  // namespace

Span<const ToolDef> roads_tool_table() { return Span<const ToolDef>(kRoadTools, sizeof(kRoadTools) / sizeof(kRoadTools[0])); }

}  // namespace mce::mcp
