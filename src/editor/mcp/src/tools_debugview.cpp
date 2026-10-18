// Gameplay debug view tools (M-DEBUGVIEW step 2): debug_lines and debug_view.
//
// Both play the saved scene headless for `ticks` and then read kit::gameplay_debug_lines from the world
// they ended in: colliders, bodies, triggers, joints, characters, vehicles and paths as world-space line
// segments. debug_lines returns the segments as data; debug_view draws them with a CPU rasteriser (no GPU)
// from the top, front or side and returns the picture as PNG. Neither writes the scene or the world.

#include <cmath>
#include <cstdio>

#include "agent_image.h"
#include "mce/cook/json.h"
#include "mce/kit/debug_lines.h"
#include "mce/mcp/headless_run.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {
namespace {

constexpr u64 kMaxTicks = 36000;
constexpr usize kMaxLines = 2000;

struct LineSnapshot {
    u32 mask = kit::kDebugAll;
    Array<kit::GameplayLine>* lines = nullptr;
};

void collect_lines(const World& cw, Span<const Entity>, void* user) {
    auto* snap = static_cast<LineSnapshot*>(user);
    World& w = const_cast<World&>(cw);  // the kit's readers take a mutable world (queries); nothing is written
    kit::gameplay_debug_lines(w, snap->mask, *snap->lines);
}

bool arg_categories(ToolCall& c, u32& mask) {
    mask = kit::kDebugAll;
    const cook::JsonValue v = arg(c, "categories");
    if (!v.valid()) return true;
    if (!v.is_array()) return arg_missing(c, "categories", "an array of: colliders, bodies, triggers, joints, characters, vehicles, paths");
    mask = 0;
    for (u32 i = 0; i < v.size(); ++i) {
        const cook::JsonValue n = v.at(i);
        const cook::String name(n.is_string() ? n.as_string() : StringView());
        const u32 bit = kit::gameplay_category_bit(name.c_str());
        if (bit == 0) return fail(c, "unknown category (use colliders, bodies, triggers, joints, characters, vehicles, paths)");
        mask |= bit;
    }
    return true;
}

// Plays the saved scene and fills `lines`. False (with c.error) when the scene cannot be played.
bool play_lines(ToolCall& c, u32 mask, u64 ticks, Array<kit::GameplayLine>& lines, u64& played) {
    if (!need_project(c)) return false;
    const Server::SceneFile& file = c.server.scene_file();
    if (file.rel.empty()) return fail(c, "the scene was never saved: scene_save it (with a path under assets/) first");
    if (scene(c).dirty()) return fail(c, "the scene has unsaved changes: scene_save first (this plays the cooked file)");
    const editor::SceneDocument& doc = scene(c).doc();
    const cook::CookReport& r = c.server.session().cook(false);
    if (!r.ok()) {
        c.error += "cook failed:";
        append_diags(c.error, r.diags, 10);
        return false;
    }
    LineSnapshot snap;
    snap.mask = mask;
    snap.lines = &lines;
    const cook::String cooked = cook::path_join(c.server.session().project().root(), "cooked");
    Result<HeadlessRun> run = run_headless(cooked.view(), doc.guid(), ticks, nullptr, nullptr, collect_lines, &snap, nullptr,
                                           c.server.session().project().base_roots());
    if (!run) {
        FixedString<512> msg;
        format_error(run.error(), msg);
        c.error += "run failed: ";
        c.error += msg.view();
        return false;
    }
    played = run.value().ticks;
    return true;
}

const char* category_name(u32 bit) {
    switch (bit) {
        case kit::kDebugColliders: return "colliders";
        case kit::kDebugBodies: return "bodies";
        case kit::kDebugTriggers: return "triggers";
        case kit::kDebugJoints: return "joints";
        case kit::kDebugCharacters: return "characters";
        case kit::kDebugVehicles: return "vehicles";
        case kit::kDebugPaths: return "paths";
        default: return "";
    }
}

void write_vec(JsonWriter& w, math::vec3 v) {
    w.begin_array().num_f(static_cast<f64>(v.x)).num_f(static_cast<f64>(v.y)).num_f(static_cast<f64>(v.z)).end_array();
}

bool debug_lines_tool(ToolCall& c) {
    u64 ticks = 1;
    if (!arg_uint(c, "ticks", ticks, 1, kMaxTicks, false)) return false;
    u64 limit = kMaxLines;
    if (!arg_uint(c, "limit", limit, 1, kMaxLines, false)) return false;
    u32 mask;
    if (!arg_categories(c, mask)) return false;
    Array<kit::GameplayLine> lines;
    u64 played = 0;
    if (!play_lines(c, mask, ticks, lines, played)) return false;
    c.out.begin_object();
    c.out.key("ticks").num_u(played).key("total").num_u(lines.size());
    c.out.key("counts").begin_object();
    for (u32 bit = 1; bit <= kit::kDebugPaths; bit <<= 1) {
        if (!(mask & bit)) continue;
        u32 n = 0;
        for (usize i = 0; i < lines.size(); ++i) n += lines[i].category == bit ? 1u : 0u;
        c.out.key(category_name(bit)).num_u(n);
    }
    c.out.end_object();
    c.out.key("lines").begin_array();
    for (usize i = 0; i < lines.size() && i < limit; ++i) {
        const kit::GameplayLine& l = lines[i];
        c.out.begin_object().key("category").str(category_name(l.category)).key("from");
        write_vec(c.out, l.a);
        c.out.key("to");
        write_vec(c.out, l.b);
        c.out.key("color").begin_array();
        c.out.num_f(static_cast<f64>(l.color.r)).num_f(static_cast<f64>(l.color.g)).num_f(static_cast<f64>(l.color.b)).num_f(static_cast<f64>(l.color.a));
        c.out.end_array().end_object();
    }
    c.out.end_array();
    c.out.key("truncated").boolean(lines.size() > limit);
    c.out.end_object();
    return true;
}

u32 pack(const math::Color& col) {
    auto ch = [](f32 v) { return static_cast<u32>(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
    return ch(col.r) | (ch(col.g) << 8) | (ch(col.b) << 16) | 0xFF000000u;
}

void put(cook::Image& img, i32 x, i32 y, u32 rgba) {
    if (x < 0 || y < 0 || x >= static_cast<i32>(img.width) || y >= static_cast<i32>(img.height)) return;
    u8* p = &img.rgba[(static_cast<usize>(y) * img.width + static_cast<usize>(x)) * 4];
    p[0] = static_cast<u8>(rgba & 0xFF);
    p[1] = static_cast<u8>((rgba >> 8) & 0xFF);
    p[2] = static_cast<u8>((rgba >> 16) & 0xFF);
    p[3] = 0xFF;
}

void draw_line(cook::Image& img, i32 x0, i32 y0, i32 x1, i32 y1, u32 rgba) {
    const i32 dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const i32 sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    i32 err = dx + dy;
    for (u32 guard = 0; guard < 20000; ++guard) {
        put(img, x0, y0, rgba);
        if (x0 == x1 && y0 == y1) break;
        const i32 e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

bool debug_view_tool(ToolCall& c) {
    u64 ticks = 1;
    if (!arg_uint(c, "ticks", ticks, 1, kMaxTicks, false)) return false;
    u64 width = 512, height = 512;
    if (!arg_uint(c, "width", width, 64, 2048, false) || !arg_uint(c, "height", height, 64, 2048, false)) return false;
    u32 mask;
    if (!arg_categories(c, mask)) return false;
    StringView view = "top";
    if (!arg_string(c, "view", view, false)) return false;
    if (view != "top" && view != "front" && view != "side") return fail(c, "view must be top, front or side");
    Array<kit::GameplayLine> lines;
    u64 played = 0;
    if (!play_lines(c, mask, ticks, lines, played)) return false;

    // top: x right, z down; front: x right, y up; side: z right, y up.
    auto project = [&](math::vec3 p, f64& u, f64& v) {
        if (view == "top") {
            u = static_cast<f64>(p.x);
            v = static_cast<f64>(p.z);
        } else if (view == "front") {
            u = static_cast<f64>(p.x);
            v = -static_cast<f64>(p.y);
        } else {
            u = static_cast<f64>(p.z);
            v = -static_cast<f64>(p.y);
        }
    };
    f64 min_u = 0, max_u = 0, min_v = 0, max_v = 0;
    bool first = true;
    for (usize i = 0; i < lines.size(); ++i) {
        for (const math::vec3 p : {lines[i].a, lines[i].b}) {
            f64 u, v;
            project(p, u, v);
            if (first) {
                min_u = max_u = u;
                min_v = max_v = v;
                first = false;
            }
            min_u = std::fmin(min_u, u);
            max_u = std::fmax(max_u, u);
            min_v = std::fmin(min_v, v);
            max_v = std::fmax(max_v, v);
        }
    }
    const f64 span = std::fmax(std::fmax(max_u - min_u, max_v - min_v), 1.0);
    const f64 pad = 16.0;
    const f64 scale = (static_cast<f64>(std::fmin(static_cast<f64>(width), static_cast<f64>(height))) - 2.0 * pad) / span;
    cook::Image img = make_image(static_cast<u32>(width), static_cast<u32>(height), 0xFF202020u);
    auto px = [&](math::vec3 p, i32& x, i32& y) {
        f64 u, v;
        project(p, u, v);
        x = static_cast<i32>(std::lround(pad + (u - min_u) * scale));
        y = static_cast<i32>(std::lround(pad + (v - min_v) * scale));
    };
    for (usize i = 0; i < lines.size(); ++i) {
        i32 x0, y0, x1, y1;
        px(lines[i].a, x0, y0);
        px(lines[i].b, x1, y1);
        draw_line(img, x0, y0, x1, y1, pack(lines[i].color));
    }
    char label[64];
    std::snprintf(label, sizeof(label), "%.*s %zu LINES", static_cast<int>(view.size()), view.data(), static_cast<size_t>(lines.size()));
    draw_label(img, 4, 4, StringView(label), 2, 0xFFFFFFFFu);
    if (c.content) {
        ToolContent item;
        item.mime = cook::String("image/png");
        if (!png_base64(img, item.base64)) return fail(c, "png encode failed");
        c.content->push_back(static_cast<ToolContent&&>(item));
    }
    c.out.begin_object();
    c.out.key("view").str(view).key("ticks").num_u(played).key("lines").num_u(lines.size());
    c.out.key("width").num_u(width).key("height").num_u(height);
    c.out.key("pixels_per_metre").num_f(scale);
    c.out.key("bounds").begin_array().num_f(min_u).num_f(min_v).num_f(max_u).num_f(max_v).end_array();
    c.out.end_object();
    return true;
}

#define MCE_DEBUGVIEW_ARGS                                                                                         \
    R"j("ticks":{"type":"integer","minimum":1,"maximum":36000,"description":"Ticks to play before reading (default 1)"},)j" \
    R"j("categories":{"type":"array","items":{"type":"string","enum":["colliders","bodies","triggers","joints","characters","vehicles","paths"]},"description":"What to draw (default all)"})j"

const ToolDef kDebugViewTools[] = {
    {"debug_lines",
     "Play the saved scene headless for 'ticks' (default 1) and report the gameplay debugger's lines as data: box "
     "colliders, rigid bodies, triggers (green while something is inside), joints (anchor, axis, link; red once "
     "broken), character capsules (orange in the air), vehicle wheels (red while skidding), waypoint chains with each "
     "driver's target, and nav agents' next corner. 'categories' limits what is drawn. Each line has category, "
     "from/to in world space and an RGBA color; 'counts' gives the number per category, 'limit' (default and maximum "
     "2000) caps the listed lines. Read-only.",
     R"j({"type":"object","properties":{)j" MCE_DEBUGVIEW_ARGS
     R"j(,"limit":{"type":"integer","minimum":1,"maximum":2000}}})j",
     debug_lines_tool},
    {"debug_view",
     "The same lines as debug_lines, drawn by a CPU rasteriser (no GPU needed) from 'view' top (x right, z down), "
     "front (x right, y up) or side (z right, y up), fitted to the picture, and returned as a PNG ('width' x "
     "'height', default 512). Use it to see where colliders, triggers, joints and paths really are.",
     R"j({"type":"object","properties":{)j" MCE_DEBUGVIEW_ARGS
     R"j(,"view":{"type":"string","enum":["top","front","side"]},"width":{"type":"integer","minimum":64,"maximum":2048},)j"
     R"j("height":{"type":"integer","minimum":64,"maximum":2048}}})j",
     debug_view_tool},
};

}  // namespace

Span<const ToolDef> debugview_tool_table() { return Span<const ToolDef>(kDebugViewTools, sizeof(kDebugViewTools) / sizeof(kDebugViewTools[0])); }

}  // namespace mce::mcp
