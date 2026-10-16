#include "mce/perf_budget.h"

namespace mce::perf {

namespace {

constexpr const char* kTargetNames[kTargetCount] = {"desktop", "mobile", "web"};

#define MCE_BUDGET_FIELD(field, is_int)                                                              \
    BudgetField {                                                                                     \
        #field, is_int, [](const Budget& b) { return static_cast<f64>(b.field); },                   \
            [](Budget& b, f64 v) { b.field = static_cast<decltype(Budget::field)>(v); }              \
    }

const BudgetField kFields[] = {
    MCE_BUDGET_FIELD(frame_ms, false),     MCE_BUDGET_FIELD(sim_ms, false),
    MCE_BUDGET_FIELD(render_ms, false),    MCE_BUDGET_FIELD(gpu_ms, false),
    MCE_BUDGET_FIELD(draws, true),         MCE_BUDGET_FIELD(triangles, true),
    MCE_BUDGET_FIELD(sprites, true),       MCE_BUDGET_FIELD(upload_bytes, true),
    MCE_BUDGET_FIELD(heap_allocations, true), MCE_BUDGET_FIELD(sprite_textures, true),
    MCE_BUDGET_FIELD(texture_size, true),
};

#undef MCE_BUDGET_FIELD

}  // namespace

const char* target_name(Target t) {
    const usize i = static_cast<usize>(t);
    return i < kTargetCount ? kTargetNames[i] : "?";
}

bool parse_target(StringView s, Target& out) {
    for (usize i = 0; i < kTargetCount; ++i)
        if (s == kTargetNames[i]) {
            out = static_cast<Target>(i);
            return true;
        }
    return false;
}

// Defaults: 60 fps on desktop and web, 30 on a phone; draw and triangle counts a mid-range phone
// (mobile) and integrated graphics in a browser (web) carry at those rates. Sim time is per frame's
// ticks, so the same 4 ms holds everywhere (a phone's slower CPU is what the margin is for).
Budget default_budget(Target t) {
    Budget b;
    switch (t) {
        case Target::Mobile:
            b.frame_ms = 33.3f;
            b.sim_ms = 4.0f;
            b.render_ms = 8.0f;
            b.gpu_ms = 25.0f;
            b.draws = 400;
            b.triangles = 500'000;
            b.sprites = 10'000;
            b.upload_bytes = 4u << 20;
            b.sprite_textures = 16;
            b.texture_size = 2048;
            break;
        case Target::Web:
            b.frame_ms = 16.7f;
            b.sim_ms = 4.0f;
            b.render_ms = 6.0f;
            b.gpu_ms = 12.0f;
            b.draws = 1000;
            b.triangles = 1'000'000;
            b.sprites = 20'000;
            b.upload_bytes = 8u << 20;
            b.sprite_textures = 32;
            b.texture_size = 4096;
            break;
        case Target::Desktop:
        case Target::Count:
            b.frame_ms = 16.7f;
            b.sim_ms = 4.0f;
            b.render_ms = 6.0f;
            b.gpu_ms = 12.0f;
            b.draws = 3000;
            b.triangles = 3'000'000;
            b.sprites = 50'000;
            b.upload_bytes = 32u << 20;
            b.sprite_textures = 64;
            b.texture_size = 8192;
            break;
    }
    return b;
}

Span<const BudgetField> budget_fields() { return Span<const BudgetField>(kFields, sizeof(kFields) / sizeof(kFields[0])); }

const BudgetField* find_budget_field(StringView name) {
    for (const BudgetField& f : kFields)
        if (name == f.name) return &f;
    return nullptr;
}

Verdict judge(const Summary& s, const Budget& b) {
    Verdict v;
    const auto add = [&](const char* metric, const char* stat, f64 measured, f64 limit) {
        if (limit <= 0) return;
        BudgetLine l;
        l.metric = metric;
        l.stat = stat;
        l.measured = measured;
        l.limit = limit;
        l.over = measured > limit;
        v.pass = v.pass && !l.over;
        v.lines.push_back(l);
    };
    const auto time = [&](const char* metric, const Stat& st, f32 limit) {
        if (st.count) add(metric, "p95", st.p95, limit);
    };
    time("frame_ms", s.frame, b.frame_ms);
    time("sim_ms", s.sim, b.sim_ms);
    time("render_ms", s.render, b.render_ms);
    time("gpu_ms", s.gpu, b.gpu_ms);
    if (s.frames == 0) return v;
    const Counters& m = s.counters_max;
    add("draws", "max", m.draws, b.draws);
    add("triangles", "max", m.triangles, b.triangles);
    add("sprites", "max", m.sprites, b.sprites);
    add("upload_bytes", "max", static_cast<f64>(m.upload_bytes), static_cast<f64>(b.upload_bytes));
    add("heap_allocations", "max", static_cast<f64>(m.heap_allocations), static_cast<f64>(b.heap_allocations));
    return v;
}

}  // namespace mce::perf
