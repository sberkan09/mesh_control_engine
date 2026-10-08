#include "mce/mcp/headless_run.h"

#include "mce/app.h"
#include "mce/asset/asset_system.h"
#include "mce/audio/clip_bank.h"
#include "mce/audio/components.h"
#include "mce/physics/module.h"
#include "mce/scene/scene.h"

namespace mce::mcp {

namespace {

// mce_player's PlayerApp with no window and no device (src/player/main.cpp): keep the two in step.
class HeadlessApp final : public app::Application {
public:
    HeadlessApp(asset::AssetSystem& assets, audio::AudioDevice& audio, asset::Guid scene, u64 total,
                TickProgressFn progress, void* user)
        : assets_(assets), audio_(audio), clips_(assets, audio), scene_(scene), total_(total), progress_(progress),
          user_(user) {}

    ResultV on_init(app::Context&) override {
        physics::install_physics2d(world_);
        audio::install(world_, audio_, &clips_);
        ResultV r = scene::load(world_, assets_, scene_, &entities_);
        if (!r) error_ = r.error();
        return r;
    }

    void on_tick(const app::TickContext& t) override {
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

Result<HeadlessRun> run_headless(StringView pack, asset::Guid scene, u64 ticks, TickProgressFn progress, void* user) {
    if (ticks == 0) return Error(Errc::InvalidArgument, "mcp.run_headless", "ticks must be > 0");
    asset::AssetSystem assets;
    const StringView roots[1] = {pack};
    MCE_TRY(assets.mount(Span<const StringView>(roots, 1)));
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
        if (app::run(app, desc) != 0) return app.error();
        out.ticks = app.ticks();
        out.entities = app.entity_count();
        out.state_hash = app.world().state_hash();
        out.metrics = app.world().metrics();
    }
    return out;
}

}  // namespace mce::mcp
