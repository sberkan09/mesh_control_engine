#pragma once
// Two .mcperf captures side by side, and one captured frame taken apart (M-PERF steps 3-4): the
// model behind MCP's perf_compare and the editor Profiler panel's capture view. Headless.
//
// Times compare on p50 and p95; a change of 10% and 0.1 ms or more counts as better or worse
// (timings are noisy). Counters compare on their worst frame, and any change counts (they repeat
// run to run). Systems compare on time per captured frame, matched by name and phase.

#include "mce/fixed_string.h"
#include "mce/perf_capture.h"

namespace mce::editor {

struct TimeChange {
    const char* metric = "";  // frame_ms, sim_ms, render_ms, gpu_ms
    bool measured = false;    // both captures measured it
    f32 p50_before = 0, p50_after = 0, p95_before = 0, p95_after = 0;
    f64 p95_change_pct = 0.0;
};

struct CounterChange {
    const char* metric = "";  // draws, passes, ...
    u64 before = 0, after = 0;
};

struct SystemChange {
    FixedString<64> name;
    FixedString<32> phase;
    f64 before_ms = 0.0, after_ms = 0.0;  // per captured frame; 0 where the capture lacks it
};

struct Comparison {
    bool same_scene = true;
    perf::Summary before, after;
    TimeChange times[4];
    Array<CounterChange> counters;
    Array<SystemChange> systems;  // the largest move first
    Array<const char*> better, worse;  // metric names
};

void compare_captures(const perf::Capture& before, const perf::Capture& after, Comparison& out);

// The capture's phase name for `phase` ("?" when out of range).
StringView perf_phase_name(const perf::Capture& c, u8 phase);
// A system's time per captured frame.
f64 system_ms_per_frame(const perf::Capture& c, const perf::SystemStats& s);
// System indices by total time, slowest first.
void rank_capture_systems(const perf::Capture& c, Array<usize>& order);

// One line of a frame's breakdown: a sim phase, a render phase or a GPU stage.
struct FramePart {
    enum Kind : u8 { Sim, Render, Gpu } kind = Sim;
    StringView name;
    f32 ms = 0.0f;
};
// The frame's measured parts with time in them, slowest first within each kind (sim, render, GPU).
void frame_parts(const perf::Capture& c, const perf::Frame& f, Array<FramePart>& out);

}  // namespace mce::editor
