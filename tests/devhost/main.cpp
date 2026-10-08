// mce_devhost: sample-ladder host and headless harness (ARCHITECTURE §2.1, L6).
// Runs N headless ticks through the one app loop and reports them (FR-RUN-025). With -module it
// is the headless module host of the sample ladder (scripting.md §6.5): the game module is
// loaded through the copy-and-load loader into a World carrying the engine runtime systems
// (input resource, physics2d), ticked, and its state hash printed for the determinism harness.
//
//   mce_devhost -ticks 10000
//   mce_devhost -module build/.../libplatformer.so -ticks 300

#include <cstdio>

#include "mce/core.h"

#if MCE_DEVHOST_MODULES
#include "mce/input/ecs.h"
#include "mce/module_host.h"
#include "mce/physics/module.h"
#endif

namespace {

MCE_CVAR(cv_ticks, "ticks", 10000, "Number of headless ticks to run", mce::CVarFlags::None);
MCE_CVAR(cv_module, "module", "", "Game module to load and tick headless (shared library path)",
         mce::CVarFlags::None);

class HeadlessProbe final : public mce::app::Application {
public:
    mce::u64 ticks = 0;
    mce::Hasher64 hash;
#if MCE_DEVHOST_MODULES
    mce::World* world = nullptr;
#endif

    mce::ResultV on_init(mce::app::Context& ctx) override {
        if (!ctx.headless) return mce::Error(mce::Errc::Unsupported, "devhost.init", "devhost runs headless only");
        return mce::ok();
    }
    void on_tick(const mce::app::TickContext& t) override {
        ++ticks;
        hash.u64v(t.tick_index);
#if MCE_DEVHOST_MODULES
        if (world) world->tick(t.dt, t.tick_index);
#endif
    }
};

}  // namespace

int main(int argc, char** argv) {
    mce::log::init({.app_name = "devhost", .level = mce::log::Level::Info});
    mce::console::parse_command_line(argc, argv);

    HeadlessProbe app;
    mce::app::AppDesc desc;
    desc.name = "mce_devhost";
    desc.headless = true;
    desc.max_ticks = static_cast<mce::u64>(cv_ticks.get_int());

    const mce::StringView module_path = cv_module.get_string();
#if MCE_DEVHOST_MODULES
    mce::LoadedModule module;
    mce::World* world = nullptr;
    if (!module_path.empty()) {
        world = new mce::World();  // NOLINT(cppcoreguidelines-owning-memory): must die before unload
        mce::input::install(*world);
        mce::physics::install_physics2d(*world);
        mce::Result<mce::LoadedModule> loaded = mce::load_game_module(module_path, *world);
        if (!loaded) {
            mce::FixedString<512> msg;
            mce::format_error(loaded.error(), msg);
            std::printf("devhost: module load failed: %s\n", msg.c_str());
            mce::log::shutdown();
            return 1;  // the World is leaked on purpose: it may hold the module's code pointers
        }
        module = loaded.value();
        app.world = world;
    }
#else
    if (!module_path.empty()) {
        std::printf("devhost: game modules are not supported on this platform yet\n");
        return 1;
    }
#endif

    mce::app::RunStats stats;
    const int rc = mce::app::run(app, desc, &stats);
#if MCE_DEVHOST_MODULES
    if (world) {
        // First "hash" in the output: scripts/ci/determinism.py compares it across runs/builds.
        std::printf("devhost: module %s v%u, %u entities, state_hash %016llx\n", module.info->name,
                    module.info->module_version, world->entity_count(),
                    static_cast<unsigned long long>(world->state_hash()));
        delete world;  // NOLINT(cppcoreguidelines-owning-memory)
        mce::unload_game_module(module);
    }
#endif
    std::printf("devhost: %llu ticks, %llu frames, %.3f s sim, hash %016llx\n",
                static_cast<unsigned long long>(stats.ticks), static_cast<unsigned long long>(stats.frames),
                stats.sim_seconds, static_cast<unsigned long long>(app.hash.value()));
    mce::log::shutdown();
    return (rc == 0 && stats.ticks == desc.max_ticks) ? 0 : 1;
}
