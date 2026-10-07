// mce_devhost: sample-ladder host and headless harness (ARCHITECTURE §2.1, L6).
// M0 scope: run N headless ticks through the one app loop and report them (FR-RUN-025).
//
//   mce_devhost -ticks 10000

#include <cstdio>

#include "mce/core.h"

namespace {

MCE_CVAR(cv_ticks, "ticks", 10000, "Number of headless ticks to run", mce::CVarFlags::None);

class HeadlessProbe final : public mce::app::Application {
public:
    mce::u64 ticks = 0;
    mce::Hasher64 hash;

    mce::ResultV on_init(mce::app::Context& ctx) override {
        if (!ctx.headless) return mce::Error(mce::Errc::Unsupported, "devhost.init", "devhost runs headless only");
        return mce::ok();
    }
    void on_tick(const mce::app::TickContext& t) override {
        ++ticks;
        hash.u64v(t.tick_index);
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

    mce::app::RunStats stats;
    const int rc = mce::app::run(app, desc, &stats);
    std::printf("devhost: %llu ticks, %llu frames, %.3f s sim, hash %016llx\n",
                static_cast<unsigned long long>(stats.ticks), static_cast<unsigned long long>(stats.frames),
                stats.sim_seconds, static_cast<unsigned long long>(app.hash.value()));
    mce::log::shutdown();
    return (rc == 0 && stats.ticks == desc.max_ticks) ? 0 : 1;
}
