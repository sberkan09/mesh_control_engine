// M-DYNRES 46b: the pure dynamic resolution controller (see dynres.h). No GPU, no clock, no state beyond the arguments.

#include "mce/render/dynres.h"

#include <cmath>

#include "mce/render/scene3d.h"

namespace mce::render {

namespace {

constexpr f32 kGrid = static_cast<f32>(kDynResGrid);

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The lowest grid scale: the floor snapped up to the grid, or the ceiling when that reaches it.
f32 grid_low(f32 floor, f32 ceiling) {
    const f32 lo = std::ceil(floor * kGrid) / kGrid;
    return lo >= ceiling ? ceiling : lo;
}

// The next grid scale above `scale` (the ceiling when the grid ends).
f32 grid_up(f32 scale, f32 ceiling) {
    if (scale >= ceiling) return ceiling;
    const f32 g = (std::floor(scale * kGrid) + 1.0f) / kGrid;
    return g >= ceiling ? ceiling : g;
}

// The next grid scale below `scale`, not under the lowest one.
f32 grid_down(f32 scale, f32 lo) {
    if (scale <= lo) return lo;
    const f32 g = (std::ceil(scale * kGrid) - 1.0f) / kGrid;
    return g < lo ? lo : g;
}

}  // namespace

f32 dynres_cost_full(f32 ms, f32 scale, f32 v) {
    const f32 d = (1.0f - v) + v * scale * scale;
    return d > 0.0f ? ms / d : ms;
}

f32 dynres_ms_at(f32 c, f32 scale, f32 v) { return c * ((1.0f - v) + v * scale * scale); }

f32 dynres_scale_for(f32 c, f32 target_ms, f32 v) {
    if (!(c > 0.0f) || !std::isfinite(c) || !std::isfinite(target_ms) || !(v > 0.0f)) return 1.0f;
    const f32 q = (target_ms / c - (1.0f - v)) / v;
    return clampf(std::sqrt(q > 0.0f ? q : 0.0f), 0.0f, 1.0f);
}

f32 dynres_quantize(f32 scale, f32 floor, f32 ceiling) {
    if (!std::isfinite(ceiling) || !std::isfinite(floor) || !(floor <= ceiling)) return ceiling;
    if (!(scale == scale)) return ceiling;
    const f32 lo = grid_low(floor, ceiling);
    if (scale >= ceiling) return ceiling;
    if (scale <= lo) return lo;
    const f32 g = std::floor(scale * kGrid) / kGrid;
    return g < lo ? lo : g;
}

f32 dynres_step(const DynResParams& p, DynResState& s, bool has_sample, f32 sample_ms, f32 sample_scale) {
    const f32 lo = grid_low(p.floor, p.ceiling);
    s.scale = dynres_quantize(s.scale, p.floor, p.ceiling);
    const bool was_rise = s.cooldown > p.down_cooldown;  // the cooldown a rise sets does not hold a drop back
    if (s.cooldown > 0) --s.cooldown;
    const bool valid = has_sample && std::isfinite(sample_ms) && sample_ms >= 0.0f && std::isfinite(sample_scale) &&
                       sample_scale > 0.0f && sample_scale <= 1.0f;
    if (!valid) return s.scale;

    // (1) the scale independent cost: spike clamp, then the exponential blend (the first sample seeds it).
    f32 c = dynres_cost_full(sample_ms, sample_scale, p.variable_share);
    if (s.samples > 0 && s.cost > 0.0f) {
        const f32 cap = s.cost * p.spike;
        if (c > cap) c = cap;
        s.cost += p.ema * (c - s.cost);
    } else {
        s.cost = c;
    }
    if (s.samples < 0xffffffffu) ++s.samples;

    // (3) over budget: drop to the largest grid scale that fits, at most max_down_steps steps.
    if (dynres_ms_at(s.cost, s.scale, p.variable_share) > p.target_ms) {
        if (s.cooldown == 0 || was_rise) {
            f32 next = dynres_quantize(dynres_scale_for(s.cost, p.target_ms, p.variable_share), p.floor, p.ceiling);
            f32 bound = s.scale;
            for (u32 i = 0; i < p.max_down_steps; ++i) bound = grid_down(bound, lo);
            if (next < bound) next = bound;
            if (next > s.scale) next = s.scale;
            s.scale = next;
            s.cooldown = p.down_cooldown;
        }
        return s.scale;
    }
    // (4) under budget with headroom: one step up after the cooldown.
    if (s.cooldown == 0) {
        const f32 up = grid_up(s.scale, p.ceiling);
        if (up > s.scale && dynres_ms_at(s.cost, up, p.variable_share) <= p.target_ms * (1.0f - p.headroom)) {
            s.scale = up;
            s.cooldown = p.up_cooldown;
        }
    }
    // (5) otherwise the scale holds (the hysteresis band).
    return s.scale;
}

bool dynres_resolve(const DynamicResolution& d, bool cvar_on, f32 cvar_target_ms, f32 tier_floor, f32 tier_ceiling,
                    DynResParams& out) {
    if (!d.enabled || !cvar_on) return false;
    if (!std::isfinite(d.target_ms) || !std::isfinite(d.min_scale) || !std::isfinite(d.max_scale) ||
        !std::isfinite(d.headroom) || !std::isfinite(cvar_target_ms) || !std::isfinite(tier_floor) ||
        !std::isfinite(tier_ceiling))
        return false;
    DynResParams p;
    p.target_ms = clampf(cvar_target_ms > 0.0f ? cvar_target_ms : d.target_ms, 4.0f, 100.0f);
    f32 floor = d.min_scale > tier_floor ? d.min_scale : tier_floor;
    if (floor < 0.5f) floor = 0.5f;
    p.floor = clampf(floor, 0.5f, 1.0f);
    f32 ceiling = d.max_scale < tier_ceiling ? d.max_scale : tier_ceiling;
    p.ceiling = clampf(ceiling, 0.5f, 1.0f);
    p.headroom = clampf(d.headroom, 0.05f, 0.5f);
    out = p;
    return true;
}

bool dynres_parse_inject(StringView text, DynResInject& out) {
    out = DynResInject{};
    DynResInject r;
    usize i = 0;
    const usize n = text.size();
    auto skip_ws = [&] {
        while (i < n && text[i] == ' ') ++i;
    };
    skip_ws();
    if (i >= n) return false;
    while (true) {
        if (r.count >= kDynResMaxInject) return false;
        skip_ws();
        // ms: digits with at most one '.'
        const usize ms_begin = i;
        bool dot = false;
        while (i < n && ((text[i] >= '0' && text[i] <= '9') || (text[i] == '.' && !dot))) {
            if (text[i] == '.') dot = true;
            ++i;
        }
        if (i == ms_begin) return false;
        f64 ms = 0.0;
        f64 scale = 1.0;
        bool frac = false;
        for (usize k = ms_begin; k < i; ++k) {
            if (text[k] == '.') {
                frac = true;
                continue;
            }
            const f64 digit = static_cast<f64>(text[k] - '0');
            if (frac) {
                scale *= 0.1;
                ms += digit * scale;
            } else {
                ms = ms * 10.0 + digit;
            }
            if (ms > 1.0e6) return false;
        }
        skip_ws();
        if (i >= n || text[i] != '@') return false;
        ++i;
        skip_ws();
        const usize f_begin = i;
        u64 frame = 0;
        while (i < n && text[i] >= '0' && text[i] <= '9') {
            if (frame > 100000000000000000ull) return false;
            frame = frame * 10 + static_cast<u64>(text[i] - '0');
            ++i;
        }
        if (i == f_begin) return false;
        if (!(ms > 0.0) || ms > 1000.0) return false;
        if (r.count > 0 && frame <= r.frame[r.count - 1]) return false;
        r.ms[r.count] = static_cast<f32>(ms);
        r.frame[r.count] = frame;
        ++r.count;
        skip_ws();
        if (i >= n) break;
        if (text[i] != ',') return false;
        ++i;
    }
    out = r;
    return true;
}

f32 dynres_inject_ms(const DynResInject& inject, u64 frame) {
    if (inject.count == 0) return 0.0f;
    f32 ms = inject.ms[0];
    for (u32 k = 0; k < inject.count && k < kDynResMaxInject; ++k) {
        if (inject.frame[k] <= frame) ms = inject.ms[k];
        else break;
    }
    return ms;
}

}  // namespace mce::render
