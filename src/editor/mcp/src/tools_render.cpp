// M-AGENT eyes (SBA-48): render_frame, render_sheet and image_diff. The frame comes from the real
// player: the server cooks, then runs the player (mce_player, or the project's host) on the cooked
// tree with -capture_ticks, the run's scripted input and an optional camera override, reads the
// captured PPM back and returns it as MCP image content. With no display (a cloud box) the player
// runs on SDL's offscreen driver with Mesa's software rasterizer, so a GPU is never required.

#include <cstdio>
#include <cstdlib>

#include "agent_image.h"
#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/platform/library.h"
#include "mce/platform/platform.h"
#include "mce/platform/process.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

#if defined(_WIN32)
constexpr const char* kPlayerExe = "mce_player.exe";
#else
constexpr const char* kPlayerExe = "mce_player";
#endif

}  // namespace

// The player the frames come from: --player, else the project host the server was built with, else
// mce_player beside the server (installed SDK) or in the build tree.
bool find_player(const Server& server, cook::String& out) {
    if (server.options().player && *server.options().player) {
        out = cook::String(server.options().player);
        return platform::file_exists(out.view());
    }
    const char* candidates[] = {"", "../../player/", "../player/", "../bin/"};
    for (const char* rel : candidates) {
        out = cook::String(platform::base_path());
        out += rel;
        out += kPlayerExe;
        if (platform::file_exists(out.view())) return true;
    }
    return false;
}

// No display to open a window on: the offscreen video driver and Mesa's software rasterizer.
bool headless_display() {
#if defined(__linux__)
    const char* x = std::getenv("DISPLAY");
    const char* wl = std::getenv("WAYLAND_DISPLAY");
    return (!x || !*x) && (!wl || !*wl);
#else
    return false;
#endif
}

void script_text(const input::InputScript& s, cook::String& out) {
    for (const input::ScriptSegment& seg : s.segments()) {
        cook::appendf(out, "%llu", static_cast<unsigned long long>(seg.from));
        if (seg.to == ~u64(0)) out += "-";
        else if (seg.to != seg.from) cook::appendf(out, "-%llu", static_cast<unsigned long long>(seg.to));
        out += ' ';
        out += seg.name.view();
        if (seg.held) cook::appendf(out, "=%g,%g\n", static_cast<f64>(seg.x), static_cast<f64>(seg.y));
        else out += "=false\n";  // named, not held ("=0" would press it with value 0)
    }
}

// Runs the player to its exit with Mesa's software switch set around the spawn when `software`
// (the child inherits the environment). False with c.error set when it cannot start.
bool spawn_player(ToolCall& c, StringView player, Span<const StringView> args, bool software, Array<char>& output, i32& code) {
    const char* had = std::getenv("LIBGL_ALWAYS_SOFTWARE");
    cook::String saved(had ? had : "");
#if !defined(_WIN32)
    if (software) setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
#endif
    platform::Process proc;
    Result<void> started = proc.start(args, true);
#if !defined(_WIN32)
    if (software) {
        if (had) setenv("LIBGL_ALWAYS_SOFTWARE", saved.c_str(), 1);
        else unsetenv("LIBGL_ALWAYS_SOFTWARE");
    }
#endif
    if (!started) {
        c.error += "cannot start the player ";
        c.error += player;
        return false;
    }
    code = proc.wait(&output);
    return true;
}

namespace {

constexpr u64 kMaxTick = 36000;
constexpr u64 kMaxSide = 3840;
constexpr usize kMaxSheetTicks = 16;
constexpr usize kMaxSheetCameras = 4;
constexpr u32 kSafeAreaColor = 0x3FD0FFFFu;

struct RenderRequest {
    cook::String scene_rel;  // played scene, path under assets/ (a copy: the cook a render runs rescans the project's assets)
    asset::Guid scene_guid;
    u32 width = 960, height = 540;
    Array<u64> ticks;        // ascending
    input::InputScript script;
    cook::String view_args;  // "-view.entity" / "-view.free" value
    const char* view_flag = nullptr;
    i64 safe[4] = {-1, -1, -1, -1};
    bool software = false;
    cook::String pack_dir;    // a cooked dir to play instead of the project's (ui_preview's document-only cook)
    cook::String ui_preview;  // -ui_preview: a UiDocument GUID drawn alone
    cook::String ui_scale;    // -ui_scale, when set
    ConvarArgs convars;       // -<name> <value> for the player
};

bool arg_size(ToolCall& c, const char* wname, const char* hname, u32& w, u32& h) {
    u64 ww = w, hh = h;
    if (!arg_uint(c, wname, ww, 64, kMaxSide, false) || !arg_uint(c, hname, hh, 64, kMaxSide, false)) return false;
    w = static_cast<u32>(ww);
    h = static_cast<u32>(hh);
    return true;
}

bool numbers(const cook::JsonValue& v, usize n, f64* out) {
    if (!v.is_array() || v.size() != n) return false;
    for (u32 i = 0; i < n; ++i) {
        if (!v.at(i).is_number()) return false;
        out[i] = v.at(i).as_number();
    }
    return true;
}

// `camera`: {"entity": id|name} (a camera entity of the open scene), {"index": n} (scene entity
// index in file order), {"position": [x,y,z], "target": [x,y,z], "fov": deg} (a free 3D camera) or
// {"center": [x,y], "zoom": z} (a free 2D camera).
bool parse_camera(ToolCall& c, const cook::JsonValue& cam, bool open_scene, RenderRequest& rq) {
    if (!cam.valid()) return true;
    if (!cam.is_object()) return arg_missing(c, "camera", "an object: {entity}, {index}, {position, target, fov} or {center, zoom}");
    const cook::JsonValue ent = cam.get("entity"), idx = cam.get("index"), pos = cam.get("position"), ctr = cam.get("center");
    rq.view_args = cook::String();
    if (ent.valid()) {
        if (!open_scene) return fail(c, "camera.entity names an entity of the open scene; with 'scene' use camera.index");
        editor::EntityId id = editor::kNoEntity;
        if (!resolve_entity(c, "camera.entity", ent, id)) return false;
        const Span<const editor::DocEntity> all = scene(c).doc().entities();
        u64 index = 0;
        for (const editor::DocEntity& e : all) {
            if (e.id == id) break;
            if (!e.prefab.is_null())
                return fail(c, "camera.entity: a prefab instance comes before it in the file, so its run index differs; give camera.index");
            ++index;
        }
        cook::appendf(rq.view_args, "%llu", static_cast<unsigned long long>(index));
        rq.view_flag = "-view.entity";
        return true;
    }
    if (idx.valid()) {
        if (!idx.is_integer() || idx.as_number() < 0 || idx.as_number() > 1e6) return arg_missing(c, "camera.index", "an entity index >= 0");
        cook::appendf(rq.view_args, "%llu", static_cast<unsigned long long>(idx.as_number()));
        rq.view_flag = "-view.entity";
        return true;
    }
    if (pos.valid()) {
        f64 p[3], t[3];
        if (!numbers(pos, 3, p) || !numbers(cam.get("target"), 3, t))
            return arg_missing(c, "camera.position / camera.target", "[x, y, z] arrays (both)");
        f64 fov = 60.0;
        if (const cook::JsonValue f = cam.get("fov"); f.valid()) {
            if (!f.is_number() || f.as_number() < 5 || f.as_number() > 170) return arg_missing(c, "camera.fov", "degrees in [5, 170]");
            fov = f.as_number();
        }
        cook::appendf(rq.view_args, "%g,%g,%g,%g,%g,%g,%g", p[0], p[1], p[2], t[0], t[1], t[2], fov);
        rq.view_flag = "-view.free";
        return true;
    }
    if (ctr.valid()) {
        f64 p[2];
        if (!numbers(ctr, 2, p)) return arg_missing(c, "camera.center", "[x, y]");
        cook::appendf(rq.view_args, "%g,%g", p[0], p[1]);
        if (const cook::JsonValue z = cam.get("zoom"); z.valid()) {
            if (!z.is_number() || z.as_number() <= 0) return arg_missing(c, "camera.zoom", "a number > 0");
            cook::appendf(rq.view_args, ",%g", z.as_number());
        }
        rq.view_flag = "-view.free";
        return true;
    }
    return arg_missing(c, "camera", "one of {entity}, {index}, {position, target, fov} or {center, zoom}");
}

// The arguments render_frame and render_sheet share: scene, input, camera, safe_area, software.
bool parse_common(ToolCall& c, RenderRequest& rq) {
    if (!need_project(c)) return false;
    StringView other;
    if (!arg_string(c, "scene", other, false)) return false;
    const Server::SceneFile& file = c.server.scene_file();
    rq.scene_rel = cook::String(file.rel.view());
    rq.scene_guid = scene(c).doc().guid();
    if (!other.empty() && other != file.rel.view()) {
        cook::String abs;
        if (!scene_path(c.server, other, abs, c.error)) return false;
        const editor::ProjectAsset* found = nullptr;
        for (const editor::ProjectAsset& a : c.server.session().project().assets())
            if (a.type == asset::ChunkType::Scene && a.path.view() == other) found = &a;
        if (!found) return fail(c, "argument 'scene': no such scene in the project (asset_list with type Scene lists them)");
        rq.scene_rel = cook::String(other);
        rq.scene_guid = found->guid;
    } else {
        if (file.rel.empty()) return fail(c, "the scene was never saved: scene_save it (with a path under assets/) first");
        if (scene(c).dirty()) return fail(c, "the scene has unsaved changes: scene_save first (the frame is drawn from the cooked file)");
    }
    if (!arg_input(c, rq.script) || !arg_convars(c, rq.convars)) return false;
    if (!parse_camera(c, arg(c, "camera"), rq.scene_rel.view() == file.rel.view(), rq)) return false;
    if (const cook::JsonValue sa = arg(c, "safe_area"); sa.valid()) {
        static constexpr const char* kKeys[4] = {"left", "top", "right", "bottom"};
        if (!sa.is_object()) return arg_missing(c, "safe_area", "an object {left, top, right, bottom} in pixels");
        for (u32 i = 0; i < 4; ++i) {
            const cook::JsonValue v = sa.get(kKeys[i]);
            if (!v.valid()) {
                rq.safe[i] = 0;
                continue;
            }
            if (!v.is_integer() || v.as_number() < 0 || v.as_number() > 2000)
                return arg_missing(c, "safe_area values", "whole pixels in [0, 2000]");
            rq.safe[i] = static_cast<i64>(v.as_number());
        }
    }
    return arg_bool(c, "software", rq.software);
}

// Cooks the project, runs the player once and reads one frame per requested tick.
bool render_ticks(ToolCall& c, RenderRequest& rq, Array<cook::Image>& frames, bool& used_software) {
    if (rq.pack_dir.empty()) {
        const cook::CookReport& r = c.server.session().cook(false);
        if (!r.ok()) {
            c.error += "cook failed:";
            append_diags(c.error, r.diags, 10);
            return false;
        }
    }
    cook::String player;
    if (!find_player(c.server, player))
        return fail(c, "no player to render with: start the server with --player <mce_player or the game's host>");
    const StringView root = c.server.session().project().root();
    const cook::String cooked = rq.pack_dir.empty() ? cook::path_join(root, "cooked") : rq.pack_dir;
    const cook::String work = cook::path_join(cook::path_join(root, "cooked").view(), ".render");
    if (Result<void> mk = cook::fs::create_directories(work.view()); !mk) return fail(c, "cannot create cooked/.render");
    const cook::String shot = cook::path_join(work.view(), "frame_{tick}.ppm");
    for (u64 t : rq.ticks) {
        cook::String old = cook::path_join(work.view(), "frame_");
        cook::appendf(old, "%llu.ppm", static_cast<unsigned long long>(t));
        (void)cook::fs::remove(old.view());
    }

    cook::String ticks, window, guid(asset::to_string(rq.scene_guid).view()), safe, input_path;
    for (usize i = 0; i < rq.ticks.size(); ++i) cook::appendf(ticks, i ? ",%llu" : "%llu", static_cast<unsigned long long>(rq.ticks[i]));
    cook::appendf(window, "%ux%u", rq.width, rq.height);
    Array<StringView> args;
    args.push_back(player.view());
    for (StringView a : {StringView("-pack"), cooked.view(), StringView("-scene"), guid.view(), StringView("-window"), window.view(),
                         StringView("-capture_ticks"), ticks.view(), StringView("-capture"), shot.view()})
        args.push_back(a);
    // A preview, not a session: no focus pause, no mouse grab, no error dialogs, no vsync wait, and
    // the sim runs ahead of the wall clock to the captured ticks (32 a frame).
    for (StringView a : {StringView("-pause_on_focus_loss"), StringView("0"), StringView("-mouse_look"), StringView("0"),
                         StringView("-platform.error_box"), StringView("0"), StringView("-r.vsync"), StringView("0"),
                         StringView("-console_open"), StringView("0"), StringView("-sim.fast_forward"), StringView("1"),
                         StringView("-sim.max_catchup"), StringView("32")})
        args.push_back(a);
    if (c.server.module().loaded()) {
        args.push_back("-module");
        args.push_back(c.server.module().source_path.view());
    }
    if (!rq.script.empty()) {
        cook::String text;
        script_text(rq.script, text);
        input_path = cook::path_join(work.view(), "input.txt");
        const Span<const byte> bytes(reinterpret_cast<const byte*>(text.view().data()), text.view().size());
        if (!cook::fs::write_file_atomic(input_path.view(), bytes)) return fail(c, "cannot write cooked/.render/input.txt");
        args.push_back("-input");
        args.push_back(input_path.view());
    }
    if (!rq.ui_preview.empty()) {
        args.push_back("-ui_preview");
        args.push_back(rq.ui_preview.view());
    }
    if (!rq.ui_scale.empty()) {
        args.push_back("-ui_scale");
        args.push_back(rq.ui_scale.view());
    }
    if (rq.view_flag) {
        args.push_back(rq.view_flag);
        args.push_back(rq.view_args.view());
    }
    if (rq.safe[0] >= 0) {
        cook::appendf(safe, "%lld,%lld,%lld,%lld", static_cast<long long>(rq.safe[0]), static_cast<long long>(rq.safe[1]),
                      static_cast<long long>(rq.safe[2]), static_cast<long long>(rq.safe[3]));
        args.push_back("-safe_area");
        args.push_back(safe.view());
    }
    Array<cook::String> flags;  // "-<name>": the player reads convars as command-line flags
    for (const cook::String& n : rq.convars.names) {
        cook::String f("-");
        f += n.view();
        flags.push_back(std::move(f));
    }
    for (usize i = 0; i < flags.size(); ++i) {
        args.push_back(flags[i].view());
        args.push_back(rq.convars.values[i].view());
    }
    used_software = rq.software || headless_display();
    if (used_software) {
        args.push_back("-platform.video_driver");
        args.push_back("offscreen");
    }

    Array<char> output;
    i32 code = 0;
    if (!spawn_player(c, player.view(), args.span(), used_software, output, code)) return false;
    frames.clear();
    for (u64 t : rq.ticks) {
        cook::String path = cook::path_join(work.view(), "frame_");
        cook::appendf(path, "%llu.ppm", static_cast<unsigned long long>(t));
        Result<cook::Image> img = read_ppm(path.view());
        if (!img) {
            cook::appendf(c.error, "the player (exit %d) wrote no frame for tick %llu; its last output:\n", code,
                          static_cast<unsigned long long>(t));
            // The tail of what it said, which names the failure (a missing scene, no GL context, ...).
            usize from = output.size() > 1500 ? output.size() - 1500 : 0;
            c.error += StringView(output.data() + from, output.size() - from);
            return false;
        }
        (void)cook::fs::remove(path.view());
        frames.push_back(static_cast<cook::Image&&>(img.value()));
    }
    return true;
}

void draw_safe_area(cook::Image& img, const RenderRequest& rq) {
    if (rq.safe[0] < 0) return;
    const i32 l = static_cast<i32>(rq.safe[0]), t = static_cast<i32>(rq.safe[1]);
    const i32 w = static_cast<i32>(img.width) - l - static_cast<i32>(rq.safe[2]);
    const i32 h = static_cast<i32>(img.height) - t - static_cast<i32>(rq.safe[3]);
    if (w > 0 && h > 0) outline_rect(img, l, t, w, h, kSafeAreaColor, 2, 200);
}

bool attach_image(ToolCall& c, const cook::Image& img) {
    if (!c.content) return true;
    ToolContent item;
    item.mime = cook::String("image/png");
    if (!png_base64(img, item.base64)) return fail(c, "png encode failed");
    c.content->push_back(static_cast<ToolContent&&>(item));
    return true;
}

// A path under the project folder for a PNG the tool reads or writes ("goldens/title.png").
bool project_png_path(ToolCall& c, const char* name, StringView rel, cook::String& abs) {
    bool ok = !rel.empty() && rel.size() < 400 && rel[0] != '/' && rel.find('\\') == StringView::npos &&
              rel.find(':') == StringView::npos && rel.size() > 4 && rel.substr(rel.size() - 4) == ".png";
    for (usize start = 0; ok && start <= rel.size();) {
        usize end = rel.find('/', start);
        if (end == StringView::npos) end = rel.size();
        const StringView seg = rel.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..") ok = false;
        start = end + 1;
    }
    if (!ok) {
        c.error += "argument '";
        c.error += name;
        c.error += "' must be a .png path inside the project folder, like \"goldens/title.png\" (forward slashes, no '..')";
        return false;
    }
    abs = cook::path_join(c.server.session().project().root(), rel);
    return true;
}

void write_result_head(ToolCall& c, const RenderRequest& rq, bool software) {
    c.out.key("scene").str(rq.scene_rel.view());
    c.out.key("width").num_u(rq.width).key("height").num_u(rq.height);
    c.out.key("renderer").str(software ? "software (offscreen, Mesa llvmpipe)" : "gpu (a window)");
}

}  // namespace

bool render_frame(ToolCall& c) {
    RenderRequest rq;
    u64 tick = 1;
    if (!arg_uint(c, "tick", tick, 1, kMaxTick, false) || !arg_size(c, "width", "height", rq.width, rq.height) ||
        !parse_common(c, rq))
        return false;
    StringView save;
    if (!arg_string(c, "save", save, false)) return false;
    cook::String save_abs;
    if (!save.empty() && !project_png_path(c, "save", save, save_abs)) return false;
    rq.ticks.push_back(tick);
    Array<cook::Image> frames;
    bool software = false;
    if (!render_ticks(c, rq, frames, software)) return false;
    cook::Image& img = frames[0];
    if (!save.empty()) {
        if (Result<void> w = write_png_file(save_abs.view(), img); !w) return fail(c, "cannot write the 'save' PNG");
    }
    draw_safe_area(img, rq);
    c.out.begin_object();
    write_result_head(c, rq, software);
    c.out.key("tick").num_u(tick);
    if (!save.empty()) c.out.key("saved").str(save);
    c.out.end_object();
    return attach_image(c, img);
}

// A cutscene at a moment (M-CINEMA step 6): the player starts with every timeline in the scene
// paused at `time` seconds (timeline.scrub: the state tracks applied, cameras placed) and the frame
// after `tick` is captured. The playhead stays put, so the frame is the timeline at `time`.
bool timeline_preview(ToolCall& c) {
    RenderRequest rq;
    const cook::JsonValue tv = arg(c, "time");
    if (!tv.valid() || !tv.is_number() || tv.as_number() < 0 || tv.as_number() > 3600)
        return arg_missing(c, "time", "seconds into the timeline, a number in [0, 3600]");
    u64 tick = 1;
    if (!arg_uint(c, "tick", tick, 1, kMaxTick, false) || !arg_size(c, "width", "height", rq.width, rq.height) ||
        !parse_common(c, rq))
        return false;
    cook::String value;
    cook::appendf(value, "%g", tv.as_number());
    rq.convars.names.push_back(cook::String("timeline.scrub"));
    rq.convars.values.push_back(static_cast<cook::String&&>(value));
    StringView save;
    if (!arg_string(c, "save", save, false)) return false;
    cook::String save_abs;
    if (!save.empty() && !project_png_path(c, "save", save, save_abs)) return false;
    rq.ticks.push_back(tick);
    Array<cook::Image> frames;
    bool software = false;
    if (!render_ticks(c, rq, frames, software)) return false;
    cook::Image& img = frames[0];
    if (!save.empty()) {
        if (Result<void> w = write_png_file(save_abs.view(), img); !w) return fail(c, "cannot write the 'save' PNG");
    }
    draw_safe_area(img, rq);
    c.out.begin_object();
    write_result_head(c, rq, software);
    c.out.key("time").num_f(tv.as_number());
    if (!save.empty()) c.out.key("saved").str(save);
    c.out.end_object();
    return attach_image(c, img);
}

// A UI document and what it needs to draw, cooked alone (M-UI-DATA2 4): the document, and by GUID
// every theme, font, sprite sheet and texture named in its text and, in turn, in theirs, copied with
// their sidecars into cooked/.render/ui_only/assets with an empty scene for the player to start,
// then cooked there. A broken scene or asset elsewhere in the project cannot block the preview.
bool cook_ui_only(ToolCall& c, const UiDocRef& doc, RenderRequest& rq) {
    const editor::EditorProject& project = c.server.session().project();
    Array<const editor::ProjectAsset*> need;
    const editor::ProjectAsset* self = project.find(doc.guid);
    if (!self) return fail(c, "the UI document is no longer in the project");
    need.push_back(self);
    for (usize i = 0; i < need.size(); ++i) {
        // Only the engine's own text formats (.mcui, .mcuitheme, .mcsheet) name other assets.
        const StringView rel = need[i]->path.view();
        const usize dot = rel.rfind('.');
        if (dot == StringView::npos || rel.substr(dot, 3) != ".mc") continue;
        cook::String text;
        if (!cook::fs::read_text(project.asset_path(rel).view(), text)) continue;
        for (usize at = text.view().find("guid:"); at != StringView::npos; at = text.view().find("guid:", at + 5)) {
            const Result<asset::Guid> g = asset::parse_guid(text.view().substr(at + 5, 36));
            const editor::ProjectAsset* a = g ? project.find(g.value()) : nullptr;
            bool known = a == nullptr;
            for (usize k = 0; !known && k < need.size(); ++k) known = need[k]->guid == a->guid;
            if (!known) need.push_back(a);
        }
    }
    const cook::String base = cook::path_join(cook::path_join(cook::path_join(project.root(), "cooked").view(), ".render").view(), "ui_only");
    const cook::String assets = cook::path_join(base.view(), "assets");
    {
        Array<cook::String> old;  // last preview's copies: a smaller closure must not keep old assets
        if (cook::fs::is_directory(assets.view()) && cook::fs::list_files(assets.view(), old))
            for (const cook::String& f : old) (void)cook::fs::remove(cook::path_join(assets.view(), f.view()).view());
    }
    auto copy = [&](StringView rel) {
        cook::String bytes;
        if (!cook::fs::read_text(project.asset_path(rel).view(), bytes)) return false;
        const Span<const byte> data(reinterpret_cast<const byte*>(bytes.c_str()), bytes.size());
        return static_cast<bool>(cook::fs::write_file_atomic(cook::path_join(assets.view(), rel).view(), data));
    };
    for (const editor::ProjectAsset* a : need) {
        cook::String side(a->path.view());
        side += ".mceasset";
        // Native documents carry their GUID inside and may have no sidecar; textures and fonts do.
        if (!copy(a->path.view())) {
            c.error += "cannot copy '";
            c.error += a->path.view();
            c.error += "' into cooked/.render/ui_only";
            return false;
        }
        (void)copy(side.view());
    }
    // The player needs a scene to start; this one is empty.
    static constexpr const char* kEmptyScene = "75e3e000-0000-4000-8000-000000000001";
    cook::String scene = "{\n  \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"";
    scene += kEmptyScene;
    scene += "\", \"name\": \"ui_only\",\n  \"entities\": []\n}\n";
    const Span<const byte> scene_bytes(reinterpret_cast<const byte*>(scene.c_str()), scene.size());
    if (!cook::fs::write_file_atomic(cook::path_join(assets.view(), "ui_only.mcscene").view(), scene_bytes))
        return fail(c, "cannot write cooked/.render/ui_only/assets");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(base.view(), cook::CookOptions{});
    if (!r.ok()) {
        c.error += "cook of the document alone failed:";
        append_diags(c.error, r.diags, 10);
        return false;
    }
    rq.pack_dir = cook::path_join(base.view(), "cooked");
    rq.scene_guid = asset::parse_guid(kEmptyScene).value();
    return true;
}

// A UI document alone (M-UI-DATA step 5): the player's -ui_preview over the camera's clear color,
// at a viewport, safe area and UI scale. Bindings read the scene the player starts (the open one
// when it is saved, else the project's first scene). When the project does not cook (a broken
// scene, say), or has no saved scene, or "document_only": true, only the document and what it
// references are cooked and the player starts an empty scene: bindings then show nothing.
bool ui_preview(ToolCall& c) {
    RenderRequest rq;
    rq.width = 960;
    rq.height = 540;
    if (!need_project(c) || !arg_size(c, "width", "height", rq.width, rq.height)) return false;
    UiDocRef doc;  // by value: the cook inside render_ticks rescans the project and frees its records
    if (!ui_document_arg(c, doc)) return false;
    rq.ui_preview = cook::String(asset::to_string(doc.guid).view());
    bool only = false;
    if (!arg_bool(c, "document_only", only)) return false;
    const char* why = only ? "document_only was set" : nullptr;
    if (!only && !c.server.session().cook(false).ok()) why = "the project does not cook (cook_errors lists why)";
    const Server::SceneFile& file = c.server.scene_file();
    if (why) {
        // Nothing of the project's scenes is needed.
    } else if (!file.rel.empty() && !scene(c).dirty()) {
        rq.scene_rel = cook::String(file.rel.view());
        rq.scene_guid = scene(c).doc().guid();
    } else {
        for (const editor::ProjectAsset& a : c.server.session().project().assets())
            if (a.type == asset::ChunkType::Scene && (rq.scene_rel.empty() || a.path.view() < rq.scene_rel.view())) {
                rq.scene_rel = cook::String(a.path.view());
                rq.scene_guid = a.guid;
            }
        if (rq.scene_rel.empty()) why = "the project has no saved scene to start";
    }
    if (why && !cook_ui_only(c, doc, rq)) return false;
    if (const cook::JsonValue sa = arg(c, "safe_area"); sa.valid()) {
        static constexpr const char* kKeys[4] = {"left", "top", "right", "bottom"};
        if (!sa.is_object()) return arg_missing(c, "safe_area", "an object {left, top, right, bottom} in pixels");
        for (u32 i = 0; i < 4; ++i) {
            const cook::JsonValue v = sa.get(kKeys[i]);
            rq.safe[i] = 0;
            if (!v.valid()) continue;
            if (!v.is_integer() || v.as_number() < 0 || v.as_number() > 2000)
                return arg_missing(c, "safe_area values", "whole pixels in [0, 2000]");
            rq.safe[i] = static_cast<i64>(v.as_number());
        }
    }
    if (const cook::JsonValue s = arg(c, "ui_scale"); s.valid()) {
        if (!s.is_number() || s.as_number() < 0.25 || s.as_number() > 4) return arg_missing(c, "ui_scale", "a number in [0.25, 4]");
        cook::appendf(rq.ui_scale, "%g", s.as_number());
    }
    if (!arg_bool(c, "software", rq.software)) return false;
    StringView save;
    if (!arg_string(c, "save", save, false)) return false;
    cook::String save_abs;
    if (!save.empty() && !project_png_path(c, "save", save, save_abs)) return false;
    rq.ticks.push_back(2);  // the second frame: the first lays the document out
    Array<cook::Image> frames;
    bool software = false;
    if (!render_ticks(c, rq, frames, software)) return false;
    cook::Image& img = frames[0];
    if (!save.empty()) {
        if (Result<void> w = write_png_file(save_abs.view(), img); !w) return fail(c, "cannot write the 'save' PNG");
    }
    draw_safe_area(img, rq);
    c.out.begin_object();
    if (!save.empty()) c.out.key("saved").str(save);
    c.out.key("document").str(doc.path.view()).key("guid").str(asset_guid_ref(doc.guid).view());
    if (why) c.out.key("document_only").str(why);
    write_result_head(c, rq, software);
    c.out.end_object();
    return attach_image(c, img);
}

bool render_sheet(ToolCall& c) {
    RenderRequest base;
    base.width = 320;
    base.height = 180;
    if (!arg_size(c, "cell_width", "cell_height", base.width, base.height)) return false;
    const cook::JsonValue ticks = arg(c, "ticks");
    if (!ticks.is_array() || ticks.size() == 0 || ticks.size() > kMaxSheetTicks)
        return arg_missing(c, "ticks", "an array of 1-16 ascending ticks");
    for (u32 i = 0; i < ticks.size(); ++i) {
        const cook::JsonValue t = ticks.at(i);
        if (!t.is_integer() || t.as_number() < 1 || t.as_number() > static_cast<f64>(kMaxTick) ||
            (i > 0 && t.as_number() <= static_cast<f64>(base.ticks.back())))
            return arg_missing(c, "ticks", "ascending ticks in [1, 36000]");
        base.ticks.push_back(static_cast<u64>(t.as_number()));
    }
    // Each camera is one player run (its own row); none: the scene's own camera.
    const cook::JsonValue cams = arg(c, "cameras");
    if (cams.valid() && (!cams.is_array() || cams.size() == 0 || cams.size() > kMaxSheetCameras))
        return arg_missing(c, "cameras", "an array of 1-4 camera objects (as render_frame's camera)");
    if (!parse_common(c, base)) return false;
    const u32 rows = cams.valid() ? cams.size() : 1;
    const u32 cols = static_cast<u32>(base.ticks.size());
    constexpr u32 kGap = 4;
    cook::Image sheet = make_image(cols * base.width + (cols + 1) * kGap, rows * base.height + (rows + 1) * kGap, 0x202024FFu);
    bool software = false;
    for (u32 row = 0; row < rows; ++row) {
        RenderRequest rq;
        rq.scene_rel = cook::String(base.scene_rel.view());
        rq.scene_guid = base.scene_guid;
        rq.width = base.width;
        rq.height = base.height;
        rq.ticks = base.ticks;
        rq.software = base.software;
        for (u32 i = 0; i < 4; ++i) rq.safe[i] = base.safe[i];
        for (const input::ScriptSegment& s : base.script.segments()) rq.script.add(s);
        for (usize i = 0; i < base.convars.names.size(); ++i) {
            rq.convars.names.push_back(cook::String(base.convars.names[i].view()));
            rq.convars.values.push_back(cook::String(base.convars.values[i].view()));
        }
        if (cams.valid() && !parse_camera(c, cams.at(row), base.scene_rel.view() == c.server.scene_file().rel.view(), rq)) return false;
        Array<cook::Image> frames;
        if (!render_ticks(c, rq, frames, software)) return false;
        for (u32 col = 0; col < cols; ++col) {
            draw_safe_area(frames[col], rq);
            const i32 x = static_cast<i32>(kGap + col * (base.width + kGap)), y = static_cast<i32>(kGap + row * (base.height + kGap));
            blit(sheet, frames[col], x, y);
            char label[40];
            if (rows > 1) std::snprintf(label, sizeof(label), "CAM %u T=%llu", row, static_cast<unsigned long long>(base.ticks[col]));
            else std::snprintf(label, sizeof(label), "T=%llu", static_cast<unsigned long long>(base.ticks[col]));
            draw_label(sheet, x + 3, y + 3, label, 2, 0xFFFFFF00u);
        }
    }
    c.out.begin_object();
    write_result_head(c, base, software);
    c.out.key("ticks").begin_array();
    for (u64 t : base.ticks) c.out.num_u(t);
    c.out.end_array();
    c.out.key("rows").num_u(rows).key("columns").num_u(cols);
    c.out.key("sheet_width").num_u(sheet.width).key("sheet_height").num_u(sheet.height);
    c.out.end_object();
    return attach_image(c, sheet);
}

bool image_diff(ToolCall& c) {
    if (!need_project(c)) return false;
    StringView golden_rel, image_rel;
    u64 threshold = 8;
    bool update = false;
    if (!arg_string(c, "golden", golden_rel, true) || !arg_string(c, "image", image_rel, false) ||
        !arg_uint(c, "threshold", threshold, 0, 255, false) || !arg_bool(c, "update", update))
        return false;
    f64 max_score = 0.01;
    if (const cook::JsonValue m = arg(c, "max_score"); m.valid()) {
        if (!m.is_number() || m.as_number() < 0 || m.as_number() > 1) return arg_missing(c, "max_score", "a number in [0, 1]");
        max_score = m.as_number();
    }
    cook::String golden_abs, image_abs;
    if (!project_png_path(c, "golden", golden_rel, golden_abs)) return false;
    const cook::JsonValue render = arg(c, "render");
    if (image_rel.empty() == !render.valid()) return fail(c, "give exactly one of 'image' (a PNG in the project) or 'render' (render_frame's arguments)");

    cook::Image image;
    bool rendered = false;
    if (!image_rel.empty()) {
        if (!project_png_path(c, "image", image_rel, image_abs)) return false;
        Result<cook::Image> img = read_png_file(image_abs.view());
        if (!img) return fail(c, "argument 'image': cannot read that PNG");
        image = static_cast<cook::Image&&>(img.value());
    } else {
        if (!render.is_object()) return arg_missing(c, "render", "an object of render_frame arguments");
        // render_frame's arguments, read from the nested object.
        ToolCall inner{c.server, render, cook::JsonValue(), c.out, c.error, nullptr};
        RenderRequest rq;
        u64 tick = 1;
        if (!arg_uint(inner, "tick", tick, 1, kMaxTick, false) || !arg_size(inner, "width", "height", rq.width, rq.height) ||
            !parse_common(inner, rq))
            return false;
        rq.ticks.push_back(tick);
        Array<cook::Image> frames;
        bool software = false;
        if (!render_ticks(inner, rq, frames, software)) return false;
        image = static_cast<cook::Image&&>(frames[0]);
        rendered = true;
    }

    const bool have_golden = cook::fs::exists(golden_abs.view());
    if (update || !have_golden) {
        if (Result<void> w = write_png_file(golden_abs.view(), image); !w) return fail(c, "cannot write the golden PNG");
        c.out.begin_object().key("golden").str(golden_rel).key("written").boolean(true);
        c.out.key("reason").str(update ? "update: true" : "no golden yet: this image is now the golden");
        c.out.key("width").num_u(image.width).key("height").num_u(image.height).end_object();
        return attach_image(c, image);
    }
    Result<cook::Image> golden = read_png_file(golden_abs.view());
    if (!golden) return fail(c, "argument 'golden': cannot read that PNG");
    if (golden.value().width != image.width || golden.value().height != image.height) {
        cook::appendf(c.error, "size mismatch: golden %ux%u, image %ux%u (render at the golden's size, or update: true)",
                      golden.value().width, golden.value().height, image.width, image.height);
        return false;
    }
    const DiffResult d = diff_images(golden.value(), image, static_cast<u32>(threshold));
    c.out.begin_object();
    c.out.key("golden").str(golden_rel);
    c.out.key("rendered").boolean(rendered);
    c.out.key("score").num_f(d.score);
    c.out.key("changed_fraction").num_f(d.changed_fraction);
    c.out.key("max_channel_diff").num_u(d.max_channel_diff);
    c.out.key("threshold").num_u(threshold);
    c.out.key("max_score").num_f(max_score);
    c.out.key("pass").boolean(d.score <= max_score);
    c.out.end_object();
    return attach_image(c, d.heatmap);
}

}  // namespace mce::mcp
