// The terrain tools (M-TERRAIN 6): terrain_sculpt and terrain_paint run the editor's brushes
// (editor::TerrainDocument, the Terrain panel's code) over a .mcterrain, terrain_import reads a
// heightmap file into it and terrain_generate grows one from noise. Each reads the source, applies
// the change, and writes the source back through the same cook-validated write asset_write uses.

#include <algorithm>
#include <cmath>

#include "mce/asset/container.h"
#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/editor/terrain_editor.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

constexpr u32 kMaxPoints = 4096;
constexpr u32 kMaxDabs = 1u << 20;

// The terrain named by 'path' or 'guid', read into `d` (the source cooked in memory gives the data the
// brushes edit).
bool load_terrain(ToolCall& c, const editor::ProjectAsset*& a, editor::TerrainDocument& d) {
    if (!need_project(c) || !asset_tool_arg(c, a)) return false;
    if (a->type != asset::ChunkType::Terrain)
        return fail(c, "that asset is not a terrain (asset_create makes one: a .mcterrain path)");
    editor::EditorProject& project = c.server.session().project();
    const cook::String abs = project.asset_path(a->path.view());
    cook::String text;
    if (!cook::fs::read_text(abs.view(), text)) return fail(c, "the terrain file cannot be read");
    const cook::IImporter* importer = project.importers().for_extension(".mcterrain");
    if (!importer) return fail(c, "no importer reads .mcterrain");
    cook::AssetRegistry registry;
    cook::DiagList scan, diags;
    registry.scan(project.root(), project.importers(), false, scan);
    Array<byte> cooked;
    if (!cook::cook_document(registry, *importer, a->guid, a->path.view(), text.view(), cooked, diags))
        return fail(c, "the terrain does not cook (cook_run names the error)");
    Result<asset::ContainerView> view = asset::ContainerView::open(cooked.span(), a->path.view());
    const u32 chunk = view ? view.value().find(asset::ChunkType::Terrain, a->guid) : 0;
    if (!view || chunk == view.value().chunk_count()) return fail(c, "the cooked terrain has no terrain chunk");
    if (!d.load(text.view(), view.value().payload(chunk), a->path.view(), diags)) return fail(c, "the terrain does not read");
    return true;
}

bool save_terrain(ToolCall& c, const editor::ProjectAsset* a, const editor::TerrainDocument& d) {
    const cook::String rel = a->path;
    const asset::Guid guid = a->guid;
    cook::String text;
    d.serialize(text);
    return asset_validate_and_write(c, rel.view(), guid, text.view());
}

bool arg_f32(ToolCall& c, const char* name, f32& out, f32 lo, f32 hi) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid()) return true;
    if (!v.is_number() || !(v.as_number() >= lo && v.as_number() <= hi)) {
        cook::appendf(c.error, "argument '%s' must be a number from %g to %g", name, f64(lo), f64(hi));
        return false;
    }
    out = static_cast<f32>(v.as_number());
    return true;
}

bool arg_u32(ToolCall& c, const char* name, u32& out, u32 lo, u32 hi) {
    u64 v = out;
    if (!arg_uint(c, name, v, lo, hi, false)) return false;
    out = static_cast<u32>(v);
    return true;
}

// 'points': [[x, z], ...] metres in the terrain's own space (its centre at 0, 0).
bool arg_points(ToolCall& c, Array<math::vec2>& out) {
    const cook::JsonValue v = arg(c, "points");
    if (!v.valid()) return arg_missing(c, "points", "an array of [x, z] points (metres from the terrain's centre)");
    bool ok = v.is_array() && v.size() >= 1 && v.size() <= kMaxPoints;
    for (u32 i = 0; ok && i < v.size(); ++i) {
        const cook::JsonValue p = v.at(i);
        ok = p.is_array() && p.size() == 2 && p.at(0).is_number() && p.at(1).is_number() && std::isfinite(p.at(0).as_number()) &&
             std::isfinite(p.at(1).as_number());
        if (ok) out.push_back({static_cast<f32>(p.at(0).as_number()), static_cast<f32>(p.at(1).as_number())});
    }
    if (!ok) return fail(c, "argument 'points' must be an array of 1 to 4096 [x, z] number pairs");
    return true;
}

// The brush's shape arguments, shared by both brush tools.
bool arg_brush(ToolCall& c, editor::TerrainBrush& b, bool& stroke, u32& repeat) {
    stroke = false;
    repeat = 1;
    return arg_f32(c, "radius", b.radius, 0.01f, 100000.0f) && arg_f32(c, "hardness", b.hardness, 0.0f, 1.0f) &&
           arg_bool(c, "stroke", stroke) && arg_u32(c, "repeat", repeat, 1, 1000);
}

// Dabs at every point (`stroke`: also along each segment, a quarter radius apart), `repeat` times.
u32 run_brush(editor::TerrainDocument& d, const editor::TerrainBrush& b, Span<const math::vec2> points, bool stroke, u32 repeat,
              u32& dabs) {
    u32 changed = 0;
    dabs = 0;
    const f32 gap = std::max(0.25f * b.radius, 0.01f);
    for (u32 r = 0; r < repeat; ++r)
        for (usize i = 0; i < points.size() && dabs < kMaxDabs; ++i) {
            if (stroke && i > 0) {
                const math::vec2 a = points[i - 1], e = points[i];
                const f32 len = std::sqrt((e.x - a.x) * (e.x - a.x) + (e.y - a.y) * (e.y - a.y));
                const u32 steps = static_cast<u32>(std::min(len / gap, 65536.0f));
                for (u32 s = 1; s <= steps && dabs < kMaxDabs; ++s) {
                    const f32 t = static_cast<f32>(s) * gap / len;
                    changed += d.apply(b, {a.x + (e.x - a.x) * t, a.y + (e.y - a.y) * t});
                    ++dabs;
                }
                if (steps > 0 && static_cast<f32>(steps) * gap >= len - 1e-4f) continue;  // the last step landed on the point
            }
            changed += d.apply(b, points[i]);
            ++dabs;
        }
    return changed;
}

void write_head(ToolCall& c, const editor::ProjectAsset* a, const editor::TerrainDocument& d) {
    c.out.key("path").str(a->path.view()).key("guid").str(asset_guid_ref(a->guid).view());
    c.out.key("resolution").num_u(d.resolution).key("size").begin_array().num_f(d.size.x).num_f(d.size.y).end_array();
}

void write_heights(ToolCall& c, const editor::TerrainDocument& d, Span<const math::vec2> probes) {
    f32 lo = 1e30f, hi = -1e30f;
    f64 sum = 0.0;
    for (f32 h : d.heights) {
        lo = std::min(lo, h);
        hi = std::max(hi, h);
        sum += h;
    }
    c.out.key("height").begin_object().key("min").num_f(lo).key("max").num_f(hi).key("mean").num_f(sum / f64(d.heights.size())).end_object();
    if (!probes.empty()) {
        // The height under each point after the change.
        c.out.key("probes").begin_array();
        for (const math::vec2& p : probes) c.out.num_f(d.height_at(p.x, p.y));
        c.out.end_array();
    }
}

bool terrain_sculpt(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    editor::TerrainDocument d;
    if (!load_terrain(c, a, d)) return false;
    StringView op;
    if (!arg_string(c, "op", op, true)) return false;
    editor::TerrainBrush b;
    if (op == "raise") b.op = editor::TerrainBrushOp::Raise;
    else if (op == "lower") b.op = editor::TerrainBrushOp::Lower;
    else if (op == "smooth") b.op = editor::TerrainBrushOp::Smooth;
    else if (op == "flatten") b.op = editor::TerrainBrushOp::Flatten;
    else if (op == "noise") b.op = editor::TerrainBrushOp::Noise;
    else return fail(c, "argument 'op' must be raise, lower, smooth, flatten or noise");
    b.strength = b.op == editor::TerrainBrushOp::Smooth || b.op == editor::TerrainBrushOp::Flatten ? 0.5f : 1.0f;
    b.target = d.height_min;
    bool stroke = false;
    u32 repeat = 1;
    Array<math::vec2> points;
    if (!arg_brush(c, b, stroke, repeat) || !arg_f32(c, "strength", b.strength, 0.0f, 100000.0f) ||
        !arg_f32(c, "target", b.target, d.height_min, d.height_max) || !arg_u32(c, "seed", b.seed, 0, 0xFFFFFFFFu) || !arg_points(c, points))
        return false;
    if ((b.op == editor::TerrainBrushOp::Smooth || b.op == editor::TerrainBrushOp::Flatten) && b.strength > 1.0f)
        return fail(c, "smooth and flatten take a strength from 0 to 1 (the share of the way each dab moves)");
    if (b.op == editor::TerrainBrushOp::Flatten && !arg(c, "target").valid())
        return fail(c, "flatten needs 'target': the height in metres to level to");
    u32 dabs = 0;
    const u32 changed = run_brush(d, b, points.span(), stroke, repeat, dabs);
    if (changed > 0 && !save_terrain(c, a, d)) return false;
    c.out.begin_object();
    write_head(c, a, d);
    c.out.key("dabs").num_u(dabs).key("changed").num_u(changed);
    write_heights(c, d, points.span());
    c.out.end_object();
    return true;
}

bool terrain_paint(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    editor::TerrainDocument d;
    if (!load_terrain(c, a, d)) return false;
    editor::TerrainBrush b;
    b.op = editor::TerrainBrushOp::Paint;
    b.strength = 1.0f;
    StringView mode = "layer";
    if (!arg_string(c, "mode", mode, false)) return false;
    if (mode == "hole") b.op = editor::TerrainBrushOp::Hole;
    else if (mode == "fill") b.op = editor::TerrainBrushOp::Fill;
    else if (mode != "layer") return fail(c, "argument 'mode' must be layer, hole or fill");
    bool stroke = false;
    u32 repeat = 1;
    Array<math::vec2> points;
    if (!arg_brush(c, b, stroke, repeat) || !arg_f32(c, "strength", b.strength, 0.0f, 1.0f) || !arg_points(c, points)) return false;
    if (b.op == editor::TerrainBrushOp::Paint) {
        if (d.layer_count == 0) return fail(c, "the terrain has no splat layers (asset_write adds 'layers')");
        if (!arg(c, "layer").valid()) return arg_missing(c, "layer", "the splat layer to paint (0-based)");
        if (!arg_u32(c, "layer", b.layer, 0, d.layer_count - 1)) return false;
    }
    u32 dabs = 0;
    const u32 changed = run_brush(d, b, points.span(), stroke, repeat, dabs);
    if (changed > 0 && !save_terrain(c, a, d)) return false;
    c.out.begin_object();
    write_head(c, a, d);
    c.out.key("dabs").num_u(dabs).key("changed").num_u(changed);
    if (d.layer_count > 0) {
        // Each layer's share of the painted weight over the whole terrain, in percent.
        Array<f64> sums;
        sums.resize(d.layer_count, 0.0);
        f64 total = 0.0;
        for (usize i = 0; i < d.weights.size(); ++i) {
            sums[i % d.layer_count] += d.weights[i];
            total += d.weights[i];
        }
        c.out.key("coverage").begin_array();
        for (f64 s : sums) c.out.num_f(total > 0.0 ? std::round(1000.0 * s / total) / 10.0 : 0.0);
        c.out.end_array();
    }
    u32 holes = 0;
    for (u8 byte_bits : d.holes)
        for (u32 bit = 0; bit < 8; ++bit) holes += (byte_bits >> bit) & 1u;
    c.out.key("hole_samples").num_u(holes);
    c.out.end_object();
    return true;
}

bool terrain_import(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    editor::TerrainDocument d;
    if (!load_terrain(c, a, d)) return false;
    StringView file;
    if (!arg_string(c, "file", file, true)) return false;
    if (!asset_rel_valid(file)) return fail(c, "argument 'file' must be a path under assets/ (forward slashes, no '..')");
    editor::TerrainHeightmapImport o;
    const usize dot = file.rfind('.');
    const StringView ext = dot == StringView::npos ? StringView() : file.substr(dot);
    if (ext == ".png" || ext == ".PNG") o.png = true;
    else if (ext == ".raw" || ext == ".r16" || ext == ".r8" || ext == ".RAW") {
        o.png = false;
        o.raw_bits = ext == ".r8" ? 8 : 16;
    } else return fail(c, "argument 'file' must be a .png or a .raw / .r16 / .r8 heightmap");
    if (!arg_u32(c, "raw_bits", o.raw_bits, 8, 16) || !arg_u32(c, "raw_width", o.raw_width, 0, 65536)) return false;
    if (const cook::JsonValue r = arg(c, "range"); r.valid()) {
        if (!r.is_array() || r.size() != 2 || !r.at(0).is_number() || !r.at(1).is_number())
            return fail(c, "argument 'range' must be [low, high] metres");
        o.low = static_cast<f32>(r.at(0).as_number());
        o.high = static_cast<f32>(r.at(1).as_number());
    }
    const cook::String abs = c.server.session().project().asset_path(file);
    cook::String bytes;
    if (!cook::fs::read_text(abs.view(), bytes)) return fail(c, "the heightmap file cannot be read");
    cook::String err;
    if (!d.import_heightmap(Span<const byte>(reinterpret_cast<const byte*>(bytes.data()), bytes.size()), o, err)) return fail(c, err.view());
    if (!save_terrain(c, a, d)) return false;
    c.out.begin_object();
    write_head(c, a, d);
    write_heights(c, d, {});
    c.out.end_object();
    return true;
}

bool terrain_generate(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    editor::TerrainDocument d;
    if (!load_terrain(c, a, d)) return false;
    editor::TerrainGenerate g;
    if (!arg_u32(c, "seed", g.seed, 0, 0xFFFFFFFFu) || !arg_f32(c, "feature", g.feature, 0.0f, 1e6f) ||
        !arg_u32(c, "octaves", g.octaves, 1, 12) || !arg_f32(c, "lacunarity", g.lacunarity, 1.1f, 4.0f) ||
        !arg_f32(c, "gain", g.gain, 0.0f, 1.0f) || !arg_f32(c, "ridged", g.ridged, 0.0f, 1.0f) || !arg_f32(c, "low", g.low, 0.0f, 1.0f) ||
        !arg_f32(c, "high", g.high, 0.0f, 1.0f) || !arg_f32(c, "island", g.island, 0.0f, 1.0f) ||
        !arg_u32(c, "thermal", g.thermal, 0, 500) || !arg_f32(c, "talus_deg", g.talus_deg, 1.0f, 89.0f) ||
        !arg_u32(c, "droplets", g.droplets, 0, 2000000))
        return false;
    if (g.low > g.high) return fail(c, "'low' must not be above 'high'");
    d.generate(g);
    if (!save_terrain(c, a, d)) return false;
    c.out.begin_object();
    write_head(c, a, d);
    write_heights(c, d, {});
    c.out.end_object();
    return true;
}

constexpr const char* kBrushProps =
    R"json("path":{"type":"string","description":"The .mcterrain under assets/"},"guid":{"type":"string","description":"\"guid:<uuid>\" instead of path"},)json"
    R"json("points":{"type":"array","items":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"description":"[[x, z], ...] metres from the terrain's centre, in its own space"},)json"
    R"json("radius":{"type":"number","default":4,"description":"metres"},"hardness":{"type":"number","default":0.5,"description":"0 falls off from the centre, 1 full to the rim"},)json"
    R"json("stroke":{"type":"boolean","default":false,"description":"also dab along the segments between points, a quarter radius apart"},)json"
    R"json("repeat":{"type":"integer","default":1,"minimum":1,"maximum":1000,"description":"run the dabs this many times"})json";

const cook::String kSculptSchema = [] {
    cook::String s;
    s += R"json({"type":"object","properties":{)json";
    s += kBrushProps;
    s += R"json(,"op":{"type":"string","enum":["raise","lower","smooth","flatten","noise"]},"strength":{"type":"number","description":"raise/lower/noise: metres per dab at the centre (default 1); smooth/flatten: 0..1 of the way per dab (default 0.5)"},"target":{"type":"number","description":"flatten: metres"},"seed":{"type":"integer","description":"noise"}},"required":["op","points"]})json";
    return s;
}();

const cook::String kPaintSchema = [] {
    cook::String s;
    s += R"json({"type":"object","properties":{)json";
    s += kBrushProps;
    s += R"json(,"mode":{"type":"string","enum":["layer","hole","fill"],"default":"layer"},"layer":{"type":"integer","minimum":0,"maximum":7,"description":"layer mode: the splat layer"},"strength":{"type":"number","default":1,"description":"layer mode: 0..1 of full weight per dab"}},"required":["points"]})json";
    return s;
}();

const ToolDef kTerrainTools[] = {
    {"terrain_sculpt",
     "Sculpt a .mcterrain (by path or guid) with the editor's brushes: 'op' raise, lower, smooth, flatten (to 'target' "
     "metres) or noise, one dab at each of 'points' ([x, z] metres from the terrain's centre in its own space), or along "
     "them with stroke: true. Saved at once (the file is cook-checked); the Terrain3D using it re-bakes. Returns the dabs, "
     "the samples changed, the height range and the height under each point after the change.",
     kSculptSchema.c_str(), terrain_sculpt},
    {"terrain_paint",
     "Paint a .mcterrain (by path or guid): mode \"layer\" raises splat layer 'layer' (its TerrainFoliage3D grows "
     "there too), \"hole\" cuts holes (not drawn, not collided, left out of the navmesh), \"fill\" closes them; dabs at "
     "'points' as terrain_sculpt. Returns the samples changed, each layer's share of the paint (%) and the hole samples.",
     kPaintSchema.c_str(), terrain_paint},
    {"terrain_import",
     "Replace a .mcterrain's heights with a heightmap file under assets/: a .png (8- or 16-bit grey; colour gives its "
     "luminance) or raw samples (.r16/.raw 16-bit little-endian, .r8 8-bit; raw_width when not square), resampled to the "
     "terrain's resolution. The image's top row lies along -z, its left column along -x. 'range' [low, high] metres maps "
     "black and white (default: the terrain's height_range). Holes and splat layers stay.",
     R"json({"type":"object","properties":{"path":{"type":"string","description":"The .mcterrain under assets/"},"guid":{"type":"string"},"file":{"type":"string","description":"The heightmap under assets/, e.g. terrain/valley.png"},"raw_bits":{"type":"integer","enum":[8,16]},"raw_width":{"type":"integer","minimum":0},"range":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2}},"required":["file"]})json",
     terrain_import},
    {"terrain_generate",
     "Grow a landscape into a .mcterrain (by path or guid): fBm value noise ('feature' metres across the largest hills, "
     "default a quarter of the size; 'octaves', 'lacunarity', 'gain'; 'ridged' 0..1 for sharp ridges) stretched between "
     "'low' and 'high' (shares 0..1 of the height range), an optional 'island' falloff (0..1 of the way in from the edges "
     "the land sinks to 'low'), then erosion-lite: 'thermal' passes settle slopes toward 'talus_deg' and 'droplets' of "
     "rain carve channels. Deterministic in 'seed'. Holes and splat layers stay (auto rules re-apply only to unpainted "
     "terrains).",
     R"json({"type":"object","properties":{"path":{"type":"string","description":"The .mcterrain under assets/"},"guid":{"type":"string"},"seed":{"type":"integer","default":1},"feature":{"type":"number"},"octaves":{"type":"integer","default":6},"lacunarity":{"type":"number","default":2},"gain":{"type":"number","default":0.5},"ridged":{"type":"number","default":0},"low":{"type":"number","default":0},"high":{"type":"number","default":1},"island":{"type":"number","default":0},"thermal":{"type":"integer","default":0},"talus_deg":{"type":"number","default":35},"droplets":{"type":"integer","default":0}}})json",
     terrain_generate},
};

}  // namespace

Span<const ToolDef> terrain_tool_table() { return Span<const ToolDef>(kTerrainTools, sizeof(kTerrainTools) / sizeof(kTerrainTools[0])); }

}  // namespace mce::mcp
