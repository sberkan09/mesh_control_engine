// procgen_run (M-PROCGEN 4): runs one of the kit's level generators (mce/kit/procgen.h) from a seed and
// answers with the level as text rows, its hash and whether the exit can be reached from the start.
// With 'count' it tries a run of seeds and lists the ones that fail. With 'bake' it paints the result
// into a .mctilemap (floor and wall tiles), the way tilemap_paint does.

#include "mce/cook/fs.h"
#include "mce/editor/tilemap_editor.h"
#include "mce/kit/procgen.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

namespace pg = kit::procgen;

constexpr u64 kMaxCount = 1000;

struct RunArgs {
    StringView kind;
    u32 width = 0, height = 0;
    u64 seed = 1;
    u64 count = 1;
    pg::RoomsParams rooms;
    pg::CavesParams caves;
    pg::WalkParams walk;
    pg::NoiseParams noise;
};

bool arg_param(ToolCall& c, const char* name, u32& out, u64 hi) {
    if (!arg(c, name).valid()) return true;
    u64 v = out;
    if (!arg_uint(c, name, v, 0, hi, false)) return false;
    out = static_cast<u32>(v);
    return true;
}

Result<pg::ProcGrid> generate(const RunArgs& a, u64 seed) {
    if (a.kind == "rooms") return pg::generate_rooms(a.width, a.height, seed, a.rooms);
    if (a.kind == "caves") return pg::generate_caves(a.width, a.height, seed, a.caves);
    if (a.kind == "walk") return pg::generate_walk(a.width, a.height, seed, a.walk);
    return pg::generate_noise(a.width, a.height, seed, a.noise);
}

// Breadth-first from the start over floor cells (4-neighbours): is the exit reached? An independent check of
// the generators' own connectivity guarantee.
bool exit_reachable(const pg::ProcGrid& g) {
    Array<u8> seen;
    seen.resize(static_cast<usize>(g.width) * g.height);
    for (u8& s : seen) s = 0;
    Array<u32> stack;
    const auto idx = [&](i32 x, i32 y) { return static_cast<u32>(y) * g.width + static_cast<u32>(x); };
    seen[idx(g.start.x, g.start.y)] = 1;
    stack.push_back(idx(g.start.x, g.start.y));
    while (!stack.empty()) {
        const u32 i = stack.back();
        stack.pop_back();
        const i32 x = static_cast<i32>(i % g.width), y = static_cast<i32>(i / g.width);
        const i32 dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (u32 d = 0; d < 4; ++d) {
            const i32 nx = x + dx[d], ny = y + dy[d];
            if (nx < 0 || ny < 0 || nx >= static_cast<i32>(g.width) || ny >= static_cast<i32>(g.height)) continue;
            if (g.at(static_cast<u32>(nx), static_cast<u32>(ny)) != pg::kFloor || seen[idx(nx, ny)]) continue;
            seen[idx(nx, ny)] = 1;
            stack.push_back(idx(nx, ny));
        }
    }
    return seen[idx(g.exit.x, g.exit.y)] != 0;
}

bool procgen_run(ToolCall& c) {
    RunArgs a;
    u64 w = 0, h = 0;
    if (!arg_string(c, "kind", a.kind, true)) return false;
    if (a.kind != "rooms" && a.kind != "caves" && a.kind != "walk" && a.kind != "noise")
        return fail(c, "argument 'kind' must be rooms, caves, walk or noise");
    if (!arg_uint(c, "width", w, pg::kMinSize, pg::kMaxSize, true) || !arg_uint(c, "height", h, pg::kMinSize, pg::kMaxSize, true) ||
        !arg_uint(c, "seed", a.seed, 0, ~u64{0}, false) || !arg_uint(c, "count", a.count, 1, kMaxCount, false))
        return false;
    a.width = static_cast<u32>(w);
    a.height = static_cast<u32>(h);
    if (!arg_param(c, "leaf_min", a.rooms.leaf_min, 64) || !arg_param(c, "room_min", a.rooms.room_min, 64) ||
        !arg_param(c, "corridor", a.rooms.corridor, 2) || !arg_param(c, "fill_percent", a.caves.fill_percent, 100) ||
        !arg_param(c, "iterations", a.caves.iterations, 20) || !arg_param(c, "coverage_percent", a.walk.coverage_percent, 100) ||
        !arg_param(c, "turn_percent", a.walk.turn_percent, 100) || !arg_param(c, "scale", a.noise.scale, 256) ||
        !arg_param(c, "octaves", a.noise.octaves, 6) || !arg_param(c, "floor_percent", a.noise.floor_percent, 100))
        return false;
    u32 min_floor = 0;
    const bool has_min_floor = arg(c, "min_floor_percent").valid();
    if (!arg_param(c, "min_floor_percent", min_floor, 100)) return false;
    if (has_min_floor) a.rooms.min_floor_percent = a.caves.min_floor_percent = a.noise.min_floor_percent = min_floor;
    bool want_ascii = true;
    if (arg(c, "ascii").valid() && !arg_bool(c, "ascii", want_ascii)) return false;
    StringView bake;
    if (!arg_string(c, "bake", bake, false)) return false;
    if (!bake.empty() && a.count != 1) return fail(c, "'bake' paints one level: leave 'count' at 1");

    if (a.count > 1) {
        // A run of seeds: only the failures are listed.
        u64 ok = 0;
        c.out.begin_object();
        c.out.key("kind").str(a.kind).key("first_seed").num_u(a.seed).key("count").num_u(a.count);
        c.out.key("failures").begin_array();
        for (u64 i = 0; i < a.count; ++i) {
            const u64 seed = a.seed + i;
            Result<pg::ProcGrid> r = generate(a, seed);
            const char* why = nullptr;
            if (!r.has_value()) why = "generator error";
            else if (!exit_reachable(r.value())) why = "exit not reachable from start";
            if (!why) {
                ++ok;
                continue;
            }
            c.out.begin_object().key("seed").num_u(seed).key("problem").str(why);
            if (!r.has_value()) c.out.key("error").str(r.error().stage);
            c.out.end_object();
        }
        c.out.end_array();
        c.out.key("ok").num_u(ok);
        c.out.end_object();
        return true;
    }

    Result<pg::ProcGrid> r = generate(a, a.seed);
    if (!r.has_value()) return fail(c, r.error().stage);
    const pg::ProcGrid& g = r.value();

    u64 floor_tile = 0, wall_tile = 0, painted = 0, outside = 0;
    if (!bake.empty()) {
        if (!need_project(c)) return false;
        const cook::JsonValue ft = arg(c, "floor_tile"), wt = arg(c, "wall_tile");
        if (!ft.valid() || !wt.valid()) return fail(c, "'bake' needs 'floor_tile' and 'wall_tile' (tileset cells: 0 empty, n draws cell n-1)");
        if (!arg_uint(c, "floor_tile", floor_tile, 0, 1u << 20, true) || !arg_uint(c, "wall_tile", wall_tile, 0, 1u << 20, true)) return false;
        const cook::String rel(bake.data(), bake.size());
        editor::TilemapDocument d;
        const cook::String abs = c.server.session().project().asset_path(rel.view());
        cook::String text;
        cook::DiagList diags;
        if (!cook::fs::read_text(abs.view(), text) || !d.parse(text.view(), rel.view(), diags))
            return fail(c, "'bake' must name an existing .mctilemap under assets/ (asset_create makes one)");
        if (floor_tile > d.cells() || wall_tile > d.cells()) return fail(c, "floor_tile or wall_tile is outside the tileset");
        // Row 0 of the map is the bottom; the grid's row 0 is the top (as in the text rows).
        for (u32 y = 0; y < g.height; ++y)
            for (u32 x = 0; x < g.width; ++x) {
                const i32 ty = static_cast<i32>(g.height) - 1 - static_cast<i32>(y);
                if (!d.contains(static_cast<i32>(x), ty)) {
                    ++outside;
                    continue;
                }
                d.set(static_cast<i32>(x), ty, g.at(x, y) == pg::kFloor ? static_cast<u32>(floor_tile) : static_cast<u32>(wall_tile));
                ++painted;
            }
        cook::String out;
        d.serialize(out);
        if (!asset_validate_and_write(c, rel.view(), d.guid, out.view())) return false;
    }

    c.out.begin_object();
    c.out.key("kind").str(a.kind).key("seed").num_u(a.seed).key("width").num_u(g.width).key("height").num_u(g.height);
    cook::String hash;
    cook::appendf(hash, "%016llx", static_cast<unsigned long long>(g.hash()));
    c.out.key("hash").str(hash.view());
    c.out.key("floor_cells").num_u(g.floor_count());
    c.out.key("start").begin_array().num_i(g.start.x).num_i(g.start.y).end_array();
    c.out.key("exit").begin_array().num_i(g.exit.x).num_i(g.exit.y).end_array();
    c.out.key("reachable").boolean(exit_reachable(g));
    if (!bake.empty()) c.out.key("baked").begin_object().key("path").str(bake).key("painted").num_u(painted).key("outside").num_u(outside).end_object();
    if (want_ascii) {
        Array<char> text;
        g.to_ascii(text);
        c.out.key("rows").begin_array();
        usize row = 0;
        for (usize i = 0; i <= text.size(); ++i)
            if (i == text.size() || text[i] == '\n') {
                if (i > row) c.out.str(StringView(text.data() + row, i - row));
                row = i + 1;
            }
        c.out.end_array();
    }
    c.out.end_object();
    return true;
}

const ToolDef kProcgenTools[] = {
    {"procgen_run",
     "Run a level generator from a seed (the same code the engine's ProcLevel and Luau use): kind rooms (BSP rooms and "
     "corridors), caves (cellular automaton), walk (drunkard's walk) or noise (threshold of value noise), on a width x "
     "height grid (8..512). Returns the level as text rows ('#' wall, '.' floor, 'S' start, 'E' exit), a hash, the floor "
     "count and whether E is reachable from S (checked again here, not trusted). Deterministic: the same arguments give the same "
     "level. 'count' > 1 tries that many consecutive seeds and lists the failing ones only. 'bake' (a .mctilemap under "
     "assets/) paints the level into it with 'floor_tile' and 'wall_tile' (tileset cells, as tilemap_paint). Generator "
     "parameters are optional top-level integers: leaf_min, room_min, corridor (rooms); fill_percent, iterations (caves); "
     "coverage_percent, turn_percent (walk); scale, octaves, floor_percent (noise); min_floor_percent (all but walk).",
     R"json({"type":"object","properties":{"kind":{"type":"string","enum":["rooms","caves","walk","noise"]},"width":{"type":"integer","minimum":8,"maximum":512},"height":{"type":"integer","minimum":8,"maximum":512},"seed":{"type":"integer","default":1},"count":{"type":"integer","default":1,"minimum":1,"maximum":1000},"ascii":{"type":"boolean","default":true},"bake":{"type":"string","description":"An existing .mctilemap under assets/"},"floor_tile":{"type":"integer","minimum":0},"wall_tile":{"type":"integer","minimum":0},"leaf_min":{"type":"integer"},"room_min":{"type":"integer"},"corridor":{"type":"integer","minimum":1,"maximum":2},"fill_percent":{"type":"integer"},"iterations":{"type":"integer"},"coverage_percent":{"type":"integer"},"turn_percent":{"type":"integer"},"scale":{"type":"integer"},"octaves":{"type":"integer"},"floor_percent":{"type":"integer"},"min_floor_percent":{"type":"integer"}},"required":["kind","width","height"]})json",
     procgen_run},
};

}  // namespace

Span<const ToolDef> procgen_tool_table() { return Span<const ToolDef>(kProcgenTools, sizeof(kProcgenTools) / sizeof(kProcgenTools[0])); }

}  // namespace mce::mcp
