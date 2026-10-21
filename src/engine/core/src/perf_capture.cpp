#include "mce/perf_capture.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace mce::perf {

namespace {

f32 percentile(const Array<f32>& sorted, u32 pct) {
    const usize i = std::min(sorted.size() - 1, (sorted.size() * pct) / 100);
    return sorted[i];
}

// The phase or GPU stage that grew the most over its median: the spike's likely cause.
struct Column {
    const char* group;  // "sim", "render", "gpu"
    StringView name;
    f32 value;
    f32 median;
};

f32 median_of(const Capture& c, f32 (*get)(const Frame&, u32), u32 col) {
    Array<f32> v;
    v.reserve(c.frames.size());
    for (const Frame& f : c.frames)
        if (const f32 x = get(f, col); x >= 0.0f) v.push_back(x);
    if (v.empty()) return -1.0f;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

f32 get_sim(const Frame& f, u32) { return f.sim_ms; }
f32 get_render(const Frame& f, u32) { return f.render_ms; }
f32 get_gpu(const Frame& f, u32) { return f.gpu_ms; }
f32 get_phase(const Frame& f, u32 i) { return f.phase_ms[i]; }
f32 get_stage(const Frame& f, u32 i) { return f.gpu_stage_ms[i]; }

class Out {
public:
    explicit Out(Array<char>& out) : out_(out) {}
    void raw(StringView s) {
        for (char ch : s) out_.push_back(ch);
    }
    void fmt(const char* f, ...) {
        char buf[256];
        va_list ap;
        va_start(ap, f);
        const int n = std::vsnprintf(buf, sizeof buf, f, ap);
        va_end(ap);
        if (n > 0) raw(StringView(buf, std::min(static_cast<usize>(n), sizeof buf - 1)));
    }
    void str(StringView s) {
        out_.push_back('"');
        for (char ch : s) {
            if (ch == '"' || ch == '\\') {
                out_.push_back('\\');
                out_.push_back(ch);
            } else if (static_cast<unsigned char>(ch) < 0x20) {
                fmt("\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
            } else {
                out_.push_back(ch);
            }
        }
        out_.push_back('"');
    }
    // Times print with three decimals (a microsecond); -1 stays -1.
    void ms(f32 v) {
        if (v < 0.0f)
            raw("-1");
        else
            fmt("%.3f", static_cast<f64>(v));
    }

private:
    Array<char>& out_;
};

void write_stat(Out& o, const char* name, const Stat& s) {
    o.fmt("\"%s\": {\"count\": %u, \"avg\": ", name, s.count);
    o.ms(s.avg);
    o.raw(", \"p50\": ");
    o.ms(s.p50);
    o.raw(", \"p95\": ");
    o.ms(s.p95);
    o.raw(", \"p99\": ");
    o.ms(s.p99);
    o.raw(", \"max\": ");
    o.ms(s.max);
    o.raw("}");
}

void write_counters(Out& o, const Counters& c) {
    o.fmt("{\"draws\": %u, \"passes\": %u, \"dispatches\": %u, \"triangles\": %u, \"meshes\": %u, \"sprites\": %u, "
          "\"sprite_batches\": %u, ",
          c.draws, c.passes, c.dispatches, c.triangles, c.meshes, c.sprites, c.sprite_batches);
    o.fmt("\"ring_overflows\": %u, \"upload_bytes\": %llu, \"heap_allocations\": %llu, ", c.ring_overflows,
          static_cast<unsigned long long>(c.upload_bytes), static_cast<unsigned long long>(c.heap_allocations));
    o.fmt("\"runs_instanced\": %u, \"draws_instanced\": %u, \"merged_groups\": %u, \"merged_children\": %u, \"decals_dropped\": %u, \"draw_causes\": [", c.runs_instanced,
          c.draws_instanced, c.merged_groups, c.merged_children, c.decals_dropped);
    for (u32 k = 0; k < kDrawCauseCount; ++k) o.fmt(k ? ", %u" : "%u", c.draw_causes[k]);
    o.raw("]}");
}

void write_names(Out& o, const Array<FixedString<32>>& names) {
    o.raw("[");
    for (usize i = 0; i < names.size(); ++i) {
        if (i) o.raw(", ");
        o.str(names[i].view());
    }
    o.raw("]");
}

}  // namespace

Stat stat_of(Span<const f32> values) {
    Array<f32> v;
    v.reserve(values.size());
    f64 sum = 0.0;
    for (f32 x : values)
        if (x >= 0.0f) {
            v.push_back(x);
            sum += x;
        }
    Stat s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.count = static_cast<u32>(v.size());
    s.avg = static_cast<f32>(sum / static_cast<f64>(v.size()));
    s.p50 = percentile(v, 50);
    s.p95 = percentile(v, 95);
    s.p99 = percentile(v, 99);
    s.max = v.back();
    return s;
}

Summary summarize(const Capture& c) {
    Summary s;
    s.frames = static_cast<u32>(c.frames.size());
    if (c.frames.empty()) return s;
    Array<f32> col;
    col.resize(c.frames.size());
    const auto stat = [&](f32 (*get)(const Frame&, u32)) {
        for (usize i = 0; i < c.frames.size(); ++i) col[i] = get(c.frames[i], 0);
        return stat_of(Span<const f32>(col.data(), col.size()));
    };
    s.frame = stat([](const Frame& f, u32) { return f.frame_ms; });
    s.sim = stat(get_sim);
    s.render = stat(get_render);
    s.gpu = stat(get_gpu);

    Counters sum;
    for (const Frame& f : c.frames) {
        const Counters& k = f.counters;
        Counters& m = s.counters_max;
        m.draws = std::max(m.draws, k.draws);
        m.passes = std::max(m.passes, k.passes);
        m.dispatches = std::max(m.dispatches, k.dispatches);
        m.triangles = std::max(m.triangles, k.triangles);
        m.meshes = std::max(m.meshes, k.meshes);
        m.sprites = std::max(m.sprites, k.sprites);
        m.sprite_batches = std::max(m.sprite_batches, k.sprite_batches);
        m.ring_overflows = std::max(m.ring_overflows, k.ring_overflows);
        m.upload_bytes = std::max(m.upload_bytes, k.upload_bytes);
        m.heap_allocations = std::max(m.heap_allocations, k.heap_allocations);
        m.runs_instanced = std::max(m.runs_instanced, k.runs_instanced);
        m.draws_instanced = std::max(m.draws_instanced, k.draws_instanced);
        for (u32 d = 0; d < kDrawCauseCount; ++d) {
            m.draw_causes[d] = std::max(m.draw_causes[d], k.draw_causes[d]);
            sum.draw_causes[d] += k.draw_causes[d];
        }
        m.merged_groups = std::max(m.merged_groups, k.merged_groups);
        m.merged_children = std::max(m.merged_children, k.merged_children);
        m.decals_dropped = std::max(m.decals_dropped, k.decals_dropped);
        sum.draws += k.draws;
        sum.passes += k.passes;
        sum.dispatches += k.dispatches;
        sum.triangles += k.triangles;
        sum.meshes += k.meshes;
        sum.sprites += k.sprites;
        sum.sprite_batches += k.sprite_batches;
        sum.ring_overflows += k.ring_overflows;
        sum.upload_bytes += k.upload_bytes;
        sum.heap_allocations += k.heap_allocations;
        sum.runs_instanced += k.runs_instanced;
        sum.draws_instanced += k.draws_instanced;
        sum.merged_groups += k.merged_groups;
        sum.merged_children += k.merged_children;
        sum.decals_dropped += k.decals_dropped;
    }
    const u64 n = c.frames.size();
    Counters& avg = s.counters_avg;
    avg.draws = static_cast<u32>(sum.draws / n);
    avg.passes = static_cast<u32>(sum.passes / n);
    avg.dispatches = static_cast<u32>(sum.dispatches / n);
    avg.triangles = static_cast<u32>(sum.triangles / n);
    avg.meshes = static_cast<u32>(sum.meshes / n);
    avg.sprites = static_cast<u32>(sum.sprites / n);
    avg.sprite_batches = static_cast<u32>(sum.sprite_batches / n);
    avg.ring_overflows = static_cast<u32>(sum.ring_overflows / n);
    avg.upload_bytes = sum.upload_bytes / n;
    avg.heap_allocations = sum.heap_allocations / n;
    avg.runs_instanced = static_cast<u32>(sum.runs_instanced / n);
    avg.draws_instanced = static_cast<u32>(sum.draws_instanced / n);
    avg.merged_groups = static_cast<u32>(sum.merged_groups / n);
    avg.merged_children = static_cast<u32>(sum.merged_children / n);
    avg.decals_dropped = static_cast<u32>(sum.decals_dropped / n);
    for (u32 k = 0; k < kDrawCauseCount; ++k) avg.draw_causes[k] = static_cast<u32>(sum.draw_causes[k] / n);

    // Spikes: frames at least twice the median frame time and 1 ms over it.
    if (s.frame.count == 0) return s;
    const f32 limit = std::max(s.frame.p50 * 2.0f, s.frame.p50 + 1.0f);
    Array<usize> hits;
    for (usize i = 0; i < c.frames.size(); ++i)
        if (c.frames[i].frame_ms >= limit) hits.push_back(i);
    std::sort(hits.begin(), hits.end(), [&](usize a, usize b) {
        return c.frames[a].frame_ms != c.frames[b].frame_ms ? c.frames[a].frame_ms > c.frames[b].frame_ms : a < b;
    });
    if (hits.size() > kMaxSpikes) hits.resize(kMaxSpikes);

    const f32 med_sim = median_of(c, get_sim, 0), med_render = median_of(c, get_render, 0),
              med_gpu = median_of(c, get_gpu, 0);
    for (usize i : hits) {
        const Frame& f = c.frames[i];
        Spike sp;
        sp.frame = f.index;
        sp.tick = f.tick;
        sp.frame_ms = f.frame_ms;
        // The part that grew the most over its median; within it, the phase or stage that did.
        Column best{"wait", {}, 0.0f, 0.0f};
        f32 best_growth = 0.5f;  // under half a millisecond of growth anywhere: the frame waited
        const auto consider = [&](const char* group, f32 v, f32 med) {
            if (v < 0.0f || med < 0.0f) return;
            if (v - med > best_growth) {
                best_growth = v - med;
                best = {group, {}, v, med};
            }
        };
        consider("sim", f.sim_ms, med_sim);
        consider("render", f.render_ms, med_render);
        consider("gpu", f.gpu_ms, med_gpu);
        sp.cause.assign(StringView(best.group));
        f32 detail_growth = 0.0f;
        StringView detail;
        const bool sim = StringView(best.group) == "sim";
        if (sim || StringView(best.group) == "render") {
            for (u32 p = 0; p < c.phases.size() && p < kMaxPhases; ++p) {
                if (((c.render_phases >> p) & 1u) == (sim ? 1u : 0u)) continue;  // the group's own phases
                const f32 g = f.phase_ms[p] - median_of(c, get_phase, p);
                if (g > detail_growth) {
                    detail_growth = g;
                    detail = c.phases[p].view();
                }
            }
        } else if (StringView(best.group) == "gpu") {
            for (u32 g = 0; g < c.gpu_stages.size() && g < kMaxGpuStages; ++g) {
                const f32 med = median_of(c, get_stage, g);
                if (f.gpu_stage_ms[g] < 0.0f || med < 0.0f) continue;
                if (f.gpu_stage_ms[g] - med > detail_growth) {
                    detail_growth = f.gpu_stage_ms[g] - med;
                    detail = c.gpu_stages[g].view();
                }
            }
        }
        if (!detail.empty()) {
            sp.cause.append(": ");
            sp.cause.append(detail);
        }
        s.spikes.push_back(sp);
    }
    return s;
}

void write_mcperf(const Capture& c, const Summary& s, Array<char>& out) {
    Out o(out);
    o.fmt("{\n  \"format\": \"mce-perf\",\n  \"version\": %u,\n  \"app\": ", kFormatVersion);
    o.str(c.app.view());
    o.raw(",\n  \"backend\": ");
    o.str(c.backend.view());
    o.raw(",\n  \"scene\": ");
    o.str(c.scene.view());
    o.fmt(",\n  \"width\": %u,\n  \"height\": %u,\n  \"sim_hz\": %u,\n  \"phases\": ", c.width, c.height, c.sim_hz);
    write_names(o, c.phases);
    o.raw(",\n  \"render_phases\": [");
    bool first = true;
    for (u32 p = 0; p < c.phases.size() && p < kMaxPhases; ++p)
        if ((c.render_phases >> p) & 1u) {
            o.raw(first ? "" : ", ");
            o.str(c.phases[p].view());
            first = false;
        }
    o.raw("]");
    o.raw(",\n  \"gpu_stages\": ");
    write_names(o, c.gpu_stages);
    o.raw(",\n  \"draw_causes\": [");  // the names of Counters::draw_causes, in order
    for (u32 k = 0; k < kDrawCauseCount; ++k) {
        if (k) o.raw(", ");
        o.str(StringView(draw_cause_name(static_cast<DrawCause>(k))));
    }
    o.raw("]");
    o.raw(",\n  \"draw_cause_hints\": [");  // what to change for each, so a reader needs no table of its own
    for (u32 k = 0; k < kDrawCauseCount; ++k) {
        if (k) o.raw(", ");
        o.str(StringView(draw_cause_hint(static_cast<DrawCause>(k))));
    }
    o.raw("]");

    o.fmt(",\n  \"summary\": {\n    \"frames\": %u,\n    ", s.frames);
    write_stat(o, "frame_ms", s.frame);
    o.raw(",\n    ");
    write_stat(o, "sim_ms", s.sim);
    o.raw(",\n    ");
    write_stat(o, "render_ms", s.render);
    o.raw(",\n    ");
    write_stat(o, "gpu_ms", s.gpu);
    o.raw(",\n    \"counters_max\": ");
    write_counters(o, s.counters_max);
    o.raw(",\n    \"counters_avg\": ");
    write_counters(o, s.counters_avg);
    o.raw(",\n    \"spikes\": [");
    for (usize i = 0; i < s.spikes.size(); ++i) {
        const Spike& sp = s.spikes[i];
        o.fmt("%s\n      {\"frame\": %llu, \"tick\": %llu, \"frame_ms\": ", i ? "," : "",
              static_cast<unsigned long long>(sp.frame), static_cast<unsigned long long>(sp.tick));
        o.ms(sp.frame_ms);
        o.raw(", \"cause\": ");
        o.str(sp.cause.view());
        o.raw("}");
    }
    o.raw(s.spikes.empty() ? "]\n  },\n" : "\n    ]\n  },\n");

    o.raw("  \"systems\": [");
    for (usize i = 0; i < c.systems.size(); ++i) {
        const SystemStats& sy = c.systems[i];
        o.raw(i ? ",\n    {\"name\": " : "\n    {\"name\": ");
        o.str(sy.name.view());
        o.raw(", \"phase\": ");
        o.str(sy.phase < c.phases.size() ? c.phases[sy.phase].view() : StringView("?"));
        o.fmt(", \"runs\": %llu, \"total_ms\": ", static_cast<unsigned long long>(sy.runs));
        o.ms(static_cast<f32>(sy.total_ms));
        o.raw(", \"max_frame_ms\": ");
        o.ms(sy.max_frame_ms);
        o.raw("}");
    }
    o.raw(c.systems.empty() ? "],\n" : "\n  ],\n");

    // Frames: one line each, in the order of the header's names.
    const usize phases = std::min<usize>(c.phases.size(), kMaxPhases);
    const usize stages = std::min<usize>(c.gpu_stages.size(), kMaxGpuStages);
    o.raw("  \"frames\": [");
    for (usize i = 0; i < c.frames.size(); ++i) {
        const Frame& f = c.frames[i];
        o.fmt("%s\n    {\"index\": %llu, \"tick\": %llu, \"ticks\": %u, \"frame_ms\": ", i ? "," : "",
              static_cast<unsigned long long>(f.index), static_cast<unsigned long long>(f.tick), f.ticks);
        o.ms(f.frame_ms);
        o.raw(", \"sim_ms\": ");
        o.ms(f.sim_ms);
        o.raw(", \"render_ms\": ");
        o.ms(f.render_ms);
        o.raw(", \"gpu_ms\": ");
        o.ms(f.gpu_ms);
        o.raw(", \"phase_ms\": [");
        for (usize p = 0; p < phases; ++p) {
            if (p) o.raw(", ");
            o.ms(f.phase_ms[p]);
        }
        o.raw("], \"gpu_stage_ms\": [");
        for (usize g = 0; g < stages; ++g) {
            if (g) o.raw(", ");
            o.ms(f.gpu_stage_ms[g]);
        }
        o.raw("], \"counters\": ");
        write_counters(o, f.counters);
        o.raw("}");
    }
    o.raw(c.frames.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

ResultV save_mcperf(const Capture& c, const char* path) {
    const Summary s = summarize(c);
    Array<char> text;
    write_mcperf(c, s, text);
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return Error(Errc::IoError, "perf.capture.write", StringView(path));
    const bool ok_write = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool ok_close = std::fclose(f) == 0;
    if (!ok_write || !ok_close) return Error(Errc::IoError, "perf.capture.write", StringView(path));
    return ok();
}

}  // namespace mce::perf
