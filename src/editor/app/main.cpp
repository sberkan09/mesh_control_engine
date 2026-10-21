// mce_editor — the docking editor. Window + GL device + EditorShell, driven by the one app loop
// (mce::app::run). Without a project the viewport shows the M0 stub world; with -project it edits
// scenes (open/save, hierarchy, inspector, picking, cook).
//
//   mce_editor                          run until the window closes
//   mce_editor -project samples/scene_demo [-scene scenes/demo.mcscene]
//   mce_editor -module build/.../libkindle.so -project projects/Kindle   a project's game loaded
//   mce_editor -project samples/scene_demo -frames 90 -selftest   CI smoke in scene mode
//   mce_editor -project samples/scene_demo -editor.play 1 -frames 90 -selftest   play-in-editor smoke
//   mce_editor -frames 120 -selftest    CI smoke: check the viewport and shell pixels, then quit
//   mce_editor -frames 60 -capture shot.ppm
//   mce_editor -editor.ini ""           no dock-layout persistence
//   mce_editor -help                    every convar with its help

#include <cstdio>
#include <cstdlib>

#include "mce/core.h"
#include "mce/editor/editor_shell.h"
#include "mce/editor/play_session.h"
#include "mce/module_host.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/rhi/default_device.h"

namespace {

using namespace mce;

MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_selftest, "selftest", false, "With -frames: verify the viewport and shell pixels on the last frame",
         CVarFlags::None);
MCE_CVAR(cv_project, "project", "", "Project folder to open (holds assets/)", CVarFlags::None);
MCE_CVAR(cv_module, "module", "",
         "Game module to load (lib<game>.so / .dll): its components in the inspector and the cook, its install() in play",
         CVarFlags::None);
MCE_CVAR(cv_scene, "scene", "", "Scene to open, relative to assets/ (default: the first scene)", CVarFlags::None);
MCE_CVAR(cv_select, "editor.select", "", "Select the entity with this name after opening the scene", CVarFlags::None);
MCE_CVAR(cv_play, "editor.play", false, "Enter play-in-editor after opening the scene (the -selftest checks it)",
         CVarFlags::None);
MCE_CVAR(cv_open, "editor.open", "", "Open this asset (relative to assets/) in its panel: a .mcui, .mctimeline, .mcanim or .mcbt",
         CVarFlags::None);
MCE_CVAR(cv_perf_capture, "editor.perf_capture", "", "Open this .mcperf capture in the Profiler's Capture tab at startup",
         CVarFlags::None);
MCE_CVAR(cv_focus, "editor.panel", "", "Bring this panel's tab to the front at startup (e.g. Profiler)", CVarFlags::None);
MCE_CVAR(cv_ini, "editor.ini", "mce_editor.ini", "Dock layout file (empty = no persistence)", CVarFlags::RequiresRestart);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);

class EditorApp final : public app::Application, public app::IPlatformHooks {
public:
    EditorApp(platform::Window& window, rhi::RenderDevice& device) : window_(window), device_(device) {}

    ResultV on_init(app::Context&) override {
        editor::add_default_panels(shell_);
        editor::EditorShellDesc desc;
        desc.window = &window_;
        desc.device = &device_;
        FixedString<256> ini;
        ini.append(cv_ini.get_string());
        desc.ini_path = ini.c_str();
        MCE_TRY(shell_.init(desc));
        window_.set_event_handler(&EditorApp::on_event, this);
        if (!cv_project.get_string().empty()) {
            if (!shell_.open_project(cv_project.get_string())) return Error(Errc::FileNotFound, "mce_editor", cv_project.get_string());
            StringView scene = cv_scene.get_string();
            Array<const editor::ProjectAsset*> scenes;
            shell_.session().list_scenes(scenes);
            if (scene.empty() && !scenes.empty()) scene = scenes[0]->path.view();
            if (!scene.empty() && !shell_.open_scene(scene)) return Error(Errc::ParseError, "mce_editor", scene);
            for (const editor::DocEntity& e : shell_.scene().doc().entities())
                if (!cv_select.get_string().empty() && e.name.view() == cv_select.get_string()) shell_.scene().selection().set(e.id);
            if (!cv_open.get_string().empty()) {
                const editor::ProjectAsset* a = nullptr;
                for (const editor::ProjectAsset& p : shell_.session().project().assets())
                    if (p.path.view() == cv_open.get_string()) a = &p;
                if (!a) return Error(Errc::FileNotFound, "mce_editor", cv_open.get_string());
                if (a->type == asset::ChunkType::UiDocument) shell_.open_ui_document(a->guid);
                if (a->type == asset::ChunkType::Timeline) shell_.open_timeline(a->guid);
                if (a->type == asset::ChunkType::AnimGraph) shell_.open_anim_graph(a->guid);
                if (a->type == asset::ChunkType::BehaviorTree) shell_.open_bt(a->guid);
            }
            if (cv_play.get_bool() && !shell_.play_in_editor()) return Error(Errc::InvalidArgument, "mce_editor", "play");
        }
        if (!cv_perf_capture.get_string().empty()) shell_.open_perf_capture(cv_perf_capture.get_string());
        focus_.append(cv_focus.get_string());  // last: opening a project focuses the Cook panel
        if (!focus_.empty()) shell_.focus_panel(focus_.c_str());
        return ok();
    }

    void on_tick(const app::TickContext& t) override { shell_.tick(t.dt); }

    void on_render(const app::RenderContext& rc) override {
        device_.begin_frame(rc.frame_index);
        shell_.build_ui(rc.frame_dt);
        rhi::CommandBuffer cmd(mem::frame());
        shell_.record(cmd, rc.alpha);
        const f64 t0 = platform::now_seconds();
        device_.submit(cmd);
        shell_.end_frame_perf((platform::now_seconds() - t0) * 1000.0);

        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last) {
            if (!cv_capture.get_string().empty()) capture(cv_capture.get_string().data());
            if (cv_selftest.get_bool() && !(shell_.playing() ? play_selftest() : shell_.scene_mode() ? scene_selftest() : selftest()))
                exit_code_ = 3;
        }
        device_.end_frame();
        if (last || shell_.quit_requested()) quit(exit_code_);
    }

    void on_shutdown() override {
        window_.set_event_handler(nullptr, nullptr);
        shell_.shutdown();
    }

    // app::IPlatformHooks
    // Returns true to quit. A window close goes through the shell, which asks before dropping
    // unsaved changes; on_render quits once the shell agrees.
    bool pump_events() override {
        if (platform::pump_events()) shell_.request_quit();
        return false;
    }
    void present(bool vsync) override { device_.present(vsync); }

private:
    static void on_event(const platform::Event& e, void* user) { static_cast<EditorApp*>(user)->shell_.on_event(e); }

    bool read(rhi::TextureHandle target, rhi::Extent2D size, Array<u8>& px) {
        px.resize(usize(size.width) * size.height * 4);
        return static_cast<bool>(device_.read_pixels(target, rhi::Rect2D{0, 0, size.width, size.height}, px.span()));
    }

    void capture(const char* path) {
        const rhi::Extent2D size = device_.backbuffer_size();
        Array<u8> px;
        if (!read({}, size, px)) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("mce_editor: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("mce_editor: wrote {}", path);
    }

    // The viewport target must hold the stub world's triangle (all three vertex colors dominate
    // somewhere), and the backbuffer must hold more than a clear (the dock shell drew).
    bool selftest() {
        const editor::ViewportInfo& v = shell_.viewport();
        if (v.target.is_null() || v.size.width < 16 || v.size.height < 16) {
            MCE_LOG_ERROR("selftest: viewport panel not visible");
            return false;
        }
        Array<u8> px;
        if (!read(v.target, v.size, px)) {
            MCE_LOG_ERROR("selftest: viewport read-back failed");
            return false;
        }
        u32 dominant[3] = {};
        for (usize i = 0; i < px.size(); i += 4) {
            const int r = px[i], g = px[i + 1], b = px[i + 2];
            if (r > 120 && r > g + 60 && r > b + 60) ++dominant[0];
            if (g > 120 && g > r + 60 && g > b + 60) ++dominant[1];
            if (b > 120 && b > r + 60 && b > g + 60) ++dominant[2];
        }
        const rhi::Extent2D bb = device_.backbuffer_size();
        Array<u8> back;
        if (!read({}, bb, back)) {
            MCE_LOG_ERROR("selftest: backbuffer read-back failed");
            return false;
        }
        u32 distinct = 0;
        u32 seen[64] = {};
        for (usize i = 0; i < back.size() && distinct < 64; i += 4 * 7) {
            const u32 c = back[i] | (u32(back[i + 1]) << 8) | (u32(back[i + 2]) << 16);
            bool found = false;
            for (u32 k = 0; k < distinct; ++k) found |= seen[k] == c;
            if (!found) seen[distinct++] = c;
        }
        MCE_LOG_INFO("selftest: viewport {}x{} red/green/blue px {}/{}/{}; backbuffer {}+ colors", v.size.width,
                     v.size.height, dominant[0], dominant[1], dominant[2], distinct);
        return dominant[0] > 0 && dominant[1] > 0 && dominant[2] > 0 && distinct >= 8;
    }

    // Scene mode: the document mirrors into the preview world and the viewport shows its sprites
    // (more than a handful of colours over the clear colour, i.e. textures arrived through the
    // cooked tree), and a pick at an entity's centre selects it.
    bool scene_selftest() {
        editor::EditorScene& scene = shell_.scene();
        if (scene.doc().size() == 0 || !scene.verify_world()) {
            MCE_LOG_ERROR("selftest: scene empty or preview world out of sync");
            return false;
        }
        const editor::ViewportInfo& v = shell_.viewport();
        if (v.target.is_null() || v.size.width < 16 || v.size.height < 16) {
            MCE_LOG_ERROR("selftest: viewport panel not visible");
            return false;
        }
        Array<u8> px;
        if (!read(v.target, v.size, px)) return false;
        u32 non_clear = 0, distinct = 0;
        u32 seen[64] = {};
        for (usize i = 0; i < px.size(); i += 4) {
            const int r = px[i], g = px[i + 1], b = px[i + 2];
            if (std::abs(r - 28) + std::abs(g - 28) + std::abs(b - 31) > 40) ++non_clear;
            const u32 c = (u32(r) >> 4) | ((u32(g) >> 4) << 4) | ((u32(b) >> 4) << 8);
            bool found = false;
            for (u32 k = 0; k < distinct; ++k) found |= seen[k] == c;
            if (!found && distinct < 64) seen[distinct++] = c;
        }
        editor::EntityId probe = editor::kNoEntity;
        for (const editor::DocEntity& e : scene.doc().entities()) {
            math::vec2 q[4];
            if (!editor::entity_quad(scene, e.id, q)) continue;
            const math::vec2 c = (q[0] + q[2]) * 0.5f;
            if (editor::pick_entity(scene, c) == e.id) probe = e.id;
        }
        // A kit3d scene opens in the orbit view, drawn by the forward renderer.
        const bool is_3d = editor::world_is_3d(scene.world());
        const bool drawn_3d = !is_3d || (shell_.view().orbit_view && shell_.preview().last_stats_3d().triangles > 0);
        MCE_LOG_INFO("selftest: scene {} entities; viewport {}x{}, {} non-clear px, {} colours; pick probe {}{}",
                     scene.doc().size(), v.size.width, v.size.height, non_clear, distinct, probe,
                     is_3d ? (drawn_3d ? "; 3D drawn" : "; 3D NOT drawn") : "");
        return non_clear > v.size.width * v.size.height / 50 && distinct >= 6 && probe != editor::kNoEntity && drawn_3d;
    }

    // Play-in-editor: the play world ticked on wall time, the document never changed, and the
    // viewport shows the running scene (its sprites over the game camera's clear colour).
    bool play_selftest() {
        editor::PlaySession& play = shell_.session().play();
        const editor::ViewportInfo& v = shell_.viewport();
        if (v.target.is_null() || play.ticks() == 0 || shell_.scene().dirty() || !shell_.scene().verify_world()) {
            MCE_LOG_ERROR("selftest: play not running ({} ticks) or the document changed", play.ticks());
            return false;
        }
        Array<u8> px;
        if (!read(v.target, v.size, px)) return false;
        u32 distinct = 0;
        u32 seen[64] = {};
        for (usize i = 0; i < px.size(); i += 4) {
            const u32 c = (u32(px[i]) >> 4) | ((u32(px[i + 1]) >> 4) << 4) | ((u32(px[i + 2]) >> 4) << 8);
            bool found = false;
            for (u32 k = 0; k < distinct; ++k) found |= seen[k] == c;
            if (!found && distinct < 64) seen[distinct++] = c;
        }
        MCE_LOG_INFO("selftest: playing, {} ticks, {} entities, hash {:016x}; viewport {} colours", play.ticks(),
                     play.entity_count(), play.state_hash(), distinct);
        return distinct >= 6;
    }

    platform::Window& window_;
    rhi::RenderDevice& device_;
    editor::EditorShell shell_;
    FixedString<64> focus_;
    int exit_code_ = 0;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("mce_editor", text.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "mce_editor";
    log::init(log_desc);
    console::parse_command_line(argc, argv);
    if (console::help_requested()) {
        console::print_help("mce_editor",
                            "usage: mce_editor [-project <dir> [-module <game module>] [-scene <path under assets/>]]\n"
                            "                  [-editor.play 1] [-editor.open <asset>] [-frames N [-capture <file.ppm>] [-selftest]] [-editor.ini <file>] [-help]\n"
                            "  The docking editor. Without -project the viewport shows the stub world.");
        log::shutdown();
        return 0;
    }

    // The game module outlives everything below: the shell's documents hold its component records
    // and the play world its code until they are gone.
    struct ModuleGuard {
        LoadedModule module;
        ~ModuleGuard() {
            if (!module.loaded()) return;
            editor::set_play_game_install(nullptr);
            unload_game_module(module);
        }
    } game;
    int rc = 1;
    if (!cv_module.get_string().empty()) {
        Result<LoadedModule> loaded = open_game_module(cv_module.get_string());
        if (!loaded) {
            const int failed = fail("game module", loaded.error());  // logs, so before log::shutdown
            log::shutdown();
            return failed;
        }
        game.module = loaded.value();
        editor::set_play_game_install(game.module.info->install);
    }
    if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        platform::WindowDesc wd;
        wd.title = "Mesh Control Engine - Editor";
        auto window = platform::Window::create(wd);
        if (!window) {
            rc = fail("window creation failed", window.error());
        } else {
            rhi::DeviceDesc dd;
            dd.debug_layer = cv_gl_debug.get_bool();
            auto device = rhi::create_default_device(*window.value(), dd);
            if (!device) {
                rc = fail("render device creation failed", device.error());
            } else {
                EditorApp app(*window.value(), *device.value());
                app::AppDesc desc;
                desc.name = "mce_editor";
                desc.platform = &app;
                desc.fences = device.value().get();
                app::RunStats stats;
                rc = app::run(app, desc, &stats);
                const u32 gl_errors = device.value()->validation_error_count();
                std::printf("mce_editor: %llu frames, %llu ticks, %u GL errors\n",
                            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.ticks),
                            gl_errors);
                if (rc == 0 && gl_errors != 0) rc = 2;
            }
        }
    }
    platform::shutdown();
    log::shutdown();
    return rc;
}
