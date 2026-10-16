// procgen_run (M-PROCGEN 4): runs one of the kit's level generators (mce/kit/procgen.h) from a seed and
// answers with the level as text rows, its hash and whether the exit can be reached from the start.
// With 'count' it tries a run of seeds and lists the ones that fail. With 'bake' it paints the result
// into a .mctilemap (floor and wall tiles), the way tilemap_paint does.

#include "mce/cook/fs.h"
#include "mce/editor/tilemap_editor.h"
#include "mce/kit/procgen.h"
#include "procgen_check.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

namespace pg = kit::procgen;

bool procgen_run(ToolCall& c) {
    ProcgenRun a;
    if (!parse_procgen_run(c.args, a, c.error)) return false;
    bool want_ascii = true;
    if (arg(c, "ascii").valid() && !arg_bool(c, "ascii", want_ascii)) return false;
    StringView bake;
    if (!arg_string(c, "bake", bake, false)) return false;
    if (!bake.empty() && a.count != 1) return fail(c, "'bake' paints one level: leave 'count' at 1");

    if (a.count > 1) {
        // A run of seeds: only the failures are listed.
        u64 ok = 0;
        c.out.begin_object();
        c.out.key("kind").str(a.kind.view()).key("first_seed").num_u(a.seed).key("count").num_u(a.count);
        c.out.key("failures").begin_array();
        for (u64 i = 0; i < a.count; ++i) {
            const u64 seed = a.seed + i;
            Result<pg::ProcGrid> r = procgen_generate(a, seed);
            const char* why = nullptr;
            if (!r.has_value()) why = "generator error";
            else if (!procgen_exit_reachable(r.value())) why = "exit not reachable from start";
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

    Result<pg::ProcGrid> r = procgen_generate(a, a.seed);
    if (!r.has_value()) return fail(c, r.error().stage);
    const pg::ProcGrid& g = r.value();

    u64 floor_tile = 0, wall_tile = 0, painted = 0, outside = 0;
    if (!bake.empty()) {
        if (!need_project(c)) return false;
        const cook::JsonValue ft = arg(c, "floor_tile"), wt = arg(c, "wall_tile");
        if (!ft.valid() || !wt.valid()) return fail(c, "'bake' needs 'floor_tile' and 'wall_tile' (tileset cells: 0 empty, n draws cell n-1)");
        if (!arg_uint(c, "floor_tile", floor_tile, 0, 1u << 20, true) || !arg_uint(c, "wall_tile", wall_tile, 0, 1u << 20, true)) return false;
        cook::String rel;
        rel.assign(bake);
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
    c.out.key("kind").str(a.kind.view()).key("seed").num_u(a.seed).key("width").num_u(g.width).key("height").num_u(g.height);
    cook::String hash;
    cook::appendf(hash, "%016llx", static_cast<unsigned long long>(g.hash()));
    c.out.key("hash").str(hash.view());
    c.out.key("floor_cells").num_u(g.floor_count());
    c.out.key("start").begin_array().num_i(g.start.x).num_i(g.start.y).end_array();
    c.out.key("exit").begin_array().num_i(g.exit.x).num_i(g.exit.y).end_array();
    c.out.key("reachable").boolean(procgen_exit_reachable(g));
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
