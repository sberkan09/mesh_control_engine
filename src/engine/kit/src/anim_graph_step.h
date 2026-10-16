#pragma once
// The AnimGraph (.mcanim) state machine step, shared by the 3D Animator (anim3d.cpp) and the 2D
// Skeleton2DAnimator (skeleton2d_anim.cpp). Templates over the animator component: both carry the
// same runtime fields (state, playing, previous, previous_time, time, blend, blend_clip,
// blend_weight, clip, params, speed, fade, finished, restart, events_fired, last_event). The 3D
// code calls these exactly as it did before they moved here, so its hashes are unchanged.
//
// Clip lengths come from a `duration(u64 clip id)` callable (0 when unknown: the clip loops
// forever and a one-shot never finishes). `crossed(u64 clip, f32 lo, f32 hi, bool closed)` is
// called for every playhead interval the step advances over, after the state's own events, so a
// caller can raise the clip's events too (the 2D clips carry events; 3D passes a no-op).

#include <cmath>

#include "mce/asset/formats.h"
#include "mce/ecs/world.h"
#include "mce/kit/anim3d.h"
#include "mce/math/math.h"
#include "mce/string_id.h"

namespace mce::kit::animgraph {

inline u64 clip_id(const char* name, usize cap) {
    const StringView s = asset::AnimGraphView::text(name, cap);
    return s.empty() ? 0 : StringId(s).value;
}

inline bool holds(const asset::AnimGraphCondition& c, const math::vec4& params) {
    const f32 p = c.param == 0 ? params.x : c.param == 1 ? params.y : c.param == 2 ? params.z : params.w;
    switch (c.op) {
        case asset::AnimCompare::Greater:
            return p > c.value;
        case asset::AnimCompare::Less:
            return p < c.value;
        case asset::AnimCompare::GreaterEqual:
            return p >= c.value;
        case asset::AnimCompare::LessEqual:
            return p <= c.value;
        case asset::AnimCompare::Equal:
            return p == c.value;
        case asset::AnimCompare::NotEqual:
            return p != c.value;
    }
    return false;
}

inline f32 param_of(const math::vec4& params, u32 i) {
    return i == 0 ? params.x : i == 1 ? params.y : i == 2 ? params.z : params.w;
}

// Starts `clip` at time 0, cross-fading from what plays over `fade` seconds.
template <class A>
void start_clip(A& a, u64 clip, f32 fade) {
    a.previous = a.playing;
    a.previous_time = a.time;
    a.playing = clip;
    a.time = 0.0f;
    a.finished = false;
    a.blend = (fade > 0.0f && a.previous != 0 && a.previous != clip) ? 0.0f : 1.0f;
    if (a.blend >= 1.0f) a.previous = 0;
}

// The blend state's two neighbouring clips and the weight of the second at the parameter value.
inline void pick_blend(const asset::AnimGraphView& g, const asset::AnimGraphState& s, const math::vec4& params,
                       u64& first, u64& second, f32& weight) {
    const f32 p = param_of(params, s.blend_param);
    const Span<const asset::AnimGraphBlendPoint> pts = g.blend_points.subspan(s.first_blend, s.blend_count);
    u32 i = 0;
    while (i + 1 < pts.size() && p > pts[i + 1].at) ++i;
    first = clip_id(pts[i].clip, sizeof pts[i].clip);
    if (i + 1 >= pts.size() || p <= pts[i].at) {
        second = 0;
        weight = 0.0f;
        return;
    }
    second = clip_id(pts[i + 1].clip, sizeof pts[i + 1].clip);
    const f32 span = pts[i + 1].at - pts[i].at;
    weight = span > 0.0f ? std::fmin(1.0f, (p - pts[i].at) / span) : 1.0f;
}

// Gradient-band weights (freeform cartesian) of a 2D blend state's points at the parameters:
// a point has full weight on itself and none past any other point; they sum to 1.
inline void blend2d_weights(const asset::AnimGraphView& g, const asset::AnimGraphState& s, const math::vec4& params,
                            f32 (&w)[asset::kAnimGraphMaxBlend2D]) {
    const Span<const asset::AnimGraphBlendPoint2D> pts = g.blend2d.subspan(s.first_blend, s.blend_count);
    const f32 px = param_of(params, s.blend_param), py = param_of(params, s.blend_param_y);
    f32 total = 0.0f;
    for (u32 i = 0; i < pts.size(); ++i) {
        f32 h = 1.0f;
        for (u32 j = 0; j < pts.size(); ++j) {
            const f32 vx = pts[j].x - pts[i].x, vy = pts[j].y - pts[i].y, len2 = vx * vx + vy * vy;
            if (j == i || !(len2 > 0.0f)) continue;
            h = math::min(h, 1.0f - ((px - pts[i].x) * vx + (py - pts[i].y) * vy) / len2);
        }
        w[i] = math::max(h, 0.0f);
        total += w[i];
    }
    if (total > 0.0f) {
        for (u32 i = 0; i < pts.size(); ++i) w[i] /= total;
        return;
    }
    u32 best = 0;  // outside every band (cannot happen with 3+ points, kept for safety): the nearest point
    f32 best_d = 0.0f;
    for (u32 i = 0; i < pts.size(); ++i) {
        const f32 d = (px - pts[i].x) * (px - pts[i].x) + (py - pts[i].y) * (py - pts[i].y);
        if (i == 0 || d < best_d) best = i, best_d = d;
        w[i] = 0.0f;
    }
    w[best] = 1.0f;
}

// A 2D blend state's heaviest clip (the state's timeline) and the next heaviest with its weight.
inline void pick_blend2d(const asset::AnimGraphView& g, const asset::AnimGraphState& s, const math::vec4& params,
                         u64& first, u64& second, f32& weight) {
    f32 w[asset::kAnimGraphMaxBlend2D];
    blend2d_weights(g, s, params, w);
    u32 a = 0, b = ~0u;
    for (u32 i = 1; i < s.blend_count; ++i)
        if (w[i] > w[a]) a = i;
    for (u32 i = 0; i < s.blend_count; ++i)
        if (i != a && w[i] > 0.0f && (b == ~0u || w[i] > w[b])) b = i;
    const asset::AnimGraphBlendPoint2D* pts = g.blend2d.data() + s.first_blend;
    first = clip_id(pts[a].clip, sizeof pts[a].clip);
    second = b == ~0u ? 0 : clip_id(pts[b].clip, sizeof pts[b].clip);
    weight = b == ~0u ? 0.0f : w[b];
}

inline void pick_state_blend(const asset::AnimGraphView& g, const asset::AnimGraphState& s, const math::vec4& params,
                             u64& first, u64& second, f32& weight) {
    if (s.blend_2d)
        pick_blend2d(g, s, params, first, second, weight);
    else
        pick_blend(g, s, params, first, second, weight);
}

template <class A>
void enter_state(A& a, const asset::AnimGraphView& g, u32 index, f32 fade) {
    const asset::AnimGraphState& s = g.states[index];
    a.state = asset::AnimGraphView::text(s.name, sizeof s.name);
    u64 first = clip_id(s.clip, sizeof s.clip), second = 0;
    f32 weight = 0.0f;
    if (s.blend_count > 0) pick_state_blend(g, s, a.params, first, second, weight);
    start_clip(a, first, fade);
    a.blend_clip = second;
    a.blend_weight = weight;
    const StringView name =
        first != 0 && s.blend_count == 0 ? asset::AnimGraphView::text(s.clip, sizeof s.clip) : StringView();
    a.clip = name;  // a blend state leaves `clip` empty: `playing` / `blend_clip` name its clips
}

// One tick of the graph: enters the start state (on `restart` or an unknown state), keeps a blend
// state's pair on the parameter, advances the playhead (wrapping a looping state, holding a
// one-shot just under its end and setting `finished`) while raising the state's events it crosses
// as AnimEvent3D, then takes the first transition whose conditions hold. Returns true when it
// entered a state this tick (the entering tick shows the state at its start).
template <class A, class Duration, class Crossed>
bool run_graph(Frame& f, Entity e, A& a, const asset::AnimGraphView& g, Duration&& duration_of, Crossed&& crossed) {
    if (g.states.empty()) return false;
    u32 cur = g.find_state(a.state.view());
    if (a.restart || cur >= g.states.size()) {
        a.restart = false;
        const u32 start = g.header->start_state < g.states.size() ? g.header->start_state : 0;
        enter_state(a, g, start, cur >= g.states.size() ? 0.0f : a.fade);
        return true;  // the entering tick shows the state at its start
    }
    const asset::AnimGraphState& s = g.states[cur];
    if (s.blend_count > 0) {
        u64 first = 0, second = 0;
        pick_state_blend(g, s, a.params, first, second, a.blend_weight);
        if (first != a.playing) {  // the parameter crossed a point: keep the phase on the new pair
            const f32 from = duration_of(a.playing), to = duration_of(first);
            a.playing = first;
            if (from > 0.0f && to > 0.0f) a.time = a.time / from * to;
        }
        a.blend_clip = second;
    }

    // Advance, firing the state's events the playhead crosses ([t0, t1) per pass).
    const f32 duration = duration_of(a.playing);
    const f32 t0 = a.time;
    f32 t1 = t0 + f.dt * a.speed * s.speed;
    auto fire_between = [&](f32 lo, f32 hi, bool closed) {
        for (u32 i = 0; i < s.event_count; ++i) {
            const asset::AnimGraphEvent& ev = g.events[s.first_event + i];
            if (ev.time >= lo && (ev.time < hi || (closed && ev.time <= hi))) {
                ++a.events_fired;
                a.last_event = asset::AnimGraphView::text(ev.name, sizeof ev.name);
                f.events().push(AnimEvent3D{e, ev.id});
            }
        }
        crossed(a.playing, lo, hi, closed);
    };
    if (!a.finished && t1 > t0) {
        if (duration > 0.0f && !s.loop && t1 >= duration) {
            fire_between(t0, duration, true);
            t1 = std::nextafter(duration, 0.0f);  // poses sample the clip looping: hold just under its end
            a.finished = true;
        } else if (duration > 0.0f && s.loop && t1 >= duration) {
            fire_between(t0, duration, false);
            t1 = std::fmod(t1, duration);
            fire_between(0.0f, t1, false);
        } else {
            fire_between(t0, t1, false);
        }
        a.time = t1;
    }

    // The first transition out of the state (or from any state) whose conditions all hold.
    for (const asset::AnimGraphTransition& tr : g.transitions) {
        if (tr.from != cur && !(tr.from == asset::kAnimAnyState && tr.to != cur)) continue;
        if (tr.on_finish && !a.finished) continue;
        bool ok = true;
        for (u32 i = 0; i < tr.condition_count && ok; ++i) ok = holds(g.conditions[tr.first_condition + i], a.params);
        if (!ok) continue;
        enter_state(a, g, tr.to, tr.fade);
        a.fade = tr.fade;
        return true;
    }
    return false;
}

}  // namespace mce::kit::animgraph
