// mce_player — plays one cooked scene (editor-tools.md §2.1: the run-in-player child process).
//
//   mce_player -pack <assets.mce | cooked dir> [-scene <guid | source path>]
//   mce_player -pack p.mce -headless 1 -ticks 600      determinism: prints the World state hash
//   mce_player -pack p.mce -frames 120 -capture a.ppm  CI smoke: write the last frame
//
// Without -scene the first Scene asset of the pack (by source path) plays. Controls: Esc pauses
// and resumes (the pause overlay), Q quits while paused.

#include <cstdio>

#include "hud_font.h"
#include "mce/asset/asset_system.h"
#include "mce/audio/components.h"
#include "mce/core.h"
#include "mce/ecs/transform.h"
#include "mce/kit/sprite_extract.h"
#include "mce/physics/module.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"
#include "mce/scene/scene.h"

namespace {

using namespace mce;
using namespace mce::math;

MCE_CVAR(cv_pack, "pack", "", "Cooked pack (assets.mce) or cooked/ directory to mount", CVarFlags::None);
MCE_CVAR(cv_scene, "scene", "", "Scene to play: GUID or source path (default: the first scene)", CVarFlags::None);
MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_ticks, "ticks", 600, "Headless: ticks to simulate", CVarFlags::None);
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
    PlayerApp(platform::Window* window, rhi::RenderDevice* device, audio::AudioDevice& audio)
        : window_(window), device_(device), audio_(audio), sink_(renderer_, assets_), clips_(assets_, audio) {}

    ResultV on_init(app::Context&) override {
        if (cv_pack.get_string().empty()) return Error(Errc::InvalidArgument, "mce_player: -pack <path> is required");
        const StringView roots[1] = {cv_pack.get_string()};
        MCE_TRY(assets_.mount(Span<const StringView>(roots, 1)));
        MCE_TRY_ASSIGN(const asset::Guid scene, pick_scene(cv_scene.get_string()));

        physics::install_physics2d(world_);
        audio::install(world_, audio_, &clips_);
        if (device_) {
            MCE_TRY(renderer_.init(*device_));
            assets_.set_upload_sink(&sink_);
            MCE_TRY(extract_.init(renderer_, &assets_));
            samples::HudFont hud;
            MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                        "player.hud_font"));
            window_->set_event_handler(&PlayerApp::on_event, this);
        }
        MCE_TRY(scene::load(world_, assets_, scene, &entities_));
        MCE_LOG_INFO("mce_player: scene {} ({}) with {} entities", asset::to_string(scene).view(), scene_path_.view(),
                     entities_.size());
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        if (paused_) return;
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

        render::SpriteBatch batch(renderer_.sprite_sets(), mem::frame());
        batch.begin(render::compute_view_2d(cam, bb, ppm));
        extract_.extract(world_, batch);
        batch.build();

        rhi::CommandBuffer cmd(mem::frame());
        rhi::PassAttachment color;
        color.clear.color[0] = clear.r;
        color.clear.color[1] = clear.g;
        color.clear.color[2] = clear.b;
        color.clear.color[3] = clear.a;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "player.world";
        cmd.begin_pass(pass);
        renderer_.record_2d(cmd, batch);
        if (paused_) record_pause_overlay(cmd, bb);
        cmd.end_pass();
        device_->submit(cmd);

        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last && !cv_capture.get_string().empty()) capture(cv_capture.get_string().data(), bb);
        device_->end_frame();
        assets_.end_frame();
        if (last) quit(0);
    }

    void on_shutdown() override {
        if (window_) window_->set_event_handler(nullptr, nullptr);
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

    void record_pause_overlay(rhi::CommandBuffer& cmd, rhi::Extent2D bb) {
        render::SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(render::compute_screen_view(bb), render::PassId::Screen2D);
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
        hud.build();
        renderer_.record_2d(cmd, hud);
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

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<PlayerApp*>(user);
        if (e.type != platform::EventType::KeyDown) return;
        switch (static_cast<platform::Key>(e.code)) {
            case platform::Key::Escape: self->paused_ = !self->paused_; break;
            case platform::Key::Q:
                if (self->paused_) self->quit(0);
                break;
            default: break;
        }
    }

    platform::Window* window_;
    rhi::RenderDevice* device_;
    audio::AudioDevice& audio_;
    asset::AssetSystem assets_;
    render::Renderer renderer_;
    render::AssetUploadSink sink_;
    audio::ClipBank clips_;
    kit::SpriteExtract extract_;
    render::Font font_;
    World world_;
    Array<Entity> entities_;
    FixedString<256> scene_path_;
    u64 sim_ticks_ = 0;
    bool paused_ = false;
};

int run_headless() {
    audio::AudioDesc ad;
    ad.backend = audio::Backend::Silent;
    auto audio = audio::AudioDevice::create(ad);
    if (!audio) return 1;
    PlayerApp app(nullptr, nullptr, *audio.value());
    app::AppDesc desc;
    desc.name = "mce_player";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 600);
    const int rc = app::run(app, desc);
    if (rc == 0)
        std::printf("mce_player: %llu ticks, %zu entities, state_hash %016llx\n", static_cast<unsigned long long>(app.ticks()),
                    app.entity_count(), static_cast<unsigned long long>(app.state_hash()));
    return rc;
}

int run_windowed() {
    platform::WindowDesc wd;
    wd.title = "MCE - Player";
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

    PlayerApp app(window.value().get(), device.value().get(), *audio.value());
    app::AppDesc desc;
    desc.name = "mce_player";
    desc.platform = &app;
    desc.fences = device.value().get();
    app::RunStats stats;
    int rc = app::run(app, desc, &stats);
    const u32 gl_errors = device.value()->validation_error_count();
    std::printf("mce_player: %llu frames, %llu ticks, %u GL errors\n", static_cast<unsigned long long>(stats.frames),
                static_cast<unsigned long long>(app.ticks()), gl_errors);
    if (rc == 0 && gl_errors != 0) rc = 2;
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "mce_player";
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    const ConVar* headless = ConVar::find(StringId("headless"));
    const bool is_headless = headless && headless->get_bool();
    int rc = 1;
    if (is_headless) {
        rc = run_headless();
    } else if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        rc = run_windowed();
        platform::shutdown();
    }
    log::shutdown();
    return rc;
}
