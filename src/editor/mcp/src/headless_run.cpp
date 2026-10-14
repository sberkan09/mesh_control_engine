#include "mce/mcp/headless_run.h"

#include "mce/app.h"
#include "mce/asset/asset_system.h"
#include "mce/audio/clip_bank.h"
#include "mce/audio/components.h"
#include "mce/editor/play_session.h"
#include "mce/input/ecs.h"
#include "mce/kit/stock.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"
#include "mce/physics/module.h"
#include "mce/scene/scene.h"
#include "mce/scene/streaming.h"

namespace mce::mcp {

namespace {

// mce_player's PlayerApp with no window and no device (src/player/src/player.cpp): keep the two in
// step. The game is the one a project's MCP server links in (mce_add_project's <game>_mcp hands its
// install() to editor::set_play_game_install, as <game>_editor does for play-in-editor).
class HeadlessApp final : public app::Application {
public:
    HeadlessApp(asset::AssetSystem& assets, audio::AudioDevice& audio, asset::Guid scene, u64 total,
                TickProgressFn progress, void* user)
        : assets_(assets), audio_(audio), clips_(assets, audio), scene_(scene), total_(total), progress_(progress),
          user_(user) {}

    ResultV on_init(app::Context&) override {
        input::install(world_);
        physics::install_physics2d(world_);
        audio::install(world_, audio_, &clips_);
        kit::install_stock(world_, &assets_);
        if (const editor::GameInstallFn game = editor::play_game_install()) {
            if (ResultV g = game(world_); !g) {
                error_ = g.error();
                return g;
            }
        }
        scene::StreamingConfig streaming;
        streaming.async = false;  // deterministic, as mce_player -headless
        scene::install_streaming(world_, assets_, streaming);
        ResultV r = scene::load(world_, assets_, scene_, &entities_);
        if (!r) error_ = r.error();
        else scene::pump_streaming(world_, scene::PumpMode::Settle);
        return r;
    }

    void on_tick(const app::TickContext& t) override {
        if (t.actions) input::publish(world_, *t.actions);  // the scripted input, as the player publishes the keyboard's
        scene::pump_streaming(world_);
        world_.tick(t.dt, sim_ticks_);
        ++sim_ticks_;
        audio_.update();  // the Silent backend mixes one block per call
        if (progress_ && (sim_ticks_ % kProgressTicks == 0 || sim_ticks_ == total_)) progress_(sim_ticks_, total_, user_);
    }

    void on_render(const app::RenderContext&) override {
        audio_.update();
        clips_.sync();
    }

    void on_shutdown() override { clips_.release_all(); }

    World& world() { return world_; }
    Span<const Entity> spawned() const { return entities_.span(); }
    u64 ticks() const { return sim_ticks_; }
    usize entity_count() const { return entities_.size(); }
    const Error& error() const { return error_; }

private:
    asset::AssetSystem& assets_;
    audio::AudioDevice& audio_;
    audio::ClipBank clips_;
    asset::Guid scene_;
    u64 total_;
    TickProgressFn progress_;
    void* user_;
    World world_;
    Array<Entity> entities_;
    u64 sim_ticks_ = 0;
    Error error_;
};

}  // namespace

Result<HeadlessRun> run_headless(StringView pack, asset::Guid scene, u64 ticks, TickProgressFn progress, void* user,
                                 InspectFn inspect, void* inspect_user, app::IInputSource* input, Span<const StringView> base) {
    if (ticks == 0) return Error(Errc::InvalidArgument, "mcp.run_headless", "ticks must be > 0");
    asset::AssetSystem assets;
    const StringView roots[1] = {pack};
    MCE_TRY(assets.mount(Span<const StringView>(roots, 1)));
    MCE_TRY(assets.mount(base));
    audio::AudioDesc ad;
    ad.backend = audio::Backend::Silent;
    MCE_TRY_ASSIGN(mem::UniquePtr<audio::AudioDevice> device, audio::AudioDevice::create(ad));

    HeadlessRun out;
    {
        HeadlessApp app(assets, *device, scene, ticks, progress, user);
        app::AppDesc desc;
        desc.name = "mce_mcp.run_headless";
        desc.headless = true;
        desc.max_ticks = ticks;
        desc.input_source = input;
        if (app::run(app, desc) != 0) return app.error();
        out.ticks = app.ticks();
        out.entities = app.entity_count();
        out.state_hash = app.world().state_hash();
        out.metrics = app.world().metrics();
        if (inspect) inspect(app.world(), app.spawned(), inspect_user);
    }
    return out;
}

}  // namespace mce::mcp
