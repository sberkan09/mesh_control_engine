#include "mce/editor/perf_monitor.h"

#include <algorithm>
#include <cmath>

namespace mce::editor {

const char* perf_stage_name(PerfStage s) {
    switch (s) {
        case PerfStage::Frame: return "frame";
        case PerfStage::Sim: return "simulation";
        case PerfStage::Ui: return "editor UI";
        case PerfStage::Record: return "cull + sort + record";
        case PerfStage::Submit: return "RHI translate + submit";
        case PerfStage::Gpu: return "GPU";
        case PerfStage::Count: break;
    }
    return "?";
}

f32 perf_stage_value(const FrameSample& s, PerfStage stage) {
    switch (stage) {
        case PerfStage::Frame: return s.frame_ms;
        case PerfStage::Sim: return s.sim_ms;
        case PerfStage::Ui: return s.ui_ms;
        case PerfStage::Record: return s.record_ms;
        case PerfStage::Submit: return s.submit_ms;
        case PerfStage::Gpu: return s.gpu_ms;
        case PerfStage::Count: break;
    }
    return -1.0f;
}

PerfMonitor::PerfMonitor() { ring_.resize(kCapacity); }

void PerfMonitor::push(const FrameSample& s) {
    if (frozen_) return;
    ring_[head_] = s;
    head_ = (head_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
    ++seen_;
}

void PerfMonitor::clear() {
    head_ = 0;
    count_ = 0;
}

const FrameSample& PerfMonitor::at(u32 i) const {
    MCE_ASSERT(i < count_);
    return ring_[(head_ + kCapacity - count_ + i) % kCapacity];
}

PerfSummary PerfMonitor::summarize(PerfStage stage) const {
    f32 values[kCapacity];
    u32 n = 0;
    f64 sum = 0.0;
    for (u32 i = 0; i < count_; ++i) {
        const f32 v = perf_stage_value(at(i), stage);
        if (v < 0.0f) continue;
        values[n++] = v;
        sum += f64(v);
    }
    PerfSummary out;
    out.samples = n;
    if (n == 0) return out;
    std::sort(values, values + n);
    // Nearest-rank percentiles: the smallest value with at least p of the samples at or below it.
    auto rank = [&](f32 p) {
        const u32 k = static_cast<u32>(std::ceil(p * static_cast<f32>(n)));
        return values[k == 0 ? 0 : k - 1];
    };
    out.mean = static_cast<f32>(sum / f64(n));
    out.p50 = rank(0.50f);
    out.p95 = rank(0.95f);
    out.p99 = rank(0.99f);
    out.max = values[n - 1];
    return out;
}

void PerfMonitor::series(PerfStage stage, Array<f32>& out) const {
    out.clear();
    for (u32 i = 0; i < count_; ++i) out.push_back(std::max(perf_stage_value(at(i), stage), 0.0f));
}

void PerfMonitor::budget(Array<BudgetRow>& out) const {
    struct Line {
        const char* label;
        PerfStage stage;
        f32 ms;
    };
    static constexpr Line kLines[] = {
        {"frame (60 fps)", PerfStage::Frame, kFrameBudgetMs},
        {"simulation", PerfStage::Sim, 3.0f},
        {"cull + sort + record", PerfStage::Record, 1.5f},
        {"RHI translate + submit", PerfStage::Submit, 3.5f},
        {"GPU", PerfStage::Gpu, 6.0f},
        {"editor UI (headroom)", PerfStage::Ui, 2.6f},
    };
    out.clear();
    for (const Line& l : kLines) {
        BudgetRow r;
        r.label = l.label;
        r.stage = l.stage;
        r.budget_ms = l.ms;
        r.measured = summarize(l.stage);
        out.push_back(r);
    }
}

u32 PerfMonitor::slow_frames() const {
    u32 n = 0;
    for (u32 i = 0; i < count_; ++i) n += at(i).frame_ms > kFrameBudgetMs ? 1u : 0u;
    return n;
}

void PerfMonitor::rank_systems(const World& world, u32 top, Array<SystemTiming>& out) {
    world.system_timings(out);
    usize kept = 0;
    for (usize i = 0; i < out.size(); ++i)
        if (out[i].runs > 0) out[kept++] = out[i];
    out.resize(kept);
    // Stable: equal costs keep phase/execution order.
    std::stable_sort(out.begin(), out.end(),
                     [](const SystemTiming& a, const SystemTiming& b) { return a.mean_ms() > b.mean_ms(); });
    if (out.size() > top) out.resize(top);
}

void PerfMonitor::to_markdown(cook::String& out, Span<const SystemTiming> systems) const {
    cook::appendf(out, "Editor frames: %u (of %llu seen), %u over %.1f ms\n\n", count_,
                  static_cast<unsigned long long>(seen_), slow_frames(), static_cast<f64>(kFrameBudgetMs));
    out += "| Stage | Budget ms | mean | p50 | p95 | p99 | max |\n|---|---:|---:|---:|---:|---:|---:|\n";
    Array<BudgetRow> rows;
    budget(rows);
    for (const BudgetRow& r : rows) {
        if (r.measured.samples == 0) {
            cook::appendf(out, "| %s | %.1f | n/a | | | | |\n", r.label, static_cast<f64>(r.budget_ms));
            continue;
        }
        const PerfSummary& m = r.measured;
        cook::appendf(out, "| %s | %.1f | %.2f | %.2f | %.2f%s | %.2f | %.2f |\n", r.label, static_cast<f64>(r.budget_ms),
                      static_cast<f64>(m.mean), static_cast<f64>(m.p50), static_cast<f64>(m.p95), r.over() ? " (over)" : "",
                      static_cast<f64>(m.p99), static_cast<f64>(m.max));
    }
    if (systems.empty()) return;
    out += "\n| System | Phase | mean ms | last ms | runs |\n|---|---|---:|---:|---:|\n";
    for (const SystemTiming& s : systems)
        cook::appendf(out, "| %s | %s | %.3f | %.3f | %llu |\n", s.name, phase_name(s.phase), s.mean_ms(), s.last_ms,
                      static_cast<unsigned long long>(s.runs));
}

}  // namespace mce::editor
