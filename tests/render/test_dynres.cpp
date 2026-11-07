// M-DYNRES 46b: the pure dynamic resolution controller (cost model, quantize, step, resolve, the injection schedule) and how
// the quality tiers and the cvars map onto PostFx::dynres. No GPU.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "mce/convar.h"
#include "mce/render/dynres.h"
#include "mce/render/forward3d.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr f32 kV = kDynResVariableShare;
constexpr u32 kLatency = 3;  // MCE_FRAMES_IN_FLIGHT: a sample is this many frames old

ConVar& cvar(const char* name) {
    ConVar* cv = ConVar::find(StringId(name));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCvars {
    ~ResetCvars() {
        cvar("r.dynres").reset();
        cvar("r.dynres.target_ms").reset();
        cvar("r.dynres.inject").reset();
    }
};

u64 fnv(const std::vector<f32>& v) {
    u64 h = 0xcbf29ce484222325ull;
    for (f32 f : v) {
        u32 bits;
        static_assert(sizeof(bits) == sizeof(f));
        std::memcpy(&bits, &f, sizeof(bits));
        for (int b = 0; b < 4; ++b) {
            h ^= (bits >> (8 * b)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

bool on_grid(f32 s, f32 ceiling) {
    if (s == ceiling) return true;
    const f32 k = s * 16.0f;
    return k == std::floor(k);
}

DynResParams params(f32 target, f32 floor, f32 ceiling) {
    DynResParams p;
    p.target_ms = target;
    p.floor = floor;
    p.ceiling = ceiling;
    return p;
}

// Frames of an injected schedule through the controller with the real latency: the sample of frame k (its cost at the scale
// frame k drew at) reaches the controller at frame k + kLatency.
std::vector<f32> run_injected(const DynResParams& p, const char* schedule, u32 frames) {
    DynResInject inj;
    REQUIRE(dynres_parse_inject(schedule, inj));
    DynResState st;
    st.scale = p.ceiling;
    std::vector<f32> scales;
    std::vector<f32> ms_ring(frames, 0.0f);
    for (u32 k = 0; k < frames; ++k) {
        const bool has = k >= kLatency;
        f32 ms = 0.0f;
        f32 sc = 1.0f;
        if (has) {
            sc = scales[k - kLatency];
            ms = dynres_ms_at(dynres_inject_ms(inj, k - kLatency), sc, kV);
        }
        scales.push_back(dynres_step(p, st, has, ms, sc));
    }
    return scales;
}

u32 reversals(const std::vector<f32>& s) {
    u32 n = 0;
    int dir = 0;
    for (usize i = 1; i < s.size(); ++i) {
        if (s[i] == s[i - 1]) continue;
        const int d = s[i] > s[i - 1] ? 1 : -1;
        if (dir != 0 && d != dir) ++n;
        dir = d;
    }
    return n;
}

}  // namespace

TEST_CASE("dynres cost model: round trip, scale_for fits the target and is monotone in the cost") {
    for (f32 c : {4.0f, 10.0f, 22.0f, 40.0f})
        for (f32 s : {0.5f, 0.625f, 0.8f, 1.0f}) {
            const f32 ms = dynres_ms_at(c, s, kV);
            CHECK(dynres_cost_full(ms, s, kV) == doctest::Approx(c).epsilon(1e-5));
        }
    CHECK(dynres_ms_at(20.0f, 1.0f, kV) == doctest::Approx(20.0f));
    CHECK(dynres_ms_at(20.0f, 0.0f, kV) == doctest::Approx(6.0f));
    CHECK(dynres_scale_for(0.0f, 14.0f, kV) == 1.0f);
    CHECK(dynres_scale_for(-1.0f, 14.0f, kV) == 1.0f);
    CHECK(dynres_scale_for(10.0f, 14.0f, kV) == 1.0f);  // fits at full scale
    CHECK(dynres_scale_for(100.0f, 14.0f, kV) == 0.0f);   // the fixed part alone is over: the floor decides
    f32 prev = 2.0f;
    for (f32 c = 8.0f; c <= 60.0f; c += 1.0f) {
        const f32 s = dynres_scale_for(c, 14.0f, kV);
        CHECK(s <= prev);
        prev = s;
        if (s > 0.0f && s < 1.0f) CHECK(dynres_ms_at(c, s, kV) == doctest::Approx(14.0f).epsilon(1e-4));
        CHECK(dynres_ms_at(c, s, kV) <= 14.0f * 1.0001f + (s == 0.0f ? 100.0f : 0.0f));
    }
    CHECK(dynres_scale_for(kNaN, 14.0f, kV) == 1.0f);
}

TEST_CASE("dynres_quantize: the grid, the exact ceiling, the floor snapped up, a floor above the ceiling") {
    CHECK(dynres_quantize(1.0f, 0.5f, 1.0f) == 1.0f);
    CHECK(dynres_quantize(2.0f, 0.5f, 1.0f) == 1.0f);
    CHECK(dynres_quantize(0.99f, 0.5f, 1.0f) == 0.9375f);
    CHECK(dynres_quantize(0.9375f, 0.5f, 1.0f) == 0.9375f);
    CHECK(dynres_quantize(0.80f, 0.5f, 1.0f) == 0.75f);
    CHECK(dynres_quantize(0.5f, 0.5f, 1.0f) == 0.5f);
    CHECK(dynres_quantize(0.3f, 0.5f, 1.0f) == 0.5f);
    // an off-grid ceiling is returned bit for bit, and the grid below it rounds down
    CHECK(dynres_quantize(0.67f, 0.5f, 0.67f) == 0.67f);
    CHECK(dynres_quantize(0.9f, 0.5f, 0.67f) == 0.67f);
    CHECK(dynres_quantize(0.66f, 0.5f, 0.67f) == 0.625f);
    CHECK(dynres_quantize(0.85f, 0.5f, 0.85f) == 0.85f);
    CHECK(dynres_quantize(0.84f, 0.5f, 0.85f) == 0.8125f);
    // the floor is snapped up to the grid
    CHECK(dynres_quantize(0.1f, 0.6f, 1.0f) == 0.625f);
    CHECK(dynres_quantize(0.62f, 0.6f, 1.0f) == 0.625f);
    CHECK(dynres_quantize(0.65f, 0.65f, 1.0f) == 0.6875f);
    // a floor above the ceiling (or one the grid snaps onto it) gives the ceiling
    CHECK(dynres_quantize(0.7f, 0.9f, 0.8f) == 0.8f);
    CHECK(dynres_quantize(0.3f, 0.66f, 0.67f) == 0.67f);
    CHECK(dynres_quantize(kNaN, 0.5f, 1.0f) == 1.0f);
    // sweep: on the grid or the ceiling, inside the range, never above the input, monotone
    for (f32 ceiling : {1.0f, 0.85f, 0.67f}) {
        f32 prev = 0.0f;
        for (f32 s = 0.0f; s <= 1.2f; s += 0.001f) {
            const f32 q = dynres_quantize(s, 0.55f, ceiling);
            CHECK(on_grid(q, ceiling));
            CHECK(q >= 0.5625f);
            CHECK(q <= ceiling);
            if (s >= 0.5625f) CHECK(q <= s + 1e-6f);
            CHECK(q >= prev);
            prev = q;
        }
    }
}

TEST_CASE("dynres_step: over budget drops at once and by at most 4 steps; a cooldown holds the next drop") {
    DynResParams p = params(14.0f, 0.5f, 1.0f);
    DynResState st;
    // c = 40 ms at scale 1 asks for far less than 4 steps allow
    f32 s = dynres_step(p, st, true, 40.0f, 1.0f);
    CHECK(s == 0.75f);  // 4 grid steps: 0.9375, 0.875, 0.8125, 0.75
    CHECK(st.cooldown == p.down_cooldown);
    // the next frames still see the overrun (the same cost at the new scale) but the cooldown holds
    for (u32 i = 0; i < p.down_cooldown - 1; ++i) {
        s = dynres_step(p, st, true, dynres_ms_at(40.0f, st.scale, kV), st.scale);
        CHECK(s == 0.75f);
    }
    s = dynres_step(p, st, true, dynres_ms_at(40.0f, st.scale, kV), st.scale);
    CHECK(s < 0.75f);  // the cooldown ran out: the second drop
    // a mild overrun drops by what fits, not the full 4 steps
    DynResState st2;
    s = dynres_step(p, st2, true, 15.0f, 1.0f);
    CHECK(s == 0.9375f);
    CHECK(dynres_ms_at(15.0f, s, kV) <= 14.0f);
}

TEST_CASE("dynres_step: the headroom band holds, recovery is one step per 60 frames, bounds respected") {
    DynResParams p = params(14.0f, 0.5f, 1.0f);
    DynResState st;
    st.scale = 0.75f;
    // c = 13: fits at 1.0 (13 <= 14) but the estimate at the next step up (0.8125: 10.7?) must be <= 11.9
    // at 0.8125: 13 * (0.3 + 0.7 * 0.66) = 10.0 -> rises
    f32 s = dynres_step(p, st, true, dynres_ms_at(13.0f, 0.75f, kV), 0.75f);
    CHECK(s == 0.8125f);
    CHECK(st.cooldown == p.up_cooldown);
    u32 frames_to_next = 0;
    while (st.scale == 0.8125f && frames_to_next < 200) {
        ++frames_to_next;
        dynres_step(p, st, true, dynres_ms_at(13.0f, st.scale, kV), st.scale);
    }
    CHECK(frames_to_next == p.up_cooldown);
    // never above the ceiling
    for (u32 i = 0; i < 2000; ++i) dynres_step(p, st, true, dynres_ms_at(5.0f, st.scale, kV), st.scale);
    CHECK(st.scale == 1.0f);
    // the hysteresis band: a cost that fits at the scale but not with headroom at the next step holds forever
    DynResState hold;
    hold.scale = 0.875f;
    // at 0.9375: c * (0.3 + 0.7 * 0.8789) = c * 0.9152; c = 14 / 0.9152 * 0.99 ~ 15.1 fits (13.8 <= 14) but > 11.9
    const f32 c_band = 15.0f;
    CHECK(dynres_ms_at(c_band, 0.875f, kV) < 14.0f);
    CHECK(dynres_ms_at(c_band, 0.9375f, kV) > 14.0f * 0.85f);
    for (u32 i = 0; i < 500; ++i) dynres_step(p, hold, true, dynres_ms_at(c_band, hold.scale, kV), hold.scale);
    CHECK(hold.scale == 0.875f);
    // never below the floor
    DynResParams pf = params(14.0f, 0.65f, 1.0f);
    DynResState low;
    for (u32 i = 0; i < 500; ++i) dynres_step(pf, low, true, dynres_ms_at(200.0f, low.scale, kV), low.scale);
    CHECK(low.scale == 0.6875f);  // the floor 0.65 snapped up to the grid
}

TEST_CASE("dynres_step: a one-frame spike is clamped, NaN and no sample hold") {
    DynResParams p = params(14.0f, 0.5f, 1.0f);
    DynResState st;
    for (u32 i = 0; i < 20; ++i) dynres_step(p, st, true, 10.0f, 1.0f);
    CHECK(st.scale == 1.0f);
    const f32 before = st.cost;
    dynres_step(p, st, true, 500.0f, 1.0f);  // a hitch
    CHECK(st.cost <= before * (1.0f + p.ema * (p.spike - 1.0f)) * 1.0001f);
    CHECK(st.scale == 1.0f);  // 20 ms smoothed would still be over 14, but the clamp keeps it at 12.5: no drop
    // NaN, inf, negative and bad scales are rejected samples
    DynResState h;
    h.scale = 0.75f;
    h.cost = 12.0f;
    h.samples = 5;
    for (f32 bad : {kNaN, kInf, -1.0f}) {
        dynres_step(p, h, true, bad, 0.75f);
        CHECK(h.scale == 0.75f);
        CHECK(h.cost == 12.0f);
        CHECK(h.samples == 5);
    }
    dynres_step(p, h, true, 100.0f, kNaN);
    dynres_step(p, h, true, 100.0f, 0.0f);
    dynres_step(p, h, true, 100.0f, 1.5f);
    dynres_step(p, h, false, 100.0f, 0.75f);
    CHECK(h.scale == 0.75f);
    CHECK(h.samples == 5);
    // no sample: the cooldown counts down to 0 and stays there
    h.cooldown = 2;
    dynres_step(p, h, false, 0.0f, 1.0f);
    dynres_step(p, h, false, 0.0f, 1.0f);
    dynres_step(p, h, false, 0.0f, 1.0f);
    CHECK(h.cooldown == 0);
    // a state beyond the ceiling or below the floor is pulled in
    DynResState out;
    out.scale = 1.0f;
    DynResParams tier = params(14.0f, 0.5f, 0.67f);
    CHECK(dynres_step(tier, out, false, 0.0f, 1.0f) == 0.67f);
    // never over budget: the ceiling comes back bit for bit
    DynResState never;
    never.scale = tier.ceiling;
    for (u32 i = 0; i < 300; ++i) CHECK(dynres_step(tier, never, true, 6.0f, never.scale) == 0.67f);
}

TEST_CASE("dynres injected profile 22@0,9@200: golden scale sequence, hash, determinism, no thrash") {
    const DynResParams p = params(14.0f, 0.6f, 1.0f);
    const std::vector<f32> a = run_injected(p, "22@0,9@200", 600);
    const std::vector<f32> b = run_injected(p, "22@0,9@200", 600);
    CHECK(a == b);
    CHECK(fnv(a) == fnv(b));
    // the change points (frame, scale after it)
    std::vector<std::pair<u32, f32>> changes;
    for (u32 k = 1; k < a.size(); ++k)
        if (a[k] != a[k - 1]) changes.emplace_back(k, a[k]);
    const std::vector<std::pair<u32, f32>> golden = {
        {3, 0.75f}, {11, 0.6875f}, {204, 0.75f}, {264, 0.8125f}, {324, 0.875f}, {384, 0.9375f}, {444, 1.0f}};
    CHECK(changes == golden);
    CHECK(fnv(a) == 0x8574d237519ea835ull);
    CHECK(a[0] == 1.0f);
    CHECK(a[199] < 1.0f);
    CHECK(a.back() == 1.0f);
    CHECK(reversals(a) <= 2);
    for (u32 k = 0; k < a.size(); ++k) {
        CHECK(on_grid(a[k], 1.0f));
        CHECK(a[k] >= 0.625f);
        CHECK(a[k] <= 1.0f);
    }
    // never more often than the cooldowns allow: drops 8 frames apart, rises 60 frames apart
    for (usize i = 1; i < changes.size(); ++i) {
        const bool up = changes[i].second > (i >= 1 ? changes[i - 1].second : 1.0f);
        const u32 gap = changes[i].first - changes[i - 1].first;
        CHECK(gap >= (up ? 60u : 8u));
    }
    // another schedule gives another sequence
    CHECK(run_injected(p, "9@0", 100) != run_injected(p, "22@0", 100));
    // a 9 ms cost never leaves the ceiling
    for (f32 s : run_injected(p, "9@0", 300)) CHECK(s == 1.0f);
}

TEST_CASE("dynres sweep: any constant cost settles inside the bounds and fits the budget when it can") {
    for (f32 cost = 4.0f; cost <= 60.0f; cost += 2.0f) {
        for (f32 ceiling : {1.0f, 0.85f, 0.67f}) {
            const DynResParams p = params(14.0f, 0.55f, ceiling);
            char sched[32];
            std::snprintf(sched, sizeof(sched), "%.1f@0", cost);
            const std::vector<f32> a = run_injected(p, sched, 900);
            const f32 end = a.back();
            CHECK(on_grid(end, ceiling));
            CHECK(end <= ceiling);
            CHECK(end >= 0.5625f);
            if (dynres_ms_at(cost, 0.5625f, kV) <= 14.0f) CHECK(dynres_ms_at(cost, end, kV) <= 14.0f * 1.001f);
            // it settles: nothing changes over the last 200 frames
            for (usize k = 700; k < a.size(); ++k) CHECK(a[k] == end);
            CHECK(reversals(a) <= 2);
        }
    }
}

TEST_CASE("dynres_parse_inject: good, malformed, unordered, over 8 entries") {
    DynResInject in;
    CHECK(dynres_parse_inject("22@0,9@200", in));
    CHECK(in.count == 2);
    CHECK(in.ms[0] == 22.0f);
    CHECK(in.frame[0] == 0);
    CHECK(in.ms[1] == 9.0f);
    CHECK(in.frame[1] == 200);
    CHECK(dynres_inject_ms(in, 0) == 22.0f);
    CHECK(dynres_inject_ms(in, 199) == 22.0f);
    CHECK(dynres_inject_ms(in, 200) == 9.0f);
    CHECK(dynres_inject_ms(in, 100000) == 9.0f);
    CHECK(dynres_parse_inject(" 24.5 @ 10 , 8@240 ", in));
    CHECK(in.count == 2);
    CHECK(in.ms[0] == 24.5f);
    CHECK(dynres_inject_ms(in, 3) == 24.5f);  // before the first entry: the first entry
    CHECK(dynres_parse_inject("5@7", in));
    CHECK(in.count == 1);
    for (const char* bad : {"", "   ", "abc", "22", "22@", "@5", "22@0,", ",22@0", "22@0,,9@1", "22@0;9@1", "-5@0", "22@-1",
                            "0@0", "2000@0", "22@0,9@0", "22@5,9@2", "22@0 9@1", "1.2.3@0", "22@1.5", "22@0x"}) {
        CAPTURE(bad);
        in.count = 3;
        CHECK_FALSE(dynres_parse_inject(bad, in));
        CHECK(in.count == 0);
        CHECK(dynres_inject_ms(in, 5) == 0.0f);
    }
    std::string eight, nine;
    for (u32 i = 0; i < 9; ++i) {
        std::string e = std::to_string(10 + i) + "@" + std::to_string(i * 10);
        if (i < 8) eight += (i ? "," : "") + e;
        nine += (i ? "," : "") + e;
    }
    CHECK(dynres_parse_inject(eight, in));
    CHECK(in.count == 8);
    CHECK_FALSE(dynres_parse_inject(nine, in));
}

TEST_CASE("dynres_resolve: the off cases, the clamps, the cvar target wins") {
    DynamicResolution d;
    d.enabled = true;
    DynResParams p;
    CHECK(dynres_resolve(d, true, 0.0f, 0.5f, 1.0f, p));
    CHECK(p.target_ms == 14.0f);
    CHECK(p.floor == 0.6f);
    CHECK(p.ceiling == 1.0f);
    CHECK(p.headroom == 0.15f);
    CHECK(p.max_down_steps == 4);
    d.enabled = false;
    CHECK_FALSE(dynres_resolve(d, true, 0.0f, 0.5f, 1.0f, p));
    d.enabled = true;
    CHECK_FALSE(dynres_resolve(d, false, 0.0f, 0.5f, 1.0f, p));
    CHECK_FALSE(dynres_resolve(d, true, kNaN, 0.5f, 1.0f, p));
    CHECK_FALSE(dynres_resolve(d, true, kInf, 0.5f, 1.0f, p));
    CHECK_FALSE(dynres_resolve(d, true, 0.0f, kNaN, 1.0f, p));
    CHECK_FALSE(dynres_resolve(d, true, 0.0f, 0.5f, kNaN, p));
    DynamicResolution bad = d;
    bad.target_ms = kNaN;
    CHECK_FALSE(dynres_resolve(bad, true, 0.0f, 0.5f, 1.0f, p));
    bad = d;
    bad.min_scale = kInf;
    CHECK_FALSE(dynres_resolve(bad, true, 0.0f, 0.5f, 1.0f, p));
    // the cvar target wins when > 0, and is clamped
    CHECK(dynres_resolve(d, true, 8.0f, 0.5f, 1.0f, p));
    CHECK(p.target_ms == 8.0f);
    CHECK(dynres_resolve(d, true, 1.0f, 0.5f, 1.0f, p));
    CHECK(p.target_ms == 4.0f);
    CHECK(dynres_resolve(d, true, 500.0f, 0.5f, 1.0f, p));
    CHECK(p.target_ms == 100.0f);
    CHECK(dynres_resolve(d, true, -3.0f, 0.5f, 1.0f, p));
    CHECK(p.target_ms == 14.0f);
    // floor = max(setting, tier, 0.5); ceiling = min(setting, tier), at least 0.5
    CHECK(dynres_resolve(d, true, 0.0f, 0.65f, 1.0f, p));
    CHECK(p.floor == 0.65f);
    d.min_scale = 0.3f;
    CHECK(dynres_resolve(d, true, 0.0f, 0.2f, 1.0f, p));
    CHECK(p.floor == 0.5f);
    d.min_scale = 0.6f;
    d.max_scale = 0.9f;
    CHECK(dynres_resolve(d, true, 0.0f, 0.5f, 0.85f, p));
    CHECK(p.ceiling == 0.85f);
    CHECK(dynres_resolve(d, true, 0.0f, 0.5f, 1.0f, p));
    CHECK(p.ceiling == 0.9f);
    CHECK(dynres_resolve(d, true, 0.0f, 0.5f, 0.2f, p));
    CHECK(p.ceiling == 0.5f);
    d.headroom = 0.9f;
    CHECK(dynres_resolve(d, true, 0.0f, 0.5f, 1.0f, p));
    CHECK(p.headroom == 0.5f);
}

TEST_CASE("dynres quality tiers, cvars and apply_quality clamps") {
    ResetCvars reset;
    CHECK(quality_settings(QualityTier::Low).dynres);
    CHECK(quality_settings(QualityTier::Medium).dynres);
    CHECK(quality_settings(QualityTier::High).dynres);
    CHECK(quality_settings(QualityTier::Low).dynres_floor == 0.5f);
    CHECK(quality_settings(QualityTier::Medium).dynres_floor == 0.5f);
    CHECK(quality_settings(QualityTier::High).dynres_floor == 0.65f);
    for (QualityTier t : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        Forward3DDesc desc;
        apply_quality(quality_settings(t), desc);
        CHECK(desc.dynres_floor == quality_settings(t).dynres_floor);
        CHECK(desc.dynres_floor <= desc.render_scale);
    }
    CHECK(cvar("r.dynres").get_bool());
    CHECK(cvar("r.dynres.target_ms").get_float() == 0.0f);
    CHECK(cvar("r.dynres.inject").get_string().empty());

    ShadowSettings sh;
    PostFx fx;
    apply_quality(quality_settings(QualityTier::High), sh, fx);
    CHECK_FALSE(fx.dynres.enabled);  // off stays off
    fx.dynres.enabled = true;
    apply_quality(quality_settings(QualityTier::High), sh, fx);
    CHECK(fx.dynres.enabled);
    cvar("r.dynres").set_string("0");
    apply_quality(quality_settings(QualityTier::High), sh, fx);
    CHECK_FALSE(fx.dynres.enabled);
    cvar("r.dynres").set_string("1");
    QualitySettings q = quality_settings(QualityTier::Medium);
    q.dynres = false;
    fx.dynres.enabled = true;
    apply_quality(q, sh, fx);
    CHECK_FALSE(fx.dynres.enabled);

    PostFx c;
    c.dynres.target_ms = 1000.0f;
    c.dynres.min_scale = 0.1f;
    c.dynres.max_scale = 0.3f;
    c.dynres.headroom = 9.0f;
    apply_quality(quality_settings(QualityTier::High), sh, c);
    CHECK(c.dynres.target_ms == 100.0f);
    CHECK(c.dynres.min_scale == 0.5f);
    CHECK(c.dynres.max_scale == 0.5f);
    CHECK(c.dynres.headroom == 0.5f);
    PostFx n;
    n.dynres.target_ms = kNaN;
    n.dynres.min_scale = kNaN;
    n.dynres.max_scale = kNaN;
    n.dynres.headroom = kNaN;
    apply_quality(quality_settings(QualityTier::High), sh, n);
    CHECK(n.dynres.target_ms == 14.0f);
    CHECK(n.dynres.min_scale == 0.6f);
    CHECK(n.dynres.max_scale == 1.0f);
    CHECK(n.dynres.headroom == 0.15f);
    PostFx lo;
    lo.dynres.target_ms = 0.1f;
    lo.dynres.min_scale = 0.9f;
    lo.dynres.max_scale = 0.7f;
    apply_quality(quality_settings(QualityTier::High), sh, lo);
    CHECK(lo.dynres.target_ms == 4.0f);
    CHECK(lo.dynres.max_scale == 0.9f);  // never below min_scale
}
