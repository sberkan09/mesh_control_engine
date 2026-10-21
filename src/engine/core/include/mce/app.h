#pragma once
// Application shell and the one main loop (core.md §2.1; FR-RUN-020/025).
//
// mce::app::run() is the only loop in the engine. It is a "Fix Your Timestep" fixed-step loop:
// sim ticks are exactly 1/sim_hz, render gets an interpolation alpha, wall-clock catch-up is
// clamped at sim.max_catchup ticks per frame, and time_scale scales accumulator inflow (never dt).
//
// Core cannot see the platform or the renderer: they plug in through IPlatformHooks and
// IFrameFenceSource. With no platform hooks the loop runs headless (FR-RUN-025).

#include "mce/memory/frame_slots.h"
#include "mce/result.h"

namespace mce::input {
struct InputState;  // owned by mce_input; core only carries a reference (fix F2)
}

namespace mce::app {

using ActionSnapshot = input::InputState;  // F2 alias

// App-shell payload for one fixed tick. The ECS builds its Frame from this (fix F3).
struct TickContext {
    f64 dt = 0;                                  // exactly 1/sim_hz, never wall time
    u64 tick_index = 0;                          // 1-based
    const input::InputState* actions = nullptr;  // null until mce_input registers a source
};

struct RenderContext {
    f32 alpha = 0;        // interpolation factor in [0, 1)
    u64 frame_index = 0;  // 0-based
    f64 frame_dt = 0;     // clamped wall time of this frame (seconds)
};

class IClock {
public:
    virtual ~IClock() = default;
    virtual f64 now_seconds() = 0;
};

// Monotonic wall clock (std::chrono::steady_clock).
IClock& steady_clock();

class IPlatformHooks {
public:
    virtual ~IPlatformHooks() = default;
    // Drains OS events. Returns true when the user requested quit (window closed).
    virtual bool pump_events() = 0;
    virtual void present(bool vsync) = 0;
};

// Implemented by the renderer: a fence guarding the CPU frame-slot memory of the frame just
// submitted (ADR-0005). Headless: no source, slots are always reusable.
class IFrameFenceSource {
public:
    virtual ~IFrameFenceSource() = default;
    virtual mem::FrameSlotFence* fence_for_frame(u64 frame_index) = 0;
};

// Implemented by mce_input (fix F2: core sees only this seam). Called once per tick, before
// on_tick, with the end of the tick's input window on the clock's microsecond timeline:
//   tick_close_us(N) = origin_us + skipped_us + (N * 1'000'000) / sim_hz
// origin_us is the clock at loop start; skipped_us is wall time that never became sim time
// (spiral-of-death clamp, time_scale < 1). With neither, window ends are exact integers
// independent of render rate (FR-INP-030, audio-input-ui.md §3.2).
class IInputSource {
public:
    virtual ~IInputSource() = default;
    virtual const input::InputState* sample_for_tick(u64 tick, u64 tick_close_us) = 0;
};

struct AppDesc {
    const char* name = "mce";
    bool headless = false;        // also enabled by the `headless` convar
    u32 sim_hz = 60;              // fixed step; dt == 1/sim_hz exactly
    u64 max_ticks = 0;            // stop after N ticks (0 = run until quit)
    u64 max_frames = 0;           // stop after N frames (0 = run until quit)
    usize frame_arena_size = mem::FrameSlots::kDefaultArenaSize;
    Span<const char* const> args;  // argv; parsed into convars before on_init
    // Optional seams. A null clock means: steady clock with a window, or a stepping clock that
    // advances exactly one tick per frame when headless (deterministic harness runs).
    IClock* clock = nullptr;
    IPlatformHooks* platform = nullptr;
    IFrameFenceSource* fences = nullptr;
    const input::InputState* input = nullptr;  // fixed snapshot for every tick (tests, harnesses)
    IInputSource* input_source = nullptr;      // per-tick sampling; wins over `input`
};

struct Context {
    const AppDesc& desc;
    mem::FrameSlots& frame_slots;
    bool headless;
};

class Application {
public:
    virtual ~Application() = default;

    // Called once after services are up: register systems, load the boot scene.
    virtual ResultV on_init(Context& ctx) = 0;
    // One fixed sim tick. Do not sample wall time in here.
    virtual void on_tick(const TickContext& tick) = 0;
    // Render with interpolation alpha. Runs once per frame (0..N ticks per frame).
    virtual void on_render(const RenderContext&) {}
    virtual void on_shutdown() {}

    void quit(int exit_code = 0) {
        want_quit_ = true;
        exit_code_ = exit_code;
    }
    bool want_quit() const { return want_quit_; }
    int exit_code() const { return exit_code_; }

private:
    bool want_quit_ = false;
    int exit_code_ = 0;
};

struct RunStats {
    u64 ticks = 0;
    u64 frames = 0;
    u64 clamped_frames = 0;  // frames whose wall time hit the spiral-of-death clamp
    f64 sim_seconds = 0;     // ticks * dt
};

// Runs the application to completion; returns the process exit code. A failing on_init is
// reported (log + stderr) and returns 1.
int run(Application& app, const AppDesc& desc, RunStats* stats = nullptr);

}  // namespace mce::app
