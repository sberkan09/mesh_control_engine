#include "procgen_check.h"

namespace mce::mcp {

namespace {

namespace pg = kit::procgen;

bool int_in(const cook::JsonValue& obj, const char* name, u64& out, u64 lo, u64 hi, bool required, cook::String& err) {
    const cook::JsonValue v = obj.get(name);
    if (!v.valid() && !required) return true;
    if (!v.is_integer() || v.as_number() < static_cast<f64>(lo) || v.as_number() > static_cast<f64>(hi)) {
        err += "argument '";
        err += name;
        cook::appendf(err, "' must be an integer in [%llu, %llu]", static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
        return false;
    }
    out = static_cast<u64>(v.as_number());
    return true;
}

bool param(const cook::JsonValue& obj, const char* name, u32& out, u64 hi, cook::String& err) {
    u64 v = out;
    if (!int_in(obj, name, v, 0, hi, false, err)) return false;
    out = static_cast<u32>(v);
    return true;
}

}  // namespace

bool parse_procgen_run(const cook::JsonValue& obj, ProcgenRun& out, cook::String& err) {
    if (!obj.is_object()) {
        err += "a seeded level is an object: {\"kind\": \"rooms\", \"width\": 48, \"height\": 32, \"seed\": 1, \"count\": 100}";
        return false;
    }
    const cook::JsonValue kind = obj.get("kind");
    if (!kind.is_string() || (kind.as_string() != "rooms" && kind.as_string() != "caves" && kind.as_string() != "walk" && kind.as_string() != "noise")) {
        err += "argument 'kind' must be rooms, caves, walk or noise";
        return false;
    }
    out.kind = cook::String(kind.as_string());
    u64 w = 0, h = 0;
    if (!int_in(obj, "width", w, pg::kMinSize, pg::kMaxSize, true, err) || !int_in(obj, "height", h, pg::kMinSize, pg::kMaxSize, true, err) ||
        !int_in(obj, "seed", out.seed, 0, 1ull << 53, false, err) || !int_in(obj, "count", out.count, 1, 1000, false, err))
        return false;
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    if (!param(obj, "leaf_min", out.rooms.leaf_min, 64, err) || !param(obj, "room_min", out.rooms.room_min, 64, err) ||
        !param(obj, "corridor", out.rooms.corridor, 2, err) || !param(obj, "fill_percent", out.caves.fill_percent, 100, err) ||
        !param(obj, "iterations", out.caves.iterations, 20, err) || !param(obj, "coverage_percent", out.walk.coverage_percent, 100, err) ||
        !param(obj, "turn_percent", out.walk.turn_percent, 100, err) || !param(obj, "scale", out.noise.scale, 256, err) ||
        !param(obj, "octaves", out.noise.octaves, 6, err) || !param(obj, "floor_percent", out.noise.floor_percent, 100, err))
        return false;
    if (obj.get("min_floor_percent").valid()) {
        u32 m = 0;
        if (!param(obj, "min_floor_percent", m, 100, err)) return false;
        out.rooms.min_floor_percent = out.caves.min_floor_percent = out.noise.min_floor_percent = m;
    }
    return true;
}

Result<pg::ProcGrid> procgen_generate(const ProcgenRun& a, u64 seed) {
    if (a.kind.view() == "rooms") return pg::generate_rooms(a.width, a.height, seed, a.rooms);
    if (a.kind.view() == "caves") return pg::generate_caves(a.width, a.height, seed, a.caves);
    if (a.kind.view() == "walk") return pg::generate_walk(a.width, a.height, seed, a.walk);
    return pg::generate_noise(a.width, a.height, seed, a.noise);
}

bool procgen_exit_reachable(const pg::ProcGrid& g) {
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

}  // namespace mce::mcp
