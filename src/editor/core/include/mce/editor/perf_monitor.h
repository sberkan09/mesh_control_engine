#pragma once
// PerfMonitor: the data behind the editor's Profiler panel (ROADMAP M7 "editor perf dashboards",
// FR-EDT-140). Headless.
//
// The editor measures each frame's stages and pushes one FrameSample. The monitor keeps a rolling
// history (a few seconds) and summarizes it: mean / p50 / p95 / p99 / max per stage, and the
// NFR-F-01/F-02 budget table, where each stage's p95 is set against its allocation. While playing,
// the play world's per-system timings (World::set_profiling) rank the systems by cost. The whole
// report renders as Markdown, the shape the milestone budget tables use.

#include "mce/containers/array.h"
#include "mce/cook/string.h"
#include "mce/ecs/world.h"

namespace mce::editor {

// One editor frame, milliseconds. A stage that was not measured this frame is negative.
struct FrameSample {
    f32 frame_ms = 0.0f;   // wall time since the previous frame
    f32 sim_ms = 0.0f;     // play-in-editor ticks run this frame (0 when not playing)
    u32 sim_ticks = 0;
    f32 ui_ms = 0.0f;      // building the panels, the sim excluded
    f32 record_ms = 0.0f;  // command recording: the viewport's scene (cull, sort) and the UI
    f32 submit_ms = 0.0f;  // RHI translate + GL submit
    f32 gpu_ms = -1.0f;    // the GPU frame from timestamp queries, when the device has them
};

enum class PerfStage : u8 { Frame, Sim, Ui, Record, Submit, Gpu, Count };
const char* perf_stage_name(PerfStage s);
f32 perf_stage_value(const FrameSample& s, PerfStage stage);

struct PerfSummary {
    f32 mean = 0.0f, p50 = 0.0f, p95 = 0.0f, p99 = 0.0f, max = 0.0f;
    u32 samples = 0;  // frames that measured the stage
};

// One line of the budget table: the NFR-F-02 allocation for the stage and the measured p95.
struct BudgetRow {
    const char* label = "";
    PerfStage stage = PerfStage::Frame;
    f32 budget_ms = 0.0f;
    PerfSummary measured;
    bool over() const { return measured.samples > 0 && measured.p95 > budget_ms; }
};

class PerfMonitor {
public:
    static constexpr u32 kCapacity = 300;  // 5 s at 60 fps
    static constexpr f32 kFrameBudgetMs = 16.6f;  // NFR-B-06 / NFR-F-01: 60 fps

    PerfMonitor();

    // Ignored while frozen (the panel's Pause keeps a spike on screen).
    void push(const FrameSample& s);
    void clear();
    void set_frozen(bool frozen) { frozen_ = frozen; }
    bool frozen() const { return frozen_; }

    u32 size() const { return count_; }
    const FrameSample& at(u32 i) const;  // 0 = oldest
    u64 frames_seen() const { return seen_; }

    PerfSummary summarize(PerfStage stage) const;
    // The stage's values oldest-first (unmeasured frames as 0), for a plot.
    void series(PerfStage stage, Array<f32>& out) const;
    // NFR-F-02 rows: frame 16.6, sim 3.0, cull + sort + record 1.5, translate + submit 3.5, GPU 6.0,
    // and the editor's own UI against the 2.6 ms headroom.
    void budget(Array<BudgetRow>& out) const;
    // Frames over the 16.6 ms frame budget in the history.
    u32 slow_frames() const;

    // The play world's systems, most expensive (mean) first, at most `top` of them.
    static void rank_systems(const World& world, u32 top, Array<SystemTiming>& out);

    // The summaries, the budget table and (when given) the ranked systems as Markdown tables.
    void to_markdown(cook::String& out, Span<const SystemTiming> systems = {}) const;

private:
    Array<FrameSample> ring_;
    u32 head_ = 0;  // next write
    u32 count_ = 0;
    u64 seen_ = 0;
    bool frozen_ = false;
};

}  // namespace mce::editor
