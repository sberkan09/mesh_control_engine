#include "mce/editor/perf_compare.h"

#include <algorithm>
#include <cmath>

namespace mce::editor {

StringView perf_phase_name(const perf::Capture& c, u8 phase) {
    return phase < c.phases.size() ? c.phases[phase].view() : StringView("?");
}

f64 system_ms_per_frame(const perf::Capture& c, const perf::SystemStats& s) {
    return c.frames.empty() ? 0.0 : s.total_ms / static_cast<f64>(c.frames.size());
}

void rank_capture_systems(const perf::Capture& c, Array<usize>& order) {
    order.clear();
    for (usize i = 0; i < c.systems.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](usize a, usize b) {
        return c.systems[a].total_ms != c.systems[b].total_ms ? c.systems[a].total_ms > c.systems[b].total_ms : a < b;
    });
}

void compare_captures(const perf::Capture& a, const perf::Capture& b, Comparison& out) {
    out = Comparison{};
    out.same_scene = a.scene.view() == b.scene.view();
    out.before = perf::summarize(a);
    out.after = perf::summarize(b);
    const perf::Summary &sa = out.before, &sb = out.after;

    const perf::Stat* stats[4][2] = {{&sa.frame, &sb.frame}, {&sa.sim, &sb.sim}, {&sa.render, &sb.render}, {&sa.gpu, &sb.gpu}};
    const char* names[4] = {"frame_ms", "sim_ms", "render_ms", "gpu_ms"};
    for (usize i = 0; i < 4; ++i) {
        TimeChange& t = out.times[i];
        const perf::Stat &x = *stats[i][0], &y = *stats[i][1];
        t.metric = names[i];
        t.measured = x.count > 0 && y.count > 0;
        if (!t.measured) continue;
        t.p50_before = x.p50;
        t.p50_after = y.p50;
        t.p95_before = x.p95;
        t.p95_after = y.p95;
        const f64 d = static_cast<f64>(y.p95) - static_cast<f64>(x.p95);
        const f64 pct = x.p95 > 0 ? d / static_cast<f64>(x.p95) * 100.0 : 0.0;
        t.p95_change_pct = std::round(pct * 10.0) / 10.0;
        if (std::fabs(d) >= 0.1 && std::fabs(pct) >= 10.0) (d < 0 ? out.better : out.worse).push_back(names[i]);
    }

    const perf::Counters &ka = sa.counters_max, &kb = sb.counters_max;
    const auto counter = [&](const char* name, u64 x, u64 y) {
        out.counters.push_back({name, x, y});
        if (x != y) (y < x ? out.better : out.worse).push_back(name);
    };
    counter("draws", ka.draws, kb.draws);
    counter("passes", ka.passes, kb.passes);
    counter("triangles", ka.triangles, kb.triangles);
    counter("meshes", ka.meshes, kb.meshes);
    counter("sprites", ka.sprites, kb.sprites);
    counter("sprite_batches", ka.sprite_batches, kb.sprite_batches);
    counter("upload_bytes", ka.upload_bytes, kb.upload_bytes);
    counter("heap_allocations", ka.heap_allocations, kb.heap_allocations);

    for (const perf::SystemStats& s : a.systems) {
        SystemChange r;
        r.name = s.name.view();
        r.phase = perf_phase_name(a, s.phase);
        r.before_ms = system_ms_per_frame(a, s);
        out.systems.push_back(r);
    }
    for (const perf::SystemStats& s : b.systems) {
        bool hit = false;
        for (SystemChange& r : out.systems)
            if (r.name.view() == s.name.view() && r.phase.view() == perf_phase_name(b, s.phase)) {
                r.after_ms = system_ms_per_frame(b, s);
                hit = true;
            }
        if (hit) continue;
        SystemChange r;
        r.name = s.name.view();
        r.phase = perf_phase_name(b, s.phase);
        r.after_ms = system_ms_per_frame(b, s);
        out.systems.push_back(r);
    }
    std::sort(out.systems.begin(), out.systems.end(), [](const SystemChange& x, const SystemChange& y) {
        const f64 dx = std::fabs(x.after_ms - x.before_ms), dy = std::fabs(y.after_ms - y.before_ms);
        return dx != dy ? dx > dy : x.name.view() < y.name.view();
    });
}

void frame_parts(const perf::Capture& c, const perf::Frame& f, Array<FramePart>& out) {
    out.clear();
    const usize phases = std::min<usize>(c.phases.size(), perf::kMaxPhases);
    for (u8 kind = FramePart::Sim; kind <= FramePart::Render; ++kind) {
        const usize first = out.size();
        for (usize i = 0; i < phases; ++i) {
            const bool render = (c.render_phases >> i) & 1u;
            if (render != (kind == FramePart::Render) || f.phase_ms[i] <= 0.0f) continue;
            out.push_back({static_cast<FramePart::Kind>(kind), c.phases[i].view(), f.phase_ms[i]});
        }
        std::stable_sort(out.begin() + first, out.end(), [](const FramePart& x, const FramePart& y) { return x.ms > y.ms; });
    }
    const usize first = out.size();
    const usize stages = std::min<usize>(c.gpu_stages.size(), perf::kMaxGpuStages);
    for (usize i = 0; i < stages; ++i)
        if (f.gpu_stage_ms[i] > 0.0f) out.push_back({FramePart::Gpu, c.gpu_stages[i].view(), f.gpu_stage_ms[i]});
    std::stable_sort(out.begin() + first, out.end(), [](const FramePart& x, const FramePart& y) { return x.ms > y.ms; });
}

}  // namespace mce::editor
