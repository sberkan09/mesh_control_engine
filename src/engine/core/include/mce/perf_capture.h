#pragma once
// .mcperf captures (M-PERF step 1): what each rendered frame cost, as the stock player measured it.
//
// A capture is a list of frames (wall time, sim and render CPU time, GPU time and stages, sim time
// per ECS phase, render counters, live heap allocations) plus per-system totals for the run. The
// player fills one with -perf_capture <file>; summarize() reduces it to percentiles and the spike
// frames; write_mcperf() renders both as JSON ("format": "mce-perf"), which the MCP server and the
// editor read with the cook's JSON parser (the runtime never parses text, FR-AST-010).
//
// Times depend on the machine that ran the capture; the counters do not, so gates compare those.
// Recording reads clocks and counters only: a capture never changes the simulation or its hash.

#include "mce/containers/array.h"
#include "mce/draw_cause.h"
#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::perf {

inline constexpr u32 kFormatVersion = 1;
inline constexpr u32 kMaxPhases = 8;      // ECS phases with sim/render time (ecs::kPhaseCount fits)
inline constexpr u32 kMaxGpuStages = 16;  // named GPU stages (Forward3D's, M-VISUAL)
inline constexpr u32 kMaxSpikes = 16;

// Machine-independent work of one frame. Zero where the frame drew nothing (headless, 2D-only).
struct Counters {
    u32 draws = 0;           // RHI draw calls (indirect submits count once)
    u32 passes = 0;          // render passes
    u32 dispatches = 0;      // compute dispatches
    u32 triangles = 0;       // 3D triangles that survived culling
    u32 meshes = 0;          // 3D submeshes that survived culling
    u32 sprites = 0;         // 2D quads written (sprites, glyphs) plus static tile quads
    u32 sprite_batches = 0;  // 2D batch groups (each one indirect submit)
    u32 ring_overflows = 0;  // RHI upload rings that ran out this frame
    u64 upload_bytes = 0;    // geometry + uniform + indirect ring bytes written this frame
    u64 heap_allocations = 0;  // live heap allocations at the end of the frame
    u32 runs_instanced = 0;    // 3D: instanced draws of identical meshes, main view (M-VISUAL3 5a)
    u32 draws_instanced = 0;   // 3D: the submesh draws folded into them
    // 3D, main view (M-CONTENT 2): submesh draws that were not in an instanced run, by DrawCause. With
    // draws_instanced (the renderer's drawn instanced draws, taken from the same view) they add up to that
    // view's scene draws; zero where the frame drew no 3D. A capture from before M-CONTENT 2 has no causes.
    u32 draw_causes[kDrawCauseCount] = {};
};

struct Frame {
    u64 index = 0;        // the app's frame index (0-based)
    u64 tick = 0;         // sim ticks completed when the frame drew
    u32 ticks = 0;        // sim ticks run since the previous frame
    f32 frame_ms = -1.0f; // wall time since the previous frame started (-1: the first frame)
    f32 sim_ms = 0.0f;    // CPU time in the ticks run since the previous frame
    f32 render_ms = 0.0f; // CPU time of the frame itself (render systems, extract, record, submit)
    f32 gpu_ms = -1.0f;   // the frame's GPU time (-1: not measured)
    f32 phase_ms[kMaxPhases] = {};  // system time per ECS phase (Capture::phases names them)
    f32 gpu_stage_ms[kMaxGpuStages];  // per Capture::gpu_stages; -1 = not measured
    Counters counters;

    Frame() {
        for (f32& v : gpu_stage_ms) v = -1.0f;
    }
};

struct SystemStats {
    FixedString<64> name;
    u8 phase = 0;            // index into Capture::phases
    f64 total_ms = 0.0;      // over the captured frames
    u64 runs = 0;
    f32 max_frame_ms = 0.0f; // the most it took in any one frame
};

struct Capture {
    FixedString<64> app;
    FixedString<32> backend;  // RHI backend, or "headless"
    FixedString<64> scene;    // the scene's guid or name
    u32 width = 0, height = 0;  // backbuffer
    u32 sim_hz = 0;
    Array<FixedString<32>> phases;      // names of Frame::phase_ms
    u32 render_phases = 0;              // bit i: phase i runs per frame (render), else per tick (sim)
    Array<FixedString<32>> gpu_stages;  // names of Frame::gpu_stage_ms
    Array<Frame> frames;
    Array<SystemStats> systems;
};

// avg / p50 / p95 / p99 / max over the measured values (negative = not measured, skipped).
struct Stat {
    u32 count = 0;
    f32 avg = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
};
Stat stat_of(Span<const f32> values);

// A frame well over the run's median: at least twice the median frame time and 1 ms more.
struct Spike {
    u64 frame = 0;
    u64 tick = 0;
    f32 frame_ms = 0;
    FixedString<64> cause;  // "sim: FixedUpdate_Post", "render", "gpu: shadow" or "wait"
};

struct Summary {
    u32 frames = 0;
    Stat frame, sim, render, gpu;
    Counters counters_max;  // the largest value of each counter in any frame
    Counters counters_avg;  // each counter's mean over the frames, rounded down
    Array<Spike> spikes;    // the worst first, at most kMaxSpikes
};

Summary summarize(const Capture& c);

// JSON text of the capture with its summary (appended to `out`, no terminator).
void write_mcperf(const Capture& c, const Summary& s, Array<char>& out);
// summarize + write_mcperf to `path`.
ResultV save_mcperf(const Capture& c, const char* path);

}  // namespace mce::perf
