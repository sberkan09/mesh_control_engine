#include "mce/app.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "mce/convar.h"
#include "mce/log.h"

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#define MCE_FRAME_MARK() FrameMark
#define MCE_ZONE(name) ZoneScopedN(name)
#else
#define MCE_FRAME_MARK() ((void)0)
#define MCE_ZONE(name) ((void)0)
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
// A browser only paints and delivers input between tasks, so the loop gives the page back once a
// frame and resumes on the next animation frame (ASYNCIFY, cmake/MceWeb.cmake; ADR-0039).
EM_ASYNC_JS(void, mce_web_next_frame, (), { await new Promise((resolve) => requestAnimationFrame(resolve)); });
#endif

namespace mce::app {
namespace {

MCE_CVAR(cv_time_scale, "time_scale", 1.0f, "Sim speed multiplier (scales accumulator inflow, not dt)", CVarFlags::Cheat);
MCE_CVAR(cv_max_catchup, "sim.max_catchup", 5, "Max fixed ticks per frame (spiral-of-death clamp)", CVarFlags::None);
MCE_CVAR(cv_fast_forward, "sim.fast_forward", false,
         "Every frame runs sim.max_catchup ticks whatever the wall clock (captures of late ticks; never for play)", CVarFlags::None);
MCE_CVAR(cv_headless, "headless", false, "Run without window, renderer or audio device", CVarFlags::RequiresRestart);
MCE_CVAR(cv_vsync, "r.vsync", true, "Present with vsync", CVarFlags::Saved);

class SteadyClock final : public IClock {
public:
    f64 now_seconds() override {
        using namespace std::chrono;
        return duration<f64>(steady_clock::now().time_since_epoch()).count();
    }
};

// Advances exactly one fixed step per query: headless runs tick once per frame, deterministically.
class SteppingClock final : public IClock {
public:
    explicit SteppingClock(f64 step) : step_(step) {}
    f64 now_seconds() override {
        const f64 t = static_cast<f64>(calls_) * step_;
        ++calls_;
        return t;
    }

private:
    f64 step_;
    u64 calls_ = 0;
};

}  // namespace

IClock& steady_clock() {
    static SteadyClock c;
    return c;
}

int run(Application& app, const AppDesc& desc, RunStats* stats_out) {
    if (!desc.args.empty()) console::parse_command_line(static_cast<int>(desc.args.size()), desc.args.data());

    const bool headless = desc.headless || cv_headless.get_bool() || desc.platform == nullptr;
    const f64 fixed_dt = 1.0 / static_cast<f64>(desc.sim_hz);

    SteppingClock stepping(fixed_dt);
    IClock& clock = desc.clock ? *desc.clock : (headless ? static_cast<IClock&>(stepping) : steady_clock());

    mem::FrameSlots frame_slots(mem::heap(), desc.frame_arena_size);
    Context ctx{desc, frame_slots, headless};

    if (ResultV r = app.on_init(ctx); !r) {
        FixedString<512> msg;
        format_error(r.error(), msg);
        MCE_LOG_CRITICAL("{}: initialization failed: {}", desc.name, msg.c_str());
        std::fprintf(stderr, "%s: initialization failed: %s\n", desc.name, msg.c_str());
        return 1;
    }
    MCE_LOG_INFO("{}: running ({}, {} Hz)", desc.name, headless ? "headless" : "windowed", desc.sim_hz);

    RunStats stats;
    f64 previous = clock.now_seconds();
    const u64 origin_us = previous > 0.0 ? static_cast<u64>(std::llround(previous * 1e6)) : 0;
    f64 skipped = 0.0;  // wall seconds that did not become sim time (clamp, time_scale); signed
    f64 accumulator = 0.0;
    u64 tick = 0;
    u64 frame = 0;

    while (!app.want_quit()) {
        MCE_FRAME_MARK();
        frame_slots.begin_frame(frame);

        const i32 max_catchup = cv_max_catchup.get_int() > 0 ? cv_max_catchup.get_int() : 1;
        const f64 max_frame_time = static_cast<f64>(max_catchup) * fixed_dt;
        const f64 now = clock.now_seconds();
        f64 frame_time = now - previous;
        previous = now;
        if (cv_fast_forward.get_bool()) {
            skipped += frame_time - max_frame_time;  // negative: the sim runs ahead of the wall
            frame_time = max_frame_time;
        } else if (frame_time > max_frame_time) {
            skipped += frame_time - max_frame_time;
            frame_time = max_frame_time;
            ++stats.clamped_frames;
        }
        if (frame_time < 0.0) frame_time = 0.0;
        const f64 scale = static_cast<f64>(cv_time_scale.get_float());
        accumulator += frame_time * scale;
        if (scale != 1.0) skipped += frame_time * (1.0 - scale);

        if (desc.platform && desc.platform->pump_events()) app.quit(0);

        {
            MCE_ZONE("sim");
            // Small epsilon absorbs accumulated rounding so exact multiples of dt tick.
            while (accumulator + 1e-9 >= fixed_dt && !app.want_quit()) {
                TickContext tc;
                tc.dt = fixed_dt;
                tc.tick_index = ++tick;
                tc.actions = desc.input;
                if (desc.input_source) {
                    // skipped is negative under time_scale > 1 (sim runs ahead of the wall).
                    const i64 skipped_us = static_cast<i64>(std::llround(skipped * 1e6));
                    const i64 close = static_cast<i64>(origin_us + (tick * 1'000'000ull) / desc.sim_hz) + skipped_us;
                    const u64 close_us = close > 0 ? static_cast<u64>(close) : 0;
                    tc.actions = desc.input_source->sample_for_tick(tick, close_us);
                }
                app.on_tick(tc);
                accumulator -= fixed_dt;
                if (desc.max_ticks && tick >= desc.max_ticks) app.quit(0);
            }
            if (accumulator < 0.0) accumulator = 0.0;
        }

        {
            MCE_ZONE("render");
            RenderContext rc;
            rc.alpha = static_cast<f32>(accumulator / fixed_dt);
            rc.frame_index = frame;
            rc.frame_dt = frame_time;
            app.on_render(rc);
        }

        frame_slots.end_frame(desc.fences ? desc.fences->fence_for_frame(frame) : nullptr);
        if (desc.platform) desc.platform->present(cv_vsync.get_bool());
#if defined(__EMSCRIPTEN__)
        mce_web_next_frame();
#endif

        ++frame;
        if (desc.max_frames && frame >= desc.max_frames) app.quit(0);
    }

    app.on_shutdown();
    stats.ticks = tick;
    stats.frames = frame;
    stats.sim_seconds = static_cast<f64>(tick) * fixed_dt;
    if (stats_out) *stats_out = stats;
    MCE_LOG_INFO("{}: exit {} after {} ticks / {} frames", desc.name, app.exit_code(), tick, frame);
    return app.exit_code();
}

}  // namespace mce::app
