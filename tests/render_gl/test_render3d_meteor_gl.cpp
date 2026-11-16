// M-METEOR 18c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the meteors in the sky
// pass (p_meteor / p_meteor2, meteor_layer in MCE_GLSL_SKY_LAYERS) and in the water's reflection. Level 0, `enabled`
// false, `rate` 0, `brightness` 0, a NaN level and the Low tier draw the frame without them bit for bit; the streaks
// change only the sky above the horizon and only around the slot's active streaks (at most 3); the head is brighter
// than the tail and the tail fades to 0; each streak lies on the great circle kit::meteor_make gives, its head and
// tail where kit::meteor_head and kit::meteor_tail put them at probe times, and its light is the kit's envelope times
// the shader's glow; a higher brightness is brighter and a longer length longer; a cloud deck in front dims them and a
// dense fog hides them; the sea reflects them; a floating-origin shift leaves the sky unchanged; the clock's 60 s
// period repeats the frame bit for bit; a slow camera turn keeps the streaks' light and the frame difference per step
// bounded (no crawl); at a low render scale a streak is never thinner than a pixel and has no gaps. The scene is the
// meteor sky (fixture3d.h). One frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "fixture3d.h"
#include "mce/kit/meteor.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

constexpr f64 kDeg = 180.0 / 3.14159265358979323846;
constexpr f64 kGain = 1.0;                    // shaders3d.h kMeteorGain
constexpr f64 kColor[3] = {1.0, 0.95, 0.85};  // shaders3d.h kMeteorColor
constexpr f64 kMinWidth = 0.15 / kDeg;        // shaders3d.h kMeteorWidth: 0.15 degrees, radians

f64 luma(const Array<u8>& px, usize i) { return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]; }

// The view ray through each pixel's center (top row first), from the last frame's view (as kSkyFS reconstructs it).
Array<vec3> ray_dirs(const Fixture3D& f) {
    const math::mat4 inv = math::inverse(f.view.view_proj);
    Array<vec3> r;
    r.reserve(usize(kW) * kH);
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const f32 nx = (f32(x) + 0.5f) / f32(kW) * 2.0f - 1.0f, ny = 1.0f - (f32(y) + 0.5f) / f32(kH) * 2.0f;
            const math::vec4 w = inv * math::vec4(nx, ny, 1.0f, 1.0f);
            r.push_back(math::normalize(vec3{w.x / w.w, w.y / w.w, w.z / w.w} - f.view.position));
        }
    return r;
}

// The pixel a direction lands on (x, y top row first), false outside the frame or behind the camera.
bool pixel_of(const Fixture3D& f, vec3 d, i32& x, i32& y) {
    const math::vec4 c = f.view.view_proj * math::vec4(d.x, d.y, d.z, 0.0f);
    if (c.w <= 0.0f) return false;
    x = i32(std::floor((c.x / c.w * 0.5f + 0.5f) * f32(kW)));
    y = i32(std::floor((0.5f - c.y / c.w * 0.5f) * f32(kH)));
    return x >= 0 && y >= 0 && x < i32(kW) && y < i32(kH);
}

template <class T>
bool same(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize i) {
    return a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2];
}

// The linear radiance behind an 8-bit sRGB value after the default tonemap (ACES fit on 0.6 x, exposure 1, which the
// meteor scene uses).
f64 linear_of(u8 v) {
    const f64 e = f64(v) / 255.0;
    const f64 y = e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
    f64 lo = 0.0, hi = 64.0;  // aces() is monotone: bisect
    for (int i = 0; i < 60; ++i) {
        const f64 x = 0.5 * (lo + hi);
        const f64 a = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
        (a < y ? lo : hi) = x;
    }
    return 0.5 * (lo + hi) / 0.6;
}

// The linear light the meteors add to pixel p, per channel (never negative).
vec3 added(const Array<u8>& px, const Array<u8>& bare, usize p) {
    const auto ch = [&](usize c) { return f32(std::max(0.0, linear_of(px[p * 4 + c]) - linear_of(bare[p * 4 + c]))); };
    return {ch(0), ch(1), ch(2)};
}

// The 8-bit luma the meteors add to pixel p (never negative).
f64 added_luma(const Array<u8>& px, const Array<u8>& bare, usize p) {
    return std::max(0.0, luma(px, p * 4) - luma(bare, p * 4));
}

// The linear luminance the meteors add to pixel p (the tonemap inverted; the light the shader adds).
f64 added_linear(const Array<u8>& px, const Array<u8>& bare, usize p) {
    const vec3 e = added(px, bare, p);
    return 0.2126 * e.x + 0.7152 * e.y + 0.0722 * e.z;
}

// Total linear light the meteors add over the frame without them, over the pixels whose ray climbs in
// (min_up, max_up] (linear, not 8-bit luma: on the dark night sky the sRGB curve would inflate a dimmed streak).
f64 flux(const Array<u8>& px, const Array<u8>& bare, const Array<vec3>& dirs, f32 min_up = -2.0f, f32 max_up = 2.0f) {
    f64 sum = 0.0;
    for (usize p = 0; p < dirs.size(); ++p)
        if (dirs[p].y > min_up && dirs[p].y <= max_up) sum += added_linear(px, bare, p);
    return sum;
}

u32 changed(const Array<u8>& px, const Array<u8>& bare) {
    u32 n = 0;
    for (usize p = 0; p < px.size() / 4; ++p) n += same_px(px, bare, p * 4) ? 0u : 1u;
    return n;
}

// No bloom (a glow would spill across the horizon and around the heads: not what is measured).
void no_bloom(Scene3D& s) {
    PostFx post = s.post();
    post.bloom = false;
    s.set_post(post);
}

Array<u8> draw_sky(Fixture3D& f, const MeteorSky* meteor, const std::function<void(Scene3D&)>& more = {}) {
    f.render(0.0f, true, [&](Scene3D& s) {
        meteor_sky_scene(s, meteor ? *meteor : MeteorSky{});  // MeteorSky{}: level 0
        if (more) more(s);
        no_bloom(s);
    });
    return f.pixels;
}

Array<u8> draw_sea(Fixture3D& f, const MeteorSky* meteor) {
    f.render(0.0f, true, [&](Scene3D& s) {
        meteor_water_scene(s, meteor ? *meteor : MeteorSky{}, water_carrier(0.05f));
        no_bloom(s);
    });
    return f.pixels;
}

// The kit's Meteor the carrier `m` was made from (the fields the streaks read).
kit::Meteor kit_of(const MeteorSky& m) {
    kit::Meteor k;
    k.rate = m.rate;
    k.brightness = m.brightness;
    k.length = m.length;
    k.speed = m.speed;
    k.seed = m.seed;
    return k;
}

// One of the slot's candidates as the kit sees it at the carrier's clock.
struct KitStreak {
    kit::MeteorStreak s;
    f32 age = 0.0f;
    f64 env = 0.0;  // kit::meteor_envelope: 0 for a candidate that is not in flight (or not active)
    vec3 head{}, tail{}, axis{}, fwd{};
};
struct KitSlot {
    u32 slot = 0, count = 0;
    KitStreak k[kit::kMeteorMaxPerSlot];
};
KitSlot kit_streaks(const MeteorSky& m) {
    const kit::Meteor km = kit_of(m);
    const kit::MeteorSlot sl = kit::meteor_slot(m.time);
    KitSlot out;
    out.slot = sl.slot;
    out.count = kit::meteor_slot_count(km, sl.slot);
    const f32 ts = sl.frac * kit::kMeteorSlotSeconds;
    for (u32 i = 0; i < kit::kMeteorMaxPerSlot; ++i) {
        KitStreak& k = out.k[i];
        k.s = kit::meteor_make(km, sl.slot, i);
        k.age = ts - k.s.start_time;
        k.env = kit::meteor_envelope(k.s, k.age);
        k.head = kit::meteor_head(k.s, k.age);
        k.tail = kit::meteor_tail(k.s, k.age);
        k.axis = math::normalize(math::cross(k.s.start, k.s.travel));
        k.fwd = math::cross(k.axis, k.head);  // the travel direction at the head
    }
    return out;
}

// The shader's glow geometry for d against streak k (the gnomonic projection at the head): u along the segment
// toward the tail, v across it, both in tangents; `len` the tail's tangent. False when d is farther from the head than
// the streak reaches (the shader's early out) for width w.
struct Glow {
    f64 u = 0.0, v = 0.0, len = 0.0, along = 0.0, dist = 0.0;
};
bool glow_of(const KitStreak& k, vec3 d, f64 w, Glow& g) {
    const f64 ch = math::dot(d, k.head);
    if (ch < std::cos(f64(k.s.tail_angle) + 4.0 * w)) return false;
    g.len = std::tan(f64(k.s.tail_angle));
    g.u = -f64(math::dot(d, k.fwd)) / ch;
    g.v = f64(math::dot(d, k.axis)) / ch;
    g.along = std::clamp(g.u, 0.0, g.len);
    g.dist = std::sqrt((g.u - g.along) * (g.u - g.along) + g.v * g.v);
    return true;
}

// The linear light (per unit colour) streak k adds along d with pixel footprint pix: exp2(-(dist / w)^2) times the
// tail fade times the envelope, w = max(0.15 degrees, pix).
f64 kit_light(const KitStreak& k, vec3 d, f64 pix) {
    if (!(k.env > 0.0)) return 0.0;
    const f64 w = std::max(kMinWidth, pix);
    Glow g;
    if (!glow_of(k, d, w, g)) return 0.0;
    const f64 q = g.dist / w;
    return std::exp2(-q * q) * (1.0 - g.along / g.len) * k.env;
}

// The radians per pixel along dirs[p], as the sky pass takes it (the neighbouring pixel's ray).
f64 pix_at(const Array<vec3>& dirs, usize p) {
    const usize q = (p % kW) + 1 < kW ? p + 1 : p - 1;
    return f64(math::length(dirs[q] - dirs[p]));
}

f64 haze(vec3 d) {
    const f64 t = std::clamp(f64(d.y) / 0.25, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// The most linear light added in the 3 x 3 pixels around direction d (-1 when d is outside the frame).
f64 peak_near(const Fixture3D& f, const Array<u8>& px, const Array<u8>& bare, vec3 d) {
    i32 x = 0, y = 0;
    if (!pixel_of(f, d, x, y)) return -1.0;
    f64 best = 0.0;
    for (i32 dy = -1; dy <= 1; ++dy)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= i32(kW) || yy >= i32(kH)) continue;
            best = std::max(best, added_linear(px, bare, usize(yy) * kW + usize(xx)));
        }
    return best;
}

// The point on streak k's great circle `behind` radians behind the head (negative: ahead of it).
vec3 on_arc(const KitStreak& k, f64 behind) {
    return kit::detail::meteor_arc(k.s, k.s.angular_speed * kit::detail::meteor_age(k.s, k.age) - f32(behind));
}

// Which in-flight streaks the changed pixels belong to, and how many changed pixels no in-flight streak reaches.
struct Attribution {
    u32 below = 0, stray = 0, lit[kit::kMeteorMaxPerSlot] = {};
    u32 streaks() const {
        u32 n = 0;
        for (u32 c : lit) n += c > 0 ? 1u : 0u;
        return n;
    }
};
Attribution attribute(const Array<u8>& px, const Array<u8>& bare, const Array<vec3>& dirs, const KitSlot& ks) {
    Attribution a;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (same_px(px, bare, p * 4)) continue;
        if (dirs[p].y <= 0.0f) {
            ++a.below;
            continue;
        }
        const f64 w = std::max(kMinWidth, pix_at(dirs, p));
        i32 best = -1;
        f64 best_dist = 1e30;
        for (u32 i = 0; i < ks.count; ++i) {
            Glow g;
            if (!(ks.k[i].env > 0.0) || !glow_of(ks.k[i], dirs[p], w, g)) continue;
            if (g.dist < best_dist) {
                best_dist = g.dist;
                best = i32(i);
            }
        }
        if (best < 0)
            ++a.stray;
        else
            ++a.lit[best];
    }
    return a;
}

}  // namespace

TEST_CASE(
    "render3d_gl: meteors at level 0, disabled, at rate 0 or brightness 0, with a NaN level or on the Low tier "
    "are the plain frame") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_sky(f, nullptr);
    MeteorSky zero = meteor_carrier(0.0f);
    CHECK(same(draw_sky(f, &zero), none));
    MeteorSky off = meteor_carrier(2.0f);
    off.enabled = false;
    CHECK(same(draw_sky(f, &off), none));
    MeteorSky no_rate = meteor_carrier(2.0f);
    no_rate.rate = 0.0f;
    CHECK(same(draw_sky(f, &no_rate), none));
    MeteorSky dark = meteor_carrier(2.0f);
    dark.brightness = 0.0f;
    CHECK(same(draw_sky(f, &dark), none));
    MeteorSky nan = meteor_carrier(std::nanf(""));  // the upload clamps again: a NaN level is off
    CHECK(same(draw_sky(f, &nan), none));
    const MeteorSky low = apply_quality(quality_settings(QualityTier::Low), meteor_carrier(2.0f));
    CHECK(low.level == 0.0f);
    CHECK(same(draw_sky(f, &low), none));
    const MeteorSky high = apply_quality(quality_settings(QualityTier::High), meteor_carrier(1.0f));
    CHECK(high.level == 1.0f);
    CHECK_FALSE(same(draw_sky(f, &high), none));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE(
    "render3d_gl: the streaks change only the sky above the horizon, around at most 3 in-flight streaks of the "
    "slot") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_sky(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    // The test frame: all three of slot 8's candidates in flight in view.
    {
        const MeteorSky m = meteor_carrier(1.0f);
        const KitSlot ks = kit_streaks(m);
        const Attribution a = attribute(draw_sky(f, &m), none, dirs, ks);
        MESSAGE("slot " << ks.slot << ": " << ks.count << " candidates; changed pixels per streak " << a.lit[0] << " / "
                        << a.lit[1] << " / " << a.lit[2] << ", out of every streak's reach " << a.stray
                        << ", at or below the horizon " << a.below);
        CHECK(ks.count == 3);
        CHECK(a.streaks() == 3);
        CHECK(a.below == 0);
        CHECK(a.stray == 0);
        for (u32 c : a.lit) CHECK(c > 40);
    }
    // Every slot of the period at four clocks across the start window: never more than 3 streaks, never more than the
    // kit's count, nothing outside an in-flight streak's reach.
    u32 frames = 0, lit_frames = 0, most = 0, stray = 0, below = 0, over = 0;
    for (u32 slot = 0; slot < kit::kMeteorSlotsPerPeriod; ++slot)
        for (f32 frac : {0.12f, 0.27f, 0.41f, 0.55f}) {
            const MeteorSky m = meteor_carrier(1.0f, (f32(slot) + frac) * kit::kMeteorSlotSeconds);
            const KitSlot ks = kit_streaks(m);
            const Attribution a = attribute(draw_sky(f, &m), none, dirs, ks);
            ++frames;
            lit_frames += a.streaks() > 0 ? 1u : 0u;
            most = std::max(most, a.streaks());
            stray += a.stray;
            below += a.below;
            over += a.streaks() > ks.count ? 1u : 0u;
        }
    MESSAGE(frames << " frames over the period: " << lit_frames << " with a streak in view, at most " << most
                   << " streaks in one frame; stray pixels " << stray << ", below the horizon " << below);
    CHECK(most <= kit::kMeteorMaxPerSlot);
    CHECK(over == 0);
    CHECK(stray == 0);
    CHECK(below == 0);
    CHECK(lit_frames > 4);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a streak's head is brighter than its tail, and the tail fades to 0") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_sky(f, nullptr);
    const MeteorSky m = meteor_carrier(1.0f);
    const KitSlot ks = kit_streaks(m);
    const Array<u8> px = draw_sky(f, &m);
    constexpr f64 kPix = 0.375 / kDeg;  // about the test frame's pixel
    for (u32 i = 0; i < ks.count; ++i) {
        const KitStreak& k = ks.k[i];
        const f64 l = f64(k.s.tail_angle);
        const f64 at_head = peak_near(f, px, none, on_arc(k, 0.05 * l));
        const f64 at_third = peak_near(f, px, none, on_arc(k, 0.35 * l));
        const f64 at_two_thirds = peak_near(f, px, none, on_arc(k, 0.65 * l));
        const f64 at_tail = peak_near(f, px, none, on_arc(k, 0.95 * l));
        const f64 past_tail = peak_near(f, px, none, on_arc(k, l + 4.0 * kPix));
        MESSAGE("streak " << i << ": added light along it, head to tail " << at_head << " / " << at_third << " / "
                          << at_two_thirds << " / " << at_tail << ", 4 pixels past the tail " << past_tail);
        REQUIRE(at_tail >= 0.0);
        REQUIRE(past_tail >= 0.0);
        CHECK(at_head > at_third);
        CHECK(at_third > at_two_thirds);
        CHECK(at_two_thirds > at_tail);
        CHECK(at_tail < 0.2 * at_head);
        CHECK(past_tail < 0.002 * at_head);
    }
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE(
    "render3d_gl: the drawn streaks follow kit::meteor_make, kit::meteor_head, kit::meteor_tail and "
    "kit::meteor_envelope") {
    // At three clocks across the test slot: per sky pixel the linear light the streaks add (the tonemap inverted) must
    // be level * brightness * kMeteorGain * kMeteorColor * the horizon fade * the sum of the kit streaks' glow (the
    // shader's distance and fade on the kit's head, tail and envelope); the brightest pixel of each streak sits at
    // the kit's head and its lit pixels on the kit's great circle, ending at the kit's tail.
    Fixture3D f;
    cloud_sky_camera(f);
    const auto no_fxaa = [](Scene3D& s) {  // FXAA would blend a line one pixel wide with its neighbours
        PostFx post = s.post();
        post.fxaa = false;
        s.set_post(post);
    };
    const Array<u8> none = draw_sky(f, nullptr, no_fxaa);
    const Array<vec3> dirs = ray_dirs(f);
    u32 n = 0, ok = 0, heads = 0, heads_ok = 0, tails_ok = 0, off_circle = 0, lit_total = 0;
    f64 worst = 0.0, worst_head = 0.0, worst_tail = 0.0, worst_circle = 0.0;
    for (f32 dt : {-0.12f, 0.0f, 0.1f}) {
        const MeteorSky m = meteor_carrier(1.5f, kMeteorSkyTime + dt);
        const KitSlot ks = kit_streaks(m);
        const Array<u8> px = draw_sky(f, &m, no_fxaa);
        f64 best[kit::kMeteorMaxPerSlot] = {}, far[kit::kMeteorMaxPerSlot] = {};
        vec3 best_d[kit::kMeteorMaxPerSlot] = {};
        for (usize p = 0; p < dirs.size(); ++p) {
            const vec3 d = dirs[p];
            if (d.y <= 0.0f || p % kW + 1 >= kW) continue;
            const f64 pix = pix_at(dirs, p);
            f64 sum = 0.0;
            for (u32 i = 0; i < ks.count; ++i) sum += kit_light(ks.k[i], d, pix);
            const f64 scale = f64(m.level) * f64(m.brightness) * kGain * haze(d);
            const vec3 got = added(px, none, p);
            if (sum * scale > 1e-3 || got.x > 0.0f) {
                ++n;
                f64 err = 0.0, want_max = 0.0;
                const f32 g[3] = {got.x, got.y, got.z};
                for (int c = 0; c < 3; ++c) {
                    const f64 want = sum * scale * kColor[c];
                    err = std::max(err, std::fabs(f64(g[c]) - want));
                    want_max = std::max(want_max, want);
                }
                worst = std::max(worst, err);
                ok += err < 0.02 + 0.08 * want_max ? 1u : 0u;
            }
            // Per streak: the brightest pixel, and over the clearly lit ones the distance from the great circle and
            // how far behind the head they reach.
            const f64 lum = added_linear(px, none, p);
            if (lum < 0.05) continue;
            const f64 w = std::max(kMinWidth, pix);
            for (u32 i = 0; i < ks.count; ++i) {
                Glow gl;
                if (!(ks.k[i].env > 0.0) || !glow_of(ks.k[i], d, w, gl) || gl.dist > 4.0 * w) continue;
                ++lit_total;
                const f64 circle = std::asin(std::fabs(f64(math::dot(d, ks.k[i].axis))));
                worst_circle = std::max(worst_circle, circle);
                off_circle += circle > 2.5 * w ? 1u : 0u;
                if (lum > best[i]) {
                    best[i] = lum;
                    best_d[i] = d;
                }
                far[i] = std::max(far[i], std::atan(gl.along));  // radians behind the head, on the circle
            }
        }
        for (u32 i = 0; i < ks.count; ++i) {
            if (!(ks.k[i].env > 0.0) || best[i] <= 0.0) continue;
            ++heads;
            const f64 head_err = std::acos(std::clamp(f64(math::dot(best_d[i], ks.k[i].head)), -1.0, 1.0)) * kDeg;
            const f64 tail_err = (f64(ks.k[i].s.tail_angle) - far[i]) * kDeg;
            // The tail end as a direction: the kit's meteor_tail is the arc point tail_angle behind the head.
            const f64 tail_gap =
                std::acos(
                    std::clamp(f64(math::dot(on_arc(ks.k[i], f64(ks.k[i].s.tail_angle)), ks.k[i].tail)), -1.0, 1.0)) *
                kDeg;
            worst_head = std::max(worst_head, head_err);
            worst_tail = std::max(worst_tail, std::fabs(tail_err));
            heads_ok += head_err < 1.0 ? 1u : 0u;
            tails_ok += (tail_err > -0.5 && tail_err < 1.5 && tail_gap < 0.05) ? 1u : 0u;  // degrees
        }
    }
    MESSAGE("light within tolerance in " << ok << " of " << n << " probe pixels (worst " << worst << "); heads within "
                                         << "1 degree of kit::meteor_head " << heads_ok << " of " << heads << " (worst "
                                         << worst_head << " degrees); lit ends within reach of " << "kit::meteor_tail "
                                         << tails_ok << " (worst " << worst_tail << " degrees short); " << off_circle
                                         << " of " << lit_total << " lit pixels off the great circle (worst "
                                         << worst_circle * kDeg << " degrees)");
    REQUIRE(n > 300);
    CHECK(f64(ok) >= 0.97 * f64(n));
    CHECK(heads >= 7);
    CHECK(heads_ok == heads);
    CHECK(tails_ok == heads);
    CHECK(off_circle == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a higher meteor brightness is brighter, a longer length longer") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_sky(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    MeteorSky m = meteor_carrier(1.0f);
    const Array<u8> base = draw_sky(f, &m);
    const f64 f1 = flux(base, none, dirs);
    m.brightness = 2.0f;
    const f64 f2 = flux(draw_sky(f, &m), none, dirs);
    m.brightness = 0.5f;
    const f64 f05 = flux(draw_sky(f, &m), none, dirs);
    m.brightness = 1.0f;
    m.length = 2.0f;
    const Array<u8> longer = draw_sky(f, &m);
    m.length = 0.5f;
    const Array<u8> shorter = draw_sky(f, &m);
    // How far behind the head a streak is lit (luma above 2), the longest of the slot's streaks, degrees.
    const auto reach = [&](const Array<u8>& px, const MeteorSky& c) {
        const KitSlot ks = kit_streaks(c);
        f64 most = 0.0;
        for (usize p = 0; p < dirs.size(); ++p) {
            if (dirs[p].y <= 0.0f || added_luma(px, none, p) < 2.0) continue;
            const f64 w = std::max(kMinWidth, pix_at(dirs, p));
            for (u32 i = 0; i < ks.count; ++i) {
                Glow g;
                if (glow_of(ks.k[i], dirs[p], w, g) && g.dist < 3.0 * w)
                    most = std::max(most, std::atan(g.along) * kDeg);
            }
        }
        return most;
    };
    MeteorSky c1 = meteor_carrier(1.0f), c2 = c1, c05 = c1;
    c2.length = 2.0f;
    c05.length = 0.5f;
    const f64 r1 = reach(base, c1), r2 = reach(longer, c2), r05 = reach(shorter, c05);
    const u32 n1 = changed(base, none), n2 = changed(longer, none), n05 = changed(shorter, none);
    MESSAGE("linear flux at brightness 0.5 / 1 / 2: "
            << f05 << " / " << f1 << " / " << f2 << "; lit reach behind the head at " << "length 0.5 / 1 / 2: " << r05
            << " / " << r1 << " / " << r2 << " degrees, changed pixels " << n05 << " / " << n1 << " / " << n2);
    REQUIRE(f1 > 0.0);
    CHECK(f2 > 1.5 * f1);
    CHECK(f05 < 0.7 * f1);
    CHECK(r2 > 1.5 * r1);
    CHECK(r1 > 1.5 * r05);
    CHECK(n2 > n1);
    CHECK(n1 > n05);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a cloud deck in front dims the streaks and a dense fog hides them") {
    Fixture3D f;
    cloud_sky_camera(f);
    const MeteorSky m = meteor_carrier(2.0f);
    const Array<u8> none = draw_sky(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    // Over the rays that climb more than 0.25, above the band where the deck fades into the horizon haze (every head
    // of the test frame is above it).
    const f64 clear = flux(draw_sky(f, &m), none, dirs, 0.25f);
    Clouds thin, deck;
    thin.coverage = deck.coverage = 1.0f;
    deck.density = 1.0f;
    const auto broken = [&](Scene3D& s) { s.set_clouds(thin); };
    const auto overcast = [&](Scene3D& s) { s.set_clouds(deck); };
    const f64 f_thin = flux(draw_sky(f, &m, broken), draw_sky(f, nullptr, broken), dirs, 0.25f);
    const f64 f_deck = flux(draw_sky(f, &m, overcast), draw_sky(f, nullptr, overcast), dirs, 0.25f);
    const auto dense = [](Scene3D& s) {
        Volumetrics v;
        v.enabled = true;
        v.density = 0.2f;
        v.height_falloff = 0.0f;
        s.set_volumetrics(v);
    };
    const f64 f_fog = flux(draw_sky(f, &m, dense), draw_sky(f, nullptr, dense), dirs);
    const f64 clear_all = flux(draw_sky(f, &m), none, dirs);
    MESSAGE("meteor flux above 0.25: clear " << clear << ", thin deck " << f_thin << ", thick deck " << f_deck
                                             << "; whole sky: clear " << clear_all << ", dense fog " << f_fog);
    REQUIRE(clear > 0.0);
    CHECK(f_thin < 0.6 * clear);
    CHECK(f_deck < 0.2 * clear);
    CHECK(f_fog < 0.05 * clear_all);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the sea reflects the streaks, and the sea without them is unchanged") {
    Fixture3D f;
    water_camera(f);
    const Array<u8> none = draw_sea(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    MeteorSky zero = meteor_carrier(0.0f, kMeteorWaterTime, kMeteorWaterSeed);
    CHECK(same(draw_sea(f, &zero), none));
    MeteorSky off = meteor_carrier(2.0f, kMeteorWaterTime, kMeteorWaterSeed);
    off.enabled = false;
    CHECK(same(draw_sea(f, &off), none));
    const MeteorSky low =
        apply_quality(quality_settings(QualityTier::Low), meteor_carrier(2.0f, kMeteorWaterTime, kMeteorWaterSeed));
    CHECK(same(draw_sea(f, &low), none));
    const MeteorSky m = meteor_carrier(2.0f, kMeteorWaterTime, kMeteorWaterSeed);
    const Array<u8> lit = draw_sea(f, &m);
    u32 sky = 0, sea = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (same_px(lit, none, p * 4)) continue;
        (dirs[p].y > 0.0f ? sky : sea) += 1u;
    }
    const f64 sea_flux = flux(lit, none, dirs, -2.0f, -0.02f);
    MESSAGE("pixels the streak changes: sky " << sky << ", sea " << sea << " (sea flux " << sea_flux << ")");
    CHECK(sky > 20);
    CHECK(sea > 20);
    CHECK(sea_flux > 0.0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the streaks stay put across an origin shift, and the 60 s period repeats them bit for bit") {
    Fixture3D f;
    cloud_sky_camera(f);
    const MeteorSky m = meteor_carrier(1.0f);
    const Array<u8> home = draw_sky(f, &m);
    // A floating-origin rebase: the layer is a function of the view direction alone.
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    CHECK(same(draw_sky(f, &m, [&](Scene3D& s) { s.set_origin_shift(shift); }), home));
    // The clock as the extract wraps it (kit::meteor_wrap_time): a period later and ten periods later.
    const f64 t = f64(kMeteorSkyTime);
    MeteorSky later = m, much_later = m, moved = m;
    later.time = kit::meteor_wrap_time(t + 60.0);
    much_later.time = kit::meteor_wrap_time(t + 600.0);
    moved.time = kit::meteor_wrap_time(t + 0.05);
    MESSAGE("wrapped clocks: " << m.time << ", +60 s " << later.time << ", +600 s " << much_later.time);
    CHECK(same(draw_sky(f, &later), home));
    CHECK(same(draw_sky(f, &much_later), home));
    CHECK_FALSE(same(draw_sky(f, &moved), home));  // the streaks do move with the clock
    CHECK(f.device->validation_error_count() == 0);
}

namespace {

// A one-degree camera turn in tenth-degree steps: the streaks' total light (lo .. hi over the steps) and the worst
// mean change of the luma they add per sky pixel between neighbouring steps, against the mean they add per lit pixel.
struct Crawl {
    f64 lo = 1e30, hi = 0.0, worst_step = 0.0, mean_lit = 0.0;
    f64 peak_lo = 1e30, peak_hi = 0.0;  // the brightest pixel's added luma over the steps
};
Crawl turn(Fixture3D& f, const MeteorSky& m) {
    Crawl c;
    Array<f64> prev;
    for (u32 i = 0; i <= 10; ++i) {
        f.yaw_deg = 0.1f * f32(i);
        const Array<u8> bare = draw_sky(f, nullptr);
        const Array<vec3> dirs = ray_dirs(f);
        const Array<u8> px = draw_sky(f, &m);
        Array<f64> add;  // the luma the streaks add per pixel (0 at or below the horizon)
        add.reserve(dirs.size());
        f64 fl = 0.0, peak = 0.0;
        u32 lit = 0;
        for (usize p = 0; p < dirs.size(); ++p) {
            const f64 v = dirs[p].y > 0.0f ? luma(px, p * 4) - luma(bare, p * 4) : 0.0;
            add.push_back(v);
            fl += std::max(v, 0.0);
            peak = std::max(peak, v);
            lit += v > 1.0 ? 1u : 0u;
        }
        c.lo = std::min(c.lo, fl);
        c.hi = std::max(c.hi, fl);
        c.peak_lo = std::min(c.peak_lo, peak);
        c.peak_hi = std::max(c.peak_hi, peak);
        if (i == 0) c.mean_lit = fl / f64(std::max(lit, 1u));
        if (!prev.empty()) {
            f64 sum = 0.0;
            u32 n = 0;
            for (usize p = 0; p < dirs.size(); ++p) {
                if (dirs[p].y <= 0.0f) continue;
                sum += std::fabs(add[p] - prev[p]);
                ++n;
            }
            c.worst_step = std::max(c.worst_step, sum / f64(std::max(n, 1u)));
        }
        prev = std::move(add);
    }
    f.yaw_deg = 0.0f;
    return c;
}

}  // namespace

TEST_CASE("render3d_gl: the streaks keep their light under a slow camera turn (no crawl)") {
    // The streaks are 0.15 degrees wide at their narrowest (a 0.3 degree full width at half maximum), under the test
    // frame's 0.375 degree pixels and, at render scale 0.5, its 0.75 degree drawn pixels: without the footprint floor
    // they would be point-sampled, not filtered, and the light of a pixel on the line, and of the brightest one, would
    // pop between the steps of a turn as the line slides between the pixel centres. The streaks' total and peak light
    // must stay put, and the light they add to a pixel may move only as far as a sub-pixel shift of a smooth line
    // moves it. The default length and the longest at full scale, the default at half scale.
    MeteorSky longest = meteor_carrier(1.0f);
    longest.length = 2.0f;
    for (f32 scale : {1.0f, 0.5f}) {
        Fixture3D f(4, true, true, scale);
        cloud_sky_camera(f);
        for (const MeteorSky& m : {meteor_carrier(1.0f), longest}) {
            if (scale < 1.0f && m.length > 1.0f) continue;
            const Crawl c = turn(f, m);
            MESSAGE("render scale " << scale << ", length " << m.length << ": meteor flux over a one-degree turn "
                                    << c.lo << " .. " << c.hi << ", brightest pixel " << c.peak_lo << " .. "
                                    << c.peak_hi << "; worst mean change of the added luma per sky pixel and step "
                                    << c.worst_step << " (mean added luma per lit pixel " << c.mean_lit << ")");
            REQUIRE(c.lo > 0.0);
            REQUIRE(c.peak_lo > 0.0);
            // Measured with the floor: flux within 0.8% and the peak within 6% (full scale), 0.4% and 2.3% (half
            // scale); without it (width 0.15 degrees whatever the pixel) half scale gave 6.9% and 25%.
            CHECK(c.hi / c.lo < 1.03);
            CHECK(c.peak_hi / c.peak_lo < 1.1);
            CHECK(c.worst_step < 0.01 * c.mean_lit);
        }
        CHECK(f.device->validation_error_count() == 0);
    }
}

TEST_CASE("render3d_gl: at a low render scale a streak is never thinner than a pixel and has no gaps") {
    // Render scale 0.5: the sky is drawn at 80 x 60 (0.75 degree pixels) and upsampled. Across each streak (between a
    // fifth and three fifths of the way to the tail) the light's spread (its RMS distance from the line) must be at
    // least three quarters of a drawn pixel, and along it every sample keeps more than half of the light the tail fade
    // leaves the brightest one (a 0.15 degree line sampled by 0.75 degree pixels breaks into dashes). Measured with
    // the footprint floor: spread 1.01 to 1.07 drawn pixels, the dimmest sample 0.71 to 0.82 of the brightest;
    // without it: 0.49 to 0.64 and 0.32 to 0.44.
    Fixture3D f(4, true, true, 0.5f);
    cloud_sky_camera(f);
    const Array<u8> none = draw_sky(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const MeteorSky m = meteor_carrier(1.0f);
    const KitSlot ks = kit_streaks(m);
    const Array<u8> px = draw_sky(f, &m);
    const f64 drawn_pix = 2.0 * pix_at(dirs, usize(kH / 2) * kW + kW / 2);  // radians per drawn pixel
    for (u32 i = 0; i < ks.count; ++i) {
        const KitStreak& k = ks.k[i];
        f64 sw = 0.0, sv2 = 0.0;
        for (usize p = 0; p < dirs.size(); ++p) {
            if (dirs[p].y <= 0.0f) continue;
            Glow g;
            if (!glow_of(k, dirs[p], 4.0 * drawn_pix, g)) continue;
            if (g.u < 0.2 * g.len || g.u > 0.6 * g.len || std::fabs(g.v) > 4.0 * drawn_pix) continue;
            const f64 lum = added_luma(px, none, p);
            sw += lum;
            sv2 += lum * g.v * g.v;
        }
        const f64 spread = sw > 0.0 ? std::sqrt(sv2 / sw) : 0.0;
        // Along the streak in quarter-pixel steps: the peak near each sample over the tail fade there.
        const f64 l = f64(k.s.tail_angle);
        f64 lo = 1e30, hi = 0.0;
        for (f64 b = 0.1 * l; b <= 0.6 * l; b += 0.25 * drawn_pix) {
            const f64 v = peak_near(f, px, none, on_arc(k, b));
            if (v < 0.0) continue;
            const f64 r = v / (1.0 - b / l);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        MESSAGE("streak " << i << ": spread across it " << spread * kDeg << " degrees (drawn pixel " << drawn_pix * kDeg
                          << "); along it the faded peak keeps " << lo << " .. " << hi);
        REQUIRE(sw > 0.0);
        REQUIRE(hi > 0.0);
        CHECK(spread > 0.75 * drawn_pix);
        CHECK(lo > 0.55 * hi);
    }
    CHECK(f.device->validation_error_count() == 0);
}
