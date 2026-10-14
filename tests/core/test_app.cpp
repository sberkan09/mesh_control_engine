#include <doctest/doctest.h>

#include "mce/app.h"
#include "mce/convar.h"
#include "mce/log.h"

using namespace mce;
using namespace mce::app;

namespace {

class CountingApp final : public Application {
public:
    u64 ticks = 0;
    u64 renders = 0;
    f64 dt_seen = 0;
    bool dt_constant = true;
    bool frame_arena_ok = true;
    u64 last_tick_index = 0;
    f32 max_alpha = 0;

    ResultV on_init(Context&) override { return ok(); }
    void on_tick(const TickContext& t) override {
        if (ticks > 0 && t.dt != dt_seen) dt_constant = false;
        dt_seen = t.dt;
        last_tick_index = t.tick_index;
        ++ticks;
        frame_arena_ok = frame_arena_ok && mem::has_frame_arena() && mem::frame().allocate(64).ptr != nullptr;
    }
    void on_render(const RenderContext& r) override {
        ++renders;
        if (r.alpha > max_alpha) max_alpha = r.alpha;
    }
};

class FailingApp final : public Application {
public:
    ResultV on_init(Context&) override { return Error(Errc::PlatformInit, "test.init"); }
    void on_tick(const TickContext&) override {}
};

// Fake wall clock advancing a fixed amount per query (simulates a render rate).
class FakeClock final : public IClock {
public:
    explicit FakeClock(f64 frame_seconds) : step_(frame_seconds) {}
    f64 now_seconds() override {
        const f64 t = static_cast<f64>(n_) * step_;
        ++n_;
        return t;
    }

private:
    f64 step_;
    u64 n_ = 0;
};

class FakePlatform final : public IPlatformHooks {
public:
    u64 pumps = 0, presents = 0;
    u64 quit_after = 0;
    bool pump_events() override { return ++pumps == quit_after; }
    void present(bool) override { ++presents; }
};

// Records the window end the loop hands each tick (the mce_input seam).
class RecordingInput final : public IInputSource {
public:
    u64 closes[64] = {};
    u64 count = 0;
    const input::InputState* sample_for_tick(u64 tick, u64 tick_close_us) override {
        if (tick <= 64) closes[tick - 1] = tick_close_us;
        count = tick;
        return nullptr;
    }
};

}  // namespace

TEST_CASE("input source gets exact rational tick window ends (FR-INP-030)") {
    CountingApp app;
    FakeClock clock(1.0 / 144.0);
    RecordingInput input;
    AppDesc desc;
    desc.headless = true;
    desc.clock = &clock;
    desc.input_source = &input;
    desc.max_ticks = 60;
    desc.frame_arena_size = 64 * 1024;
    run(app, desc);
    REQUIRE(input.count == 60);
    for (u64 n = 1; n <= 60; ++n) CHECK(input.closes[n - 1] == (n * 1'000'000ull) / 60);
    mem::set_frame_arena(nullptr);
}

TEST_CASE("clamped wall time shifts later input windows instead of piling up events") {
    CountingApp app;
    FakeClock clock(1.0);  // each frame takes a second; 5 ticks run, the rest is skipped
    RecordingInput input;
    AppDesc desc;
    desc.headless = true;
    desc.clock = &clock;
    desc.input_source = &input;
    desc.max_frames = 3;
    desc.frame_arena_size = 64 * 1024;
    run(app, desc);
    REQUIRE(input.count == 15);
    // Frame 1 covers wall [0,1): 5 ticks, 1 - 5/60 s skipped before they run.
    const u64 skipped_us = 1'000'000 - (5 * 1'000'000ull) / 60;
    CHECK(input.closes[0] == skipped_us + 1'000'000ull / 60);
    // Every tick's window still ends at or before the frame's wall time.
    for (u64 n = 1; n <= 15; ++n) CHECK(input.closes[n - 1] <= ((n + 4) / 5) * 1'000'000ull);
    mem::set_frame_arena(nullptr);
}

TEST_CASE("headless run ticks exactly max_ticks with a constant dt (FR-RUN-025)") {
    CountingApp app;
    AppDesc desc;
    desc.name = "test";
    desc.headless = true;
    desc.max_ticks = 10000;
    desc.frame_arena_size = 64 * 1024;
    RunStats stats;
    CHECK(run(app, desc, &stats) == 0);
    CHECK(app.ticks == 10000);
    CHECK(stats.ticks == 10000);
    CHECK(stats.frames == 10000);  // stepping clock: one tick per frame
    CHECK(app.dt_constant);
    CHECK(app.dt_seen == 1.0 / 60.0);
    CHECK(app.last_tick_index == 10000);
    CHECK(app.frame_arena_ok);
    mem::set_frame_arena(nullptr);
}

TEST_CASE("tick grid is independent of render rate (FR-RUN-020)") {
    // 30 fps render: 2 ticks per frame; 240 fps: one tick every 4 frames. Same sim time -> same ticks.
    for (f64 fps : {30.0, 240.0, 144.0}) {
        CountingApp app;
        FakeClock clock(1.0 / fps);
        AppDesc desc;
        desc.headless = true;
        desc.clock = &clock;
        desc.max_ticks = 600;  // 10 s of sim
        desc.frame_arena_size = 64 * 1024;
        RunStats stats;
        CHECK(run(app, desc, &stats) == 0);
        CHECK(app.ticks == 600);
        CHECK(app.dt_constant);
        CHECK(app.max_alpha < 1.0f);
        const f64 expected_frames = 10.0 * fps;
        CHECK(static_cast<f64>(stats.frames) == doctest::Approx(expected_frames).epsilon(0.01));
        mem::set_frame_arena(nullptr);
    }
}

TEST_CASE("spiral-of-death clamp limits catch-up ticks per frame") {
    CountingApp app;
    FakeClock clock(1.0);  // every frame takes a full second
    AppDesc desc;
    desc.headless = true;
    desc.clock = &clock;
    desc.max_frames = 10;
    desc.frame_arena_size = 64 * 1024;
    RunStats stats;
    run(app, desc, &stats);
    CHECK(app.ticks == 50);  // 5 per frame (sim.max_catchup default)
    CHECK(stats.clamped_frames == 10);
    mem::set_frame_arena(nullptr);
}

TEST_CASE("sim.fast_forward runs sim.max_catchup ticks every frame, however short the frame") {
    ConVar* ff = ConVar::find("sim.fast_forward");
    ConVar* mc = ConVar::find("sim.max_catchup");
    REQUIRE(ff);
    REQUIRE(mc);
    ff->set_bool(true);
    mc->set_int(32);
    CountingApp app;
    FakeClock clock(0.001);  // a 1 ms frame would run no tick at 60 Hz
    AppDesc desc;
    desc.headless = true;
    desc.clock = &clock;
    desc.max_frames = 10;
    desc.frame_arena_size = 64 * 1024;
    run(app, desc);
    ff->reset();
    mc->reset();
    CHECK(app.ticks >= 9 * 32);
    CHECK(app.ticks <= 10 * 32);
    CHECK(app.dt_constant);
    mem::set_frame_arena(nullptr);
}

TEST_CASE("time_scale scales accumulator inflow, never dt (FR-RUN-050)") {
    ConVar* ts = ConVar::find("time_scale");
    REQUIRE(ts);
    ts->set_float(0.25f);
    CountingApp app;
    FakeClock clock(1.0 / 60.0);
    AppDesc desc;
    desc.headless = true;
    desc.clock = &clock;
    desc.max_frames = 400;
    desc.frame_arena_size = 64 * 1024;
    run(app, desc);
    ts->reset();
    CHECK(app.ticks == 100);
    CHECK(app.dt_seen == 1.0 / 60.0);
    mem::set_frame_arena(nullptr);
}

TEST_CASE("platform hooks: quit request from the event pump ends the loop") {
    CountingApp app;
    FakePlatform platform;
    platform.quit_after = 7;
    FakeClock clock(1.0 / 60.0);
    AppDesc desc;
    desc.platform = &platform;
    desc.clock = &clock;
    desc.frame_arena_size = 64 * 1024;
    CHECK(run(app, desc) == 0);
    CHECK(platform.pumps == 7);
    CHECK(platform.presents == 7);
    mem::set_frame_arena(nullptr);
}

TEST_CASE("failing on_init returns a nonzero exit code") {
    FailingApp app;
    AppDesc desc;
    desc.headless = true;
    desc.frame_arena_size = 64 * 1024;
    CHECK(run(app, desc) == 1);
    mem::set_frame_arena(nullptr);
}

// MCE_LOG_* refuses a printf format at compile time (its % codes would be logged as they are).
static_assert(mce::log::detail::printf_conversion("gem %u of %u") == 4);
static_assert(mce::log::detail::printf_conversion("%-8.3lf") == 0);
static_assert(mce::log::detail::printf_conversion("{} at 50% done, 100%% sure, {:.1f}%") < 0);

TEST_CASE("log: fmt formats pass the printf check") {
    MCE_LOG_INFO("app test: {} of {} ({:.0f}%)", 1, 2, 50.0);
    MCE_LOG_INFO("app test: no arguments");
    CHECK(true);
}
