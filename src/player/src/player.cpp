// The player runtime (mce/player/player.h): mce_player and every project host built from the
// project template run this. It plays one cooked scene (editor-tools.md §2.1: the run-in-player
// child process), with the project's game module installed on top of the engine systems.
//
//   mce_player -pack <assets.mce | cooked dir> [-scene <guid | source path>] [-module <game module>]
//   mce_player -pack p.mce -headless 1 -ticks 600      determinism: prints the World state hash
//   mce_player -pack p.mce -frames 120 -capture a.ppm  CI smoke: write the last frame
//
// Without -scene the first Scene asset of the pack (by source path) plays. F7 toggles 2D post. Controls: Esc pauses
// and resumes (the pause overlay), Q quits while paused, ` or F1 opens the developer console (FR-UI-045: the log,
// convars and console functions; -console 0 removes it from a shipped build's command line).
//
// Lifecycle (FR-RUN-060): losing focus or minimizing pauses the sim (the pause overlay is up when the player comes
// back, so nothing moves before they choose to resume) and mutes the master bus until focus returns. With
// -save <slot>, a save of that name is loaded on top of the scene at start and written on every clean quit (window
// close, Q, -frames / -ticks reached) before anything shuts down; the write is atomic with a .bak, so a kill during
// it keeps the last good save (ecs/save.h).

#include <cstdio>

#include "hud_font.h"
#include "mce/asset/asset_system.h"
#include "mce/audio/components.h"
#include "mce/core.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"
#include "mce/kit/sprite_extract.h"
#include "mce/physics/module.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"
#include "mce/module_host.h"
#include "mce/player/player.h"
#include "mce/scene/scene.h"
#include "mce/ui/ui.h"
#include "ship_paths.h"

namespace {

using namespace mce;
using namespace mce::math;

MCE_CVAR(cv_pack, "pack", "", "Cooked pack (assets.mce) or cooked/ directory to mount (a host's default: assets.mce beside it)",
         CVarFlags::None);
MCE_CVAR(cv_module, "module", "", "Game module to install on top of the engine systems (a host's default: its own module)",
         CVarFlags::None);
MCE_CVAR(cv_scene, "scene", "", "Scene to play: GUID or source path (default: the first scene)", CVarFlags::None);
MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_ticks, "ticks", 600, "Headless: ticks to simulate", CVarFlags::None);
MCE_CVAR(cv_post, "r.post2d", true, "Apply the camera's Post2DComponent effects (F7 toggles)", CVarFlags::None);
MCE_CVAR(cv_pause_on_focus_loss, "pause_on_focus_loss", true, "Pause and mute when the window loses focus or minimizes",
         CVarFlags::None);
MCE_CVAR(cv_save, "save", "", "Save slot: loaded at start when it exists, written on clean quit (empty = off)",
         CVarFlags::None);
MCE_CVAR(cv_save_dir, "save_dir", "", "Directory for save slots (default: the per-user MCE save folder)", CVarFlags::None);
MCE_CVAR(cv_console, "console", true, "Allow the developer console (` or F1)", CVarFlags::None);
MCE_CVAR(cv_console_open, "console_open", false, "Start with the developer console open", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("mce_player", text.c_str());
    return 1;
}

class PlayerApp final : public app::Application, public app::IPlatformHooks {
public:
    PlayerApp(const player::PlayerDesc& desc, platform::Window* window, rhi::RenderDevice* device, audio::AudioDevice& audio)
        : desc_(desc), window_(window), device_(device), audio_(audio), sink_(renderer_, assets_), clips_(assets_, audio) {}

    ResultV on_init(app::Context&) override {
        FixedString<1024> pack;
        if (!cv_pack.get_string().empty() || desc_.pack)
            samples::resolve_shipped_path(cv_pack.get_string(), desc_.pack ? desc_.pack : "", desc_.pack_fallback ? desc_.pack_fallback : "",
                                          pack);
        if (pack.empty()) return Error(Errc::InvalidArgument, "mce_player: -pack <path> is required");
        const StringView roots[1] = {pack.view()};
        MCE_TRY(assets_.mount(Span<const StringView>(roots, 1)));
        MCE_TRY_ASSIGN(const asset::Guid scene, pick_scene(cv_scene.get_string()));

        physics::install_physics2d(world_);
        audio::install(world_, audio_, &clips_);
        kit::install_particles(world_, &assets_);
        kit::install_sequences(world_, &assets_);
        // The game on top of the engine systems (the game-module contract), before the scene so
        // its systems see the first tick.
        if (cv_module.get_string().empty() && desc_.static_module) {
            const ModuleInfo* info = desc_.static_module();
            if (!info || !info->install) return Error(Errc::ModuleMissingEntry, "player: the linked game has no install()");
            MCE_TRY(info->install(world_));
            MCE_LOG_INFO("{}: game '{}' v{} (linked in)", desc_.app_name, info->name, info->module_version);
        } else if (!cv_module.get_string().empty() || desc_.module) {
            FixedString<1024> path;
            samples::resolve_shipped_path(cv_module.get_string(), desc_.module ? desc_.module : "",
                                          desc_.module_fallback ? desc_.module_fallback : "", path);
            MCE_TRY_ASSIGN(module_.module, load_game_module(path.view(), world_));
            MCE_LOG_INFO("{}: game module '{}' v{} from {}", desc_.app_name, module_.module.info->name,
                         module_.module.info->module_version, module_.module.loaded_path.c_str());
        }
        if (device_) {
            MCE_TRY(renderer_.init(*device_));
            assets_.set_upload_sink(&sink_);
            MCE_TRY(extract_.init(renderer_, &assets_));
            samples::HudFont hud;
            MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                        "player.hud_font"));
            MCE_TRY(ui::create_builtin_skin(renderer_, skin_));
            console_.init(skin_.skin, 0.5f);
            if (cv_console.get_bool() && cv_console_open.get_bool()) set_console(true);
            window_->set_event_handler(&PlayerApp::on_event, this);
        }
        MCE_TRY(scene::load(world_, assets_, scene, &entities_));
        MCE_LOG_INFO("{}: scene {} ({}) with {} entities", desc_.app_name, asset::to_string(scene).view(), scene_path_.view(),
                     entities_.size());
        scene_guid_ = scene;
        if (!cv_save_dir.get_string().empty()) save::set_root(cv_save_dir.get_string());
        if (const StringView slot = cv_save.get_string(); !slot.empty() && save::exists(slot)) {
            save::SaveInfo info;
            MCE_TRY_ASSIGN(const save::LoadStats st, save::load_game(world_, slot, &info));
            sim_ticks_ = info.tick;
            std::printf("mce_player: save '%.*s' loaded (%u entities, tick %llu)\n", static_cast<int>(slot.size()), slot.data(),
                        st.entities, static_cast<unsigned long long>(info.tick));
        }
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        if (paused_) return;
        tick_dt_ = t.dt;
        world_.tick(t.dt, sim_ticks_);
        ++sim_ticks_;
        if (!device_) audio_.update();  // headless: the Silent backend mixes one block per call
    }

    void on_render(const app::RenderContext& rc) override {
        audio_.update();
        clips_.sync();
        if (!device_) return;
        device_->begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_->backbuffer_size();
        render::Camera2D cam;
        f32 ppm = 48.0f;
        Color clear{0.08f, 0.09f, 0.12f, 1.0f};
        kit::primary_camera(world_, cam, ppm, &clear);

        update_console(bb);
        const bool overlay = paused_ || console_.is_open();

        render::SpriteBatch batch(renderer_.sprite_sets(), mem::frame());
        batch.begin(render::compute_view_2d(cam, bb, ppm));
        extract_.extract(world_, batch);
        batch.build();

        // With 2D post on, the world renders offscreen and the post pass writes the backbuffer.
        render::Post2D post;
        const bool use_post = cv_post.get_bool() && extract_.post_2d(world_, post);
        if (use_post) ensure_post_target(bb);
        rhi::CommandBuffer cmd(mem::frame());
        rhi::PassAttachment color;
        color.texture = use_post ? post_target_ : rhi::TextureHandle{};
        color.clear.color[0] = clear.r;
        color.clear.color[1] = clear.g;
        color.clear.color[2] = clear.b;
        color.clear.color[3] = clear.a;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "player.world";
        cmd.begin_pass(pass);
        renderer_.record_2d(cmd, batch);
        if (use_post) {
            cmd.end_pass();
            renderer_.record_post_2d(cmd, post_target_, bb, post);
            if (overlay) {
                rhi::PassAttachment over;
                over.load = rhi::LoadOp::Load;
                rhi::RenderPassDesc op;
                op.colors = Span<const rhi::PassAttachment>(&over, 1);
                op.debug_name = "player.overlay";
                cmd.begin_pass(op);
                record_overlay(cmd, bb);
                cmd.end_pass();
            }
        } else {
            if (overlay) record_overlay(cmd, bb);
            cmd.end_pass();
        }
        device_->submit(cmd);

        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last && !cv_capture.get_string().empty()) capture(cv_capture.get_string().data(), bb);
        device_->end_frame();
        assets_.end_frame();
        if (last) quit(0);
    }

    void on_shutdown() override {
        // A clean quit flushes the save before anything is torn down (FR-RUN-060).
        if (const StringView slot = cv_save.get_string(); !slot.empty()) {
            save::SaveInfo info;
            info.scene = scene_guid_;
            info.tick = sim_ticks_;
            info.playtime = static_cast<f64>(sim_ticks_) * tick_dt_;
            if (Result<void> r = save::save_game(world_, slot, info); !r) {
                FixedString<512> why;
                format_error(r.error(), why);
                MCE_LOG_ERROR("mce_player: save '{}' failed: {}", slot, why.view());
            } else {
                std::printf("mce_player: save '%.*s' written (tick %llu)\n", static_cast<int>(slot.size()), slot.data(),
                            static_cast<unsigned long long>(sim_ticks_));
            }
        }
        set_muted(false);
        if (window_) window_->set_event_handler(nullptr, nullptr);
        if (device_ && !post_target_.is_null()) device_->destroy_texture(post_target_);
        clips_.release_all();
        extract_.shutdown();
        if (device_) renderer_.shutdown();
    }

    bool pump_events() override { return platform::pump_events(); }
    void present(bool vsync) override { device_->present(vsync); }

    u64 state_hash() { return world_.state_hash(); }
    u64 ticks() const { return sim_ticks_; }
    usize entity_count() const { return entities_.size(); }

private:
    // A GUID, a source path ("scenes/level.mcscene"), or empty for the first scene by path.
    Result<asset::Guid> pick_scene(StringView want) {
        if (!want.empty()) {
            if (Result<asset::Guid> g = asset::parse_guid(want)) {
                scene_path_.assign(assets_.info(assets_.find(g.value())).path);
                return g.value();
            }
        }
        asset::AssetKey best = asset::kInvalidAssetKey;
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
            const asset::AssetInfo info = assets_.info(k);
            if (info.type != asset::ChunkType::Scene) continue;
            if (!want.empty()) {
                if (info.path == want) best = k;
                continue;
            }
            if (best == asset::kInvalidAssetKey || info.path < assets_.info(best).path) best = k;
        }
        if (best == asset::kInvalidAssetKey) return Error(Errc::NotFound, "mce_player: no such scene in the pack", want);
        scene_path_.assign(assets_.info(best).path);
        return assets_.info(best).guid;
    }

    // ---- the developer console (FR-UI-045) ----------------------------------------------------

    void set_console(bool open) {
        console_.set_open(open);
        window_->set_text_input(open);
    }

    void update_console(rhi::Extent2D bb) {
        ui::UpdateContext ctx;
        ctx.viewport = bb;
        ctx.ui_scale = window_->display_scale();
        input::InputState in;
        in.cancel_pressed = con_.cancel;
        ctx.actions = &in;
        ctx.text_input = con_.typed.view();
        ctx.key_backspace = con_.backspace;
        ctx.key_enter = con_.enter;
        ctx.key_up = con_.up;
        ctx.key_down = con_.down;
        ctx.dt = 1.0 / 60.0;
        const bool was_open = console_.is_open();
        console_.update(ctx);  // pulls the log ring even while closed
        if (was_open && !console_.is_open()) window_->set_text_input(false);  // closed with Esc
        con_ = ConsoleKeys{};
    }

    void record_overlay(rhi::CommandBuffer& cmd, rhi::Extent2D bb) {
        render::SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(render::compute_screen_view(bb), render::PassId::Screen2D);
        if (paused_) add_pause_overlay(hud, bb);
        console_.submit(hud, 200);
        hud.build();
        renderer_.record_2d(cmd, hud);
    }

    void add_pause_overlay(render::SpriteBatch& hud, rhi::Extent2D bb) {
        render::SpriteInstance dim;  // a translucent veil over the frozen frame
        dim.set = extract_.sprite_set(asset::Guid{});
        dim.position = {0.5f * static_cast<f32>(bb.width), 0.5f * static_cast<f32>(bb.height)};
        dim.scale = {static_cast<f32>(bb.width), static_cast<f32>(bb.height)};
        dim.tint = Color{0.0f, 0.0f, 0.0f, 0.55f};
        hud.add(dim);
        render::TextLayout layout(mem::frame());
        render::TextLayoutDesc desc;
        desc.size_px = 32;
        desc.align = render::TextAlign::Center;
        render::layout_text(font_.face, "PAUSED\nESC RESUME   Q QUIT", desc, layout);
        render::TextPlacement p;
        p.origin = {0.5f * (static_cast<f32>(bb.width) - layout.width), 0.5f * (static_cast<f32>(bb.height) - layout.height)};
        p.sort_order = 1;
        hud.add_text(font_, layout, p);
    }

    void ensure_post_target(rhi::Extent2D size) {
        if (!post_target_.is_null() && post_size_.width == size.width && post_size_.height == size.height) return;
        if (!post_target_.is_null()) device_->destroy_texture(post_target_);
        rhi::TextureDesc td;
        td.size = size;
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.debug_name = "player.post2d_src";
        post_target_ = device_->create_texture(td);
        post_size_ = size;
    }

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_->read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("mce_player: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("mce_player: wrote {}", path);
    }

    // Unfocused / minimized: pause (the overlay waits for the player) and mute the master bus.
    void set_background(bool background) {
        if (!cv_pause_on_focus_loss.get_bool() || background == background_) return;
        background_ = background;
        if (background) paused_ = true;
        set_muted(background);
    }

    void set_muted(bool muted) {
        if (muted == muted_) return;
        muted_ = muted;
        if (muted) {
            master_db_ = audio_.bus_gain_db(audio::Bus_Master);
            audio_.set_bus_gain(audio::Bus_Master, -80.0f);
        } else {
            audio_.set_bus_gain(audio::Bus_Master, master_db_);
        }
    }

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<PlayerApp*>(user);
        switch (e.type) {
            case platform::EventType::FocusOut:
            case platform::EventType::Minimized: self->set_background(true); return;
            case platform::EventType::FocusIn:
            case platform::EventType::Restored: self->set_background(false); return;
            default: break;
        }
        if (self->console_.is_open()) {  // the console owns the keyboard while open
            if (e.type == platform::EventType::TextInput && e.code >= 32 && e.code < 127 && e.code != '`') {
                const char c = static_cast<char>(e.code);
                self->con_.typed.append(StringView(&c, 1));
            }
            if (e.type != platform::EventType::KeyDown) return;
            switch (static_cast<platform::Key>(e.code)) {
                case platform::Key::Escape: self->con_.cancel = true; break;
                case platform::Key::Return: self->con_.enter = true; break;
                case platform::Key::Backspace: self->con_.backspace = true; break;
                case platform::Key::Up: self->con_.up = true; break;
                case platform::Key::Down: self->con_.down = true; break;
                case platform::Key::Grave:
                case platform::Key::F1:
                    if (!(e.flags & platform::EventFlag::Repeat)) self->set_console(false);
                    break;
                default: break;
            }
            return;
        }
        if (e.type != platform::EventType::KeyDown) return;
        switch (static_cast<platform::Key>(e.code)) {
            case platform::Key::Grave:
            case platform::Key::F1:
                if (cv_console.get_bool() && !(e.flags & platform::EventFlag::Repeat)) self->set_console(true);
                break;
            case platform::Key::Escape: self->paused_ = !self->paused_; break;
            case platform::Key::Q:
                if (self->paused_) self->quit(0);
                break;
            case platform::Key::F7: cv_post.set_bool(!cv_post.get_bool()); break;
            default: break;
        }
    }

    const player::PlayerDesc& desc_;
    platform::Window* window_;
    rhi::RenderDevice* device_;
    audio::AudioDevice& audio_;
    asset::AssetSystem assets_;
    render::Renderer renderer_;
    render::AssetUploadSink sink_;
    audio::ClipBank clips_;
    kit::SpriteExtract extract_;
    render::Font font_;
    ui::BuiltinSkin skin_;
    ui::DevConsole console_;
    struct ConsoleKeys {  // this frame's console input, gathered by on_event
        FixedString<64> typed;
        bool cancel = false, enter = false, backspace = false, up = false, down = false;
    };
    ConsoleKeys con_;
    // Declared before the World so it is destroyed after it: the World holds the module's code.
    struct ModuleGuard {
        LoadedModule module;
        ~ModuleGuard() { unload_game_module(module); }
    } module_;
    World world_;
    Array<Entity> entities_;
    FixedString<256> scene_path_;
    u64 sim_ticks_ = 0;
    bool paused_ = false;
    bool background_ = false;  // unfocused or minimized
    bool muted_ = false;
    f32 master_db_ = 0.0f;     // the master gain to restore when focus returns
    f64 tick_dt_ = 1.0 / 60.0;
    asset::Guid scene_guid_;
    rhi::TextureHandle post_target_;
    rhi::Extent2D post_size_;
};

int run_headless(const player::PlayerDesc& pd) {
    audio::AudioDesc ad;
    ad.backend = audio::Backend::Silent;
    auto audio = audio::AudioDevice::create(ad);
    if (!audio) return 1;
    PlayerApp app(pd, nullptr, nullptr, *audio.value());
    app::AppDesc desc;
    desc.name = pd.app_name;
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 600);
    const int rc = app::run(app, desc);
    if (rc == 0)
        std::printf("%s: %llu ticks, %zu entities, state_hash %016llx\n", pd.app_name, static_cast<unsigned long long>(app.ticks()),
                    app.entity_count(), static_cast<unsigned long long>(app.state_hash()));
    return rc;
}

int run_windowed(const player::PlayerDesc& pd) {
    platform::WindowDesc wd;
    wd.title = pd.title;
    wd.width = 1280;
    wd.height = 720;
    auto window = platform::Window::create(wd);
    if (!window) return fail("window creation failed", window.error());
    rhi::DeviceDesc dd;
    dd.debug_layer = cv_gl_debug.get_bool();
    auto device = rhi::create_default_device(*window.value(), dd);
    if (!device) return fail("render device creation failed", device.error());
    auto audio = audio::AudioDevice::create({});  // falls back to Silent when no output device opens
    if (!audio) return fail("audio init failed", audio.error());

    PlayerApp app(pd, window.value().get(), device.value().get(), *audio.value());
    app::AppDesc desc;
    desc.name = pd.app_name;
    desc.platform = &app;
    desc.fences = device.value().get();
    app::RunStats stats;
    int rc = app::run(app, desc, &stats);
    const u32 gl_errors = device.value()->validation_error_count();
    std::printf("%s: %llu frames, %llu ticks, %u GL errors\n", pd.app_name, static_cast<unsigned long long>(stats.frames),
                static_cast<unsigned long long>(app.ticks()), gl_errors);
    if (rc == 0 && gl_errors != 0) rc = 2;
    return rc;
}

}  // namespace

int mce::player::run(int argc, char** argv, const PlayerDesc& pd) {
    log::LogDesc log_desc;
    log_desc.app_name = pd.app_name;
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    const ConVar* headless = ConVar::find(StringId("headless"));
    const bool is_headless = headless && headless->get_bool();
    int rc = 1;
    if (is_headless) {
        rc = run_headless(pd);
    } else if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        rc = run_windowed(pd);
        platform::shutdown();
    }
    log::shutdown();
    return rc;
}
