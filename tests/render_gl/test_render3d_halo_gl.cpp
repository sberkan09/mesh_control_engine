// M-HALO 54b against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the ice halo in the sky pass
// (p_halo, halo_term in MCE_GLSL_HALO, added by cirrus_layer) and in the water's reflection (water_sky). Strength 0,
// `enabled` false, a NaN strength, the Low tier, `r.halo 0` and cirrus coverage 0 draw the frame without the halo bit
// for bit; the halo changes only the sky above the horizon; the ring's radial peak sits at 22 degrees from the sun, its
// inner edge is steeper than its outer one, red inside and blue outside with dispersion 1 and neutral with 0; the dogs
// sit on the sun's horizon line 22 degrees from it, farther apart in azimuth and dimmer at a higher sun; there is no
// halo in a gap between the streaks and the low layer in front hides it; a sun under the horizon draws none; the moon's
// ring is fainter than the sun's and absent by day; the sea reflects it and the sea without it is unchanged; an origin
// shift leaves the sky unchanged; a sub-pixel yaw moves the halo by a bounded amount at 160 x 120 and 640 x 360 (the
// crawl test); and the halo's GLSL alone agrees with the CPU twin (mce/render/halo.h) at probe angles. The scenes are
// the halo day and night (fixture3d.h).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

#include "fixture3d.h"
#include "mce/render/cirrus.h"
#include "mce/render/cloud_shadow.h"
#include "mce/render/halo.h"
#include "mce/render/quality.h"
#include "shaders3d.h"  // render's 3D shaders: the halo's text runs alone

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

constexpr f32 kDeg = 0.017453292f;

// The view ray through each pixel's center (top row first) of a w x h frame, from view `v`.
Array<vec3> ray_dirs(const View3D& v, u32 w = kW, u32 h = kH) {
    const math::mat4 inv = math::inverse(v.view_proj);
    Array<vec3> r;
    r.reserve(usize(w) * h);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const f32 nx = (f32(x) + 0.5f) / f32(w) * 2.0f - 1.0f, ny = 1.0f - (f32(y) + 0.5f) / f32(h) * 2.0f;
            const math::vec4 q = inv * math::vec4(nx, ny, 1.0f, 1.0f);
            r.push_back(math::normalize(vec3{q.x / q.w, q.y / q.w, q.z / q.w} - v.position));
        }
    return r;
}

bool same(const Array<u8>& a, const Array<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize i) {
    return a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2];
}

// The linear radiance behind an 8-bit sRGB value after the default tonemap (ACES fit on 0.6 x, exposure 1).
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

// The linear radiance frame `b` adds over frame `a` at pixel p, channel ch.
f64 added(const Array<u8>& a, const Array<u8>& b, usize p, u32 ch) {
    return linear_of(b[p * 4 + ch]) - linear_of(a[p * 4 + ch]);
}

f32 dot3(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

f32 angle(vec3 a, vec3 b) {
    const f32 c = dot3(a, b);
    return std::acos(c < 1.0f ? (c > -1.0f ? c : -1.0f) : 1.0f);
}

// No bloom and no FXAA: each carries a changed pixel to its neighbours (across the horizon too).
void plain_post(Scene3D& s) {
    PostFx post = s.post();
    post.bloom = false;
    post.fxaa = false;
    s.set_post(post);
}

struct CvarScope {  // sets a convar for the scope, resets it after
    ConVar* cv;
    CvarScope(const char* name, const char* value) : cv(ConVar::find(StringId(name))) {
        REQUIRE(cv != nullptr);
        REQUIRE(cv->set_string(value));
    }
    ~CvarScope() { cv->reset(); }
    CvarScope(const CvarScope&) = delete;
    CvarScope& operator=(const CvarScope&) = delete;
};

// A full cirrus deck: coverage 1 and density 1 make the opacity the layer's cap nearly everywhere, so the veil is 1
// above the horizon fade and the halo's own shape shows.
Cirrus full_deck() {
    Cirrus c = cirrus_carrier(1.0f);
    c.density = 1.0f;
    return c;
}

// The halo day (halo null: none) at sun elevation `el`, the cirrus `cirrus`, the low layer `clouds`.
Array<u8> draw_day(Fixture3D& f, const Halo* halo, const Cirrus& cirrus, f32 el = 12.0f,
                   const Clouds& clouds = Clouds{}, const std::function<void(Scene3D&)>& more = {}) {
    f.render(0.0f, true, [&](Scene3D& s) {
        halo_sky_scene(s, cirrus, halo ? *halo : Halo{}, el, clouds);  // Halo{}: strength 0
        if (more) more(s);
        plain_post(s);
    });
    return f.pixels;
}

Array<u8> draw_night(Fixture3D& f, const Halo* halo, const Cirrus& cirrus, const NightSky& night = halo_night_carrier()) {
    f.render(0.0f, true, [&](Scene3D& s) {
        halo_night_scene(s, night, cirrus, halo ? *halo : Halo{});
        plain_post(s);
    });
    return f.pixels;
}

// The mean linear radiance (per channel) the halo adds in angular bins of `bin` radians from `center`, over the pixels
// whose ray passes `keep`.
struct Profile {
    Array<f64> r, g, b;
    Array<u32> n;
    f32 lo = 0.0f, bin = 0.0f;
    f32 at(usize i) const { return lo + (f32(i) + 0.5f) * bin; }
    usize peak() const {
        usize best = 0;
        for (usize i = 0; i < g.size(); ++i)
            if (n[i] && g[i] > g[best]) best = i;
        return best;
    }
};
Profile profile(const Array<u8>& base, const Array<u8>& with, const Array<vec3>& dirs, vec3 center, f32 lo, f32 hi,
                f32 bin, const std::function<bool(vec3)>& keep) {
    Profile pr;
    pr.lo = lo;
    pr.bin = bin;
    const usize nb = usize((hi - lo) / bin);
    pr.r.resize(nb);
    pr.g.resize(nb);
    pr.b.resize(nb);
    pr.n.resize(nb);
    for (usize i = 0; i < nb; ++i) {
        pr.r[i] = pr.g[i] = pr.b[i] = 0.0;
        pr.n[i] = 0;
    }
    for (usize p = 0; p < dirs.size(); ++p) {
        if (!keep(dirs[p])) continue;
        const f32 t = angle(dirs[p], center);
        if (t < lo || t >= hi) continue;
        const usize i = std::min(usize((t - lo) / bin), nb - 1);
        pr.r[i] += added(base, with, p, 0);
        pr.g[i] += added(base, with, p, 1);
        pr.b[i] += added(base, with, p, 2);
        ++pr.n[i];
    }
    for (usize i = 0; i < nb; ++i)
        if (pr.n[i]) {
            pr.r[i] /= f64(pr.n[i]);
            pr.g[i] /= f64(pr.n[i]);
            pr.b[i] /= f64(pr.n[i]);
        }
    return pr;
}

// The radial centroid (rad) of channel `ch` of a profile over its bins from lo to hi (rad).
f64 centroid(const Profile& pr, int ch, f32 lo, f32 hi) {
    f64 m = 0.0, w = 0.0;
    for (usize i = 0; i < pr.g.size(); ++i) {
        const f32 t = pr.at(i);
        if (t < lo || t > hi || !pr.n[i]) continue;
        const f64 v = std::max(ch == 0 ? pr.r[i] : (ch == 1 ? pr.g[i] : pr.b[i]), 0.0);
        m += v * f64(t);
        w += v;
    }
    return w > 0.0 ? m / w : 0.0;
}

}  // namespace

TEST_CASE("render3d_gl: the halo at strength 0, disabled, NaN, Low tier, r.halo 0 or without cirrus draws the plain frame") {
    Fixture3D f;
    halo_camera(f);
    const Cirrus deck = halo_cirrus();
    const Array<u8> none = draw_day(f, nullptr, deck);
    const Halo zero = halo_carrier(0.0f);
    CHECK(same(draw_day(f, &zero, deck), none));
    Halo off = halo_carrier(1.0f);
    off.enabled = false;
    CHECK(same(draw_day(f, &off, deck), none));
    const Halo nan = halo_carrier(std::nanf(""));  // the upload clamps again: a NaN strength is off
    CHECK(same(draw_day(f, &nan, deck), none));
    const Halo low = apply_quality(quality_settings(QualityTier::Low), halo_carrier(1.0f));
    CHECK(low.strength == 0.0f);
    CHECK(same(draw_day(f, &low, deck), none));
    {
        CvarScope cv("r.halo", "0");
        const Halo culled = apply_quality(quality_settings(QualityTier::High), halo_carrier(1.0f));
        CHECK(culled.strength == 0.0f);
        CHECK(same(draw_day(f, &culled, deck), none));
    }
    const Halo high = apply_quality(quality_settings(QualityTier::High), halo_carrier(1.0f));
    CHECK(high.strength == 1.0f);
    CHECK_FALSE(same(draw_day(f, &high, deck), none));
    // No cirrus (coverage 0, the cirrus's Low tier): no halo, the frame without either bit for bit.
    const Array<u8> clear = draw_day(f, nullptr, Cirrus{});
    CHECK(same(draw_day(f, &high, Cirrus{}), clear));
    const Cirrus low_cirrus = apply_quality(quality_settings(QualityTier::Low), halo_cirrus());
    CHECK(same(draw_day(f, &high, low_cirrus), clear));
    // The low layer on: the halo off is the low layer's frame bit for bit.
    const Clouds low_deck = cloud_sky_clouds(0.4f);
    CHECK(same(draw_day(f, &zero, deck, 12.0f, low_deck), draw_day(f, nullptr, deck, 12.0f, low_deck)));
    // The sun under the horizon: none.
    CHECK(same(draw_day(f, &high, deck, -5.0f), draw_day(f, nullptr, deck, -5.0f)));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the ring peaks 22 degrees from the sun, sharp inside, soft outside, red inside and blue outside") {
    Fixture3D f;
    halo_camera(f);
    const Cirrus deck = full_deck();
    const Array<u8> none = draw_day(f, nullptr, deck);
    const Array<vec3> dirs = ray_dirs(f.view);
    const Halo h = halo_carrier(1.0f);
    const Array<u8> with = draw_day(f, &h, deck);
    u32 moved_above = 0, moved_below = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        const bool moved = !same_px(none, with, p * 4);
        (dirs[p].y > kCirrusMinUp ? moved_above : moved_below) += moved ? 1u : 0u;
    }
    MESSAGE("pixels the halo moved above / at or under the horizon limit: " << moved_above << " / " << moved_below);
    CHECK(moved_above > 1500);
    CHECK(moved_below == 0);
    // The radial profile above the dogs (elevation over 20 degrees), in 0.5 degree bins from 10 to 34 degrees.
    const vec3 sun = halo_sun_toward(12.0f);
    const auto above_dogs = [](vec3 d) { return d.y > std::sin(20.0f * kDeg); };
    const Profile pr = profile(none, with, dirs, sun, 10.0f * kDeg, 34.0f * kDeg, 0.5f * kDeg, above_dogs);
    const usize pk = pr.peak();
    const f32 peak_deg = pr.at(pk) / kDeg;
    const f64 inner = (pr.g[pk] - pr.g[pk - 3]) / 1.5, outer = (pr.g[pk] - pr.g[pk + 3]) / 1.5;
    MESSAGE("ring peak at " << peak_deg << " deg (" << pr.g[pk] << " linear); slope per degree inside / outside: " << inner
                            << " / " << outer << "; at 18 deg " << pr.g[usize((18.0f - 10.0f) / 0.5f)]);
    CHECK(peak_deg > 21.0f);
    CHECK(peak_deg < 23.5f);
    CHECK(pr.g[pk] > 0.05);
    CHECK(inner > 1.5 * outer);
    CHECK(outer > 0.0);
    CHECK(std::fabs(pr.g[usize((18.0f - 10.0f) / 0.5f)]) < 0.1 * pr.g[pk]);  // inside the ring: nothing added
    // The dispersion: the red channel's centroid sits inside the blue one's with dispersion 1, not with 0.
    Halo flat = h;
    flat.dispersion = 0.0f;
    const Array<u8> neutral = draw_day(f, &flat, deck);
    const Profile pn = profile(none, neutral, dirs, sun, 10.0f * kDeg, 34.0f * kDeg, 0.5f * kDeg, above_dogs);
    const f32 lo = 20.0f * kDeg, hi = 26.0f * kDeg;
    const f64 split1 = (centroid(pr, 2, lo, hi) - centroid(pr, 0, lo, hi)) / kDeg;
    const f64 split0 = (centroid(pn, 2, lo, hi) - centroid(pn, 0, lo, hi)) / kDeg;
    MESSAGE("blue minus red centroid, dispersion 1 / 0: " << split1 << " / " << split0 << " deg");
    CHECK(split1 > 0.15);
    CHECK(split1 > split0 + 0.1);
    CHECK(std::fabs(split0) < 0.1);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the sun dogs sit on the sun's horizon line 22 degrees away, wider apart and dimmer when higher") {
    Fixture3D f;
    halo_camera(f);
    const Cirrus deck = full_deck();
    const Halo h = halo_carrier(1.0f);
    struct Found {
        f32 sep_deg = 0.0f;  // the azimuth between the two dogs
        f64 bright = 0.0;    // the mean linear green they add within 2 degrees of their centres
    };
    const auto find = [&](f32 el) {
        f.pitch_deg = -el;  // the camera level with the sun: the dogs on the frame's middle row
        const Array<u8> none = draw_day(f, nullptr, deck, el);
        const Array<u8> with = draw_day(f, &h, deck, el);
        const Array<vec3> dirs = ray_dirs(f.view);
        const vec3 sun = halo_sun_toward(el);
        Found out;
        f32 az[2] = {0.0f, 0.0f};
        for (int k = 0; k < 2; ++k) {
            const vec3 dog = halo_sun_dog_center(sun, k == 0 ? -1.0f : 1.0f);
            f64 best = -1.0, sum = 0.0;
            u32 n = 0;
            vec3 at{};
            for (usize p = 0; p < dirs.size(); ++p) {
                const f32 a = angle(dirs[p], dog);
                if (a > 6.0f * kDeg) continue;
                const f64 g = added(none, with, p, 1);
                if (g > best) {
                    best = g;
                    at = dirs[p];
                }
                if (a < 2.0f * kDeg) {
                    sum += g;
                    ++n;
                }
            }
            REQUIRE(n > 4);
            out.bright += sum / f64(n) * 0.5;
            const f32 el_found = std::asin(at.y) / kDeg;
            MESSAGE("sun at " << el << " deg, dog " << k << ": brightest " << angle(at, sun) / kDeg
                              << " deg from the sun at elevation " << el_found << " (" << angle(at, dog) / kDeg
                              << " deg from the predicted centre)");
            CHECK(std::fabs(el_found - el) < 1.5f);
            CHECK(angle(at, dog) < 1.5f * kDeg);
            CHECK(angle(at, sun) / kDeg == doctest::Approx(22.0f).epsilon(0.07));
            az[k] = std::atan2(at.x, -at.z);
        }
        out.sep_deg = (az[1] - az[0]) / kDeg;
        return out;
    };
    const Found low = find(8.0f), high = find(25.0f);
    MESSAGE("dogs apart in azimuth at 8 / 25 deg: " << low.sep_deg << " / " << high.sep_deg << "; brightness " << low.bright
                                                    << " / " << high.bright);
    CHECK(std::fabs(low.sep_deg) == doctest::Approx(44.5f).epsilon(0.05));
    CHECK(std::fabs(high.sep_deg) > std::fabs(low.sep_deg) + 2.0f);
    CHECK(low.bright > 1.2 * high.bright);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: no halo in the gaps between the streaks, and a thick low deck in front hides it") {
    Fixture3D f;
    halo_camera(f);
    const Cirrus thin = cirrus_carrier(0.5f);  // broken streaks
    const Array<u8> none = draw_day(f, nullptr, thin);
    const Array<vec3> dirs = ray_dirs(f.view);
    const Halo h = halo_carrier(1.0f);
    const Array<u8> with = draw_day(f, &h, thin);
    const CirrusParams cp = cirrus_shader_params(thin, 30.0f);
    const vec3 sun = halo_sun_toward(12.0f);
    u32 gap = 0, gap_moved = 0, streak = 0, streak_moved = 0;
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x + 1 < kW; ++x) {
            const usize p = usize(y) * kW + x;
            const vec3 d = dirs[p];
            const f32 t = angle(d, sun);
            if (d.y < 0.15f || t < 21.5f * kDeg || t > 24.0f * kDeg) continue;  // on the ring
            const f32 pix = math::length(dirs[p + 1] - d);
            const f32 a = cirrus_opacity(d, cp, pix);
            if (a == 0.0f) {
                ++gap;
                gap_moved += same_px(none, with, p * 4) ? 0u : 1u;
            } else if (a > 0.08f) {
                ++streak;
                streak_moved += same_px(none, with, p * 4) ? 0u : 1u;
            }
        }
    MESSAGE("on the ring: gap pixels " << gap << " (" << gap_moved << " moved), streak pixels " << streak << " ("
                                       << streak_moved << " moved)");
    REQUIRE(gap > 30);
    REQUIRE(streak > 30);
    CHECK(gap_moved <= gap / 20);  // a pixel's neighbour rounding across the mask edge
    CHECK(streak_moved > streak * 8 / 10);

    // A full low deck in front: where the low layer is opaque (its CPU twin, cloud_shadow.h, at the pixel's point on its
    // plane gives a full opacity, past its horizon fade) the halo moves nothing; in the open it moves most of them.
    const Cirrus deck = full_deck();
    Clouds low = cloud_sky_clouds(1.0f);
    low.density = 1.0f;
    const Array<u8> open_none = draw_day(f, nullptr, deck), open_with = draw_day(f, &h, deck);
    const Array<u8> under_none = draw_day(f, nullptr, deck, 12.0f, low), under_with = draw_day(f, &h, deck, 12.0f, low);
    CloudDensityParams dp;
    dp.coverage = low.coverage;
    dp.density = low.density;
    dp.scale_m = low.scale_m;
    dp.drift_cells = cloud_shadow_drift(30.0f, low.speed, low.scale_m, low.wind_dir);
    u32 checked = 0, shown = 0, hidden_moved = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        const f32 t = angle(dirs[p], sun);
        if (dirs[p].y <= 0.3f || t < 22.0f * kDeg || t > 25.0f * kDeg) continue;  // on the ring, high
        const f32 k = low.height_m / dirs[p].y;
        if (cloud_density(math::vec2{dirs[p].x * k, dirs[p].z * k}, dp) < 0.9999f) continue;
        ++checked;
        shown += same_px(open_none, open_with, p * 4) ? 0u : 1u;
        for (u32 ch = 0; ch < 3; ++ch)
            hidden_moved += std::abs(i32(under_with[p * 4 + ch]) - i32(under_none[p * 4 + ch])) > 1 ? 1u : 0u;
    }
    MESSAGE("over " << checked << " high ring pixels: the halo alone moved " << shown
                    << ", behind a full low deck channels moved by more than 1: " << hidden_moved);
    REQUIRE(checked > 50);
    CHECK(shown > checked * 8 / 10);
    CHECK(hidden_moved == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the moon's ring is fainter than the sun's, centred on the moon, and absent by day") {
    Fixture3D f;
    halo_camera(f);
    const Cirrus deck = full_deck();
    const Halo h = halo_carrier(1.0f);
    const Array<u8> none = draw_night(f, nullptr, deck);
    const Array<vec3> dirs = ray_dirs(f.view);
    const Array<u8> with = draw_night(f, &h, deck);
    const auto high = [](vec3 d) { return d.y > 0.15f; };
    const Profile pm = profile(none, with, dirs, kHaloMoonDir, 10.0f * kDeg, 34.0f * kDeg, 0.5f * kDeg, high);
    const usize pk = pm.peak();
    // The sun's (day exposure 0.6: the linear radiance is linear_of / 0.6; the night's exposure is 1).
    const Array<u8> day_none = draw_day(f, nullptr, deck), day_with = draw_day(f, &h, deck);
    const Profile ps = profile(day_none, day_with, dirs, halo_sun_toward(12.0f), 10.0f * kDeg, 34.0f * kDeg, 0.5f * kDeg,
                               [](vec3 d) { return d.y > std::sin(20.0f * kDeg); });
    const f64 sun_peak = ps.g[ps.peak()] / 0.6;
    MESSAGE("moon ring peak at " << pm.at(pk) / kDeg << " deg, " << pm.g[pk] << " linear; the sun's " << sun_peak);
    CHECK(pm.at(pk) / kDeg > 21.0f);
    CHECK(pm.at(pk) / kDeg < 23.5f);
    CHECK(pm.g[pk] > 0.0);
    CHECK(pm.g[pk] < 0.2 * sun_peak);
    // By day (the night fade 0: no moon uniforms) the moon draws no ring.
    NightSky day = halo_night_carrier();
    day.night_fade = 0.0f;
    CHECK(same(draw_night(f, &h, deck, day), draw_night(f, nullptr, deck, day)));
    // No moon: none.
    const NightSky dark = halo_night_carrier(0.0f);
    CHECK(same(draw_night(f, &h, deck, dark), draw_night(f, nullptr, deck, dark)));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the sea reflects the halo, and the sea without it is unchanged") {
    Fixture3D f;
    water_camera(f);
    const auto sea = [&](const Halo* h) {
        f.render(0.0f, true, [&](Scene3D& s) {
            halo_sky_scene(s, full_deck(), h ? *h : Halo{}, 14.0f);
            s.set_water(water_carrier(0.0f));
            plain_post(s);
        });
        return f.pixels;
    };
    const Array<u8> none = sea(nullptr);
    const Array<vec3> dirs = ray_dirs(f.view);
    const Halo zero = halo_carrier(0.0f);
    CHECK(same(sea(&zero), none));
    const Halo h = halo_carrier(1.0f);
    const Array<u8> with = sea(&h);
    u32 sky = 0, water = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (same_px(none, with, p * 4)) continue;
        if (dirs[p].y > 0.0f)
            ++sky;
        else
            ++water;
    }
    MESSAGE("calm sea: pixels the halo moved above / under the horizon: " << sky << " / " << water);
    CHECK(sky > 300);
    CHECK(water > 100);  // the reflection
    CHECK(f.device->validation_error_count() == 0);
}

namespace {

// The halo day at w x h from the fixture's camera (yaw `yaw_deg`), as Fixture3D::render but into its own target.
Array<u8> draw_at(Fixture3D& f, u32 w, u32 h, f32 yaw_deg, const Halo* halo, View3D& view) {
    rhi::TextureDesc td;
    td.size = {w, h};
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.halo.crawl";
    const rhi::TextureHandle t = f.device->create_texture(td);
    const Camera3D cam = orbit_camera(f.look_at, math::radians(yaw_deg), math::radians(f.pitch_deg), 7.0f);
    f.device->begin_frame(f.frame++);
    Scene3D scene(f.renderer.meshes(), mem::heap());
    scene.begin(cam, {w, h});
    halo_sky_scene(scene, full_deck(), halo ? *halo : Halo{}, 12.0f);
    plain_post(scene);
    scene.build();
    view = scene.view();
    rhi::CommandBuffer cmd;
    f.renderer.record_3d(cmd, scene, t);
    f.device->submit(cmd);
    Array<u8> px;
    px.resize(usize(w) * h * 4);
    REQUIRE(f.device->read_pixels(t, rhi::Rect2D{0, 0, w, h}, px.span()).has_value());
    f.device->end_frame();
    f.device->destroy_texture(t);
    return px;
}

}  // namespace

TEST_CASE("render3d_gl: the halo stays put across an origin shift, and a sub-pixel yaw moves it smoothly (the crawl)") {
    Fixture3D f;
    halo_camera(f);
    const Halo h = halo_carrier(1.0f);
    const Array<u8> home = draw_day(f, &h, full_deck());
    const auto rebase = [&](Scene3D& s) { s.set_origin_shift(vec3{1000.0f, 0.0f, 1000.0f}); };
    CHECK(same(draw_day(f, &h, full_deck(), 12.0f, Clouds{}, rebase), home));
    // The crawl: what the halo adds (the frame with it minus the frame without it, in linear green) at yaw 0 and at a
    // quarter-pixel yaw. A band-limited ring moves by a quarter of its edge at most; an aliased one would jump by its
    // whole height at the pixels its edge crosses. The 99th percentile of the change, over the peak.
    const u32 sizes[2][2] = {{160u, 120u}, {640u, 360u}};
    for (const auto& wh : sizes) {
        const u32 w = wh[0], hh = wh[1];
        const f32 fov_x = 2.0f * std::atan(std::tan(math::radians(22.5f)) * f32(w) / f32(hh));
        const f32 step = 0.25f * fov_x / f32(w) / kDeg;  // a quarter pixel, in degrees
        View3D v0, v1;
        const Array<u8> n0 = draw_at(f, w, hh, 0.0f, nullptr, v0), h0 = draw_at(f, w, hh, 0.0f, &h, v0);
        const Array<u8> n1 = draw_at(f, w, hh, step, nullptr, v1), h1 = draw_at(f, w, hh, step, &h, v1);
        const Array<vec3> dirs = ray_dirs(v0, w, hh);
        Array<f64> dv;
        f64 peak = 0.0;
        for (usize p = 0; p < dirs.size(); ++p) {
            if (dirs[p].y < 0.05f) continue;
            const f64 a0 = added(n0, h0, p, 1), a1 = added(n1, h1, p, 1);
            peak = std::max(peak, a0);
            if (a0 > 0.0 || a1 > 0.0) dv.push_back(std::fabs(a1 - a0));
        }
        REQUIRE(dv.size() > 100);
        std::sort(dv.begin(), dv.end());
        const f64 p99 = dv[dv.size() * 99 / 100];
        MESSAGE(w << " x " << hh << ": a " << step << " deg yaw changes the halo by " << p99 << " (99th pct) of a "
                  << peak << " peak over " << dv.size() << " pixels");
        CHECK(peak > 0.05);
        CHECK(p99 < 0.15 * peak);
    }
    CHECK(f.device->validation_error_count() == 0);
}

// ---- the halo's GLSL alone against the CPU twin --------------------------------------------------------------------

namespace {

u16 to_half(f32 v) {  // f32 to IEEE half, round to nearest even (normal range only: the case's values)
    u32 b;
    std::memcpy(&b, &v, 4);
    const u32 sign = (b >> 16) & 0x8000u;
    const i32 e = i32((b >> 23) & 0xffu) - 127 + 15;
    u32 m = b & 0x7fffffu;
    if (e <= 0) return u16(sign);
    if (e >= 31) return u16(sign | 0x7c00u);
    u32 h = sign | (u32(e) << 10) | (m >> 13);
    m &= 0x1fffu;
    if (m > 0x1000u || (m == 0x1000u && (h & 1u))) ++h;
    return u16(h);
}

f32 from_half(u16 h) {  // normal range and zero only
    const u32 sign = u32(h & 0x8000u) << 16;
    const u32 e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
    const u32 b = e == 0 ? sign : sign | ((e - 15 + 127) << 23) | (m << 13);
    f32 v;
    std::memcpy(&v, &b, 4);
    return v;
}

f32 halfed(f32 v) { return from_half(to_half(v)); }

// The test's own PassU: the names the halo's text reads. t_sun: xyz toward the sun, w the cirrus opacity.
constexpr const char* kTwinHead = R"(#version 450 core
layout(std140, binding = 1) uniform PassU {
    vec4 p_light_color;
    vec4 p_night;
    vec4 p_moon;
    vec4 p_moon2;
    vec4 p_halo;
    vec4 t_sun;
};
)";
// Texel x of the 4 N x 1 target: direction x / 4 (u_dirs: xyz the direction, w the radians per pixel), output x % 4
// (0..2 halo_term's rgb, 3 the green ring profile at the angle from the sun), its float bits as four bytes.
constexpr const char* kTwinMain = R"(
layout(binding = 6) uniform sampler2D u_dirs;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    int x = int(gl_FragCoord.x);
    vec4 dp = texelFetch(u_dirs, ivec2(x / 4, 0), 0);
    vec3 d = dp.xyz;
    int k = x % 4;
    float cs = dot(d, t_sun.xyz);
    float v;
    if (k == 3) {
        v = halo_ring(acos(clamp(cs, -1.0, 1.0)) - 0.38397244, dp.w);
    } else {
        vec3 c = halo_term(d, t_sun.xyz, cs, t_sun.w, dp.w);
        v = k == 0 ? c.r : (k == 1 ? c.g : c.b);
    }
    o_color = unpackUnorm4x8(floatBitsToUint(v));
}
)";

struct TwinBlock {
    f32 light[4], night[4], moon[4], moon2[4], halo[4], sun[4];
};

Array<f32> run_twin(Fixture3D& f, const Array<vec3>& dirs, const Array<f32>& pix, const TwinBlock& u) {
    const u32 n = u32(dirs.size()), w = n * 4;
    Array<u16> data;
    data.resize(usize(n) * 4);
    for (u32 i = 0; i < n; ++i) {
        data[i * 4] = to_half(dirs[i].x);
        data[i * 4 + 1] = to_half(dirs[i].y);
        data[i * 4 + 2] = to_half(dirs[i].z);
        data[i * 4 + 3] = to_half(pix[i]);
    }
    rhi::TextureDesc dd;
    dd.size = {n, 1};
    dd.format = rhi::TextureFormat::RGBA16F;
    dd.usage = rhi::TextureUsage::Sampled;
    dd.debug_name = "test.halo.dirs";
    const rhi::TextureHandle tex =
        f.device->create_texture(dd, Span<const byte>(reinterpret_cast<const byte*>(data.data()), data.size() * 2));
    const std::string fs = std::string(kTwinHead) + shaders3d::kHaloGLSL + kTwinMain;
    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = {shaders3d::kFullscreenVS, "fullscreen.vert"};
    pd.fragment_shader = {fs.c_str(), "test.halo_twin.frag"};
    pd.state.depth = {};
    pd.debug_name = "test.halo_twin";
    auto pipe = f.device->create_pipeline(pd);
    REQUIRE(pipe.has_value());
    rhi::TextureDesc td;
    td.size = {w, 1};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.halo_twin.out";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    const rhi::SamplerHandle point = f.device->create_sampler(sd);
    f.device->begin_frame(f.frame++);
    rhi::CommandBuffer cmd;
    rhi::PassAttachment o;
    o.texture = rt;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&o, 1);
    p.debug_name = "test.halo_twin";
    cmd.begin_pass(p);
    cmd.bind_pipeline(pipe.value());
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(u));
    cmd.bind_texture(rhi::bank::kAlbedo, tex, point);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    f.device->submit(cmd);
    Array<u8> px;
    px.resize(usize(w) * 4);
    REQUIRE(f.device->read_pixels(rt, rhi::Rect2D{0, 0, w, 1}, px.span()).has_value());
    f.device->end_frame();
    CHECK(f.device->validation_error_count() == 0);
    Array<f32> out;
    out.resize(w);
    for (u32 i = 0; i < w; ++i) {
        const u32 b =
            u32(px[i * 4]) | (u32(px[i * 4 + 1]) << 8) | (u32(px[i * 4 + 2]) << 16) | (u32(px[i * 4 + 3]) << 24);
        std::memcpy(&out[i], &b, 4);
    }
    f.device->destroy_texture(tex);
    f.device->destroy_texture(rt);
    f.device->destroy_pipeline(pipe.value());
    f.device->destroy_sampler(point);
    return out;
}

// The ray `theta` from `c` toward azimuth `phi` around it.
vec3 around(vec3 c, f32 theta, f32 phi) {
    const vec3 up = std::fabs(c.y) < 0.99f ? vec3{0.0f, 1.0f, 0.0f} : vec3{1.0f, 0.0f, 0.0f};
    const vec3 u = math::normalize(math::cross(up, c));
    const vec3 v = math::cross(c, u);
    const f32 st = std::sin(theta), ct = std::cos(theta), sp = std::sin(phi), cp = std::cos(phi);
    return vec3{c.x * ct + (u.x * cp + v.x * sp) * st, c.y * ct + (u.y * cp + v.y * sp) * st,
                c.z * ct + (u.z * cp + v.z * sp) * st};
}

}  // namespace

TEST_CASE("render3d_gl: the halo GLSL agrees with the CPU twin at probe angles around the sun, the dogs and the moon") {
    Fixture3D f;
    struct Case {
        f32 sun_el;   // the light's elevation (deg)
        bool moon;    // a moon 20 degrees up on the far side, its ring probed
        bool held;    // the moon holds the light (the light's direction is the moon's)
        f32 opacity;  // the cirrus opacity
        Halo h;
    };
    const auto halo = [](f32 strength, f32 dogs, f32 moon, f32 disp) {
        Halo h;
        h.strength = strength;
        h.sun_dogs = dogs;
        h.moon = moon;
        h.dispersion = disp;
        return h;
    };
    const Case cases[] = {
        {12.0f, false, false, 0.5f, halo(1.0f, 1.0f, 0.5f, 1.0f)},  {3.0f, false, false, 0.3f, halo(0.7f, 1.0f, 0.5f, 0.0f)},
        {35.0f, false, false, 0.45f, halo(1.0f, 0.5f, 0.5f, 0.6f)}, {65.0f, false, false, 0.5f, halo(1.0f, 1.0f, 0.5f, 1.0f)},
        {-30.0f, true, false, 0.4f, halo(1.0f, 1.0f, 1.0f, 1.0f)},  {20.0f, true, true, 0.5f, halo(1.0f, 1.0f, 0.5f, 1.0f)},
    };
    usize compared = 0, lit = 0, dogs_hit = 0;
    f32 worst = 0.0f;
    u32 off = 0;
    for (const Case& k : cases) {
        const f32 e = k.sun_el * kDeg;
        vec3 sun{std::cos(e) * 0.2f, std::sin(e), -std::cos(e) * 0.98f};
        sun = math::normalize(sun);
        const vec3 moon_dir = k.held ? sun : math::normalize(vec3{0.3f, std::sin(20.0f * kDeg), 0.9f});
        HaloLight l;
        l.light = k.moon && !k.held ? vec3{0.03f, 0.035f, 0.05f} : vec3{2.0f, 1.92f, 1.8f};
        if (k.moon) {
            l.moon_intensity = 6.0f;
            l.night_fade = 1.0f;
            l.moon_dir = moon_dir;
            l.moon_tint = vec3{0.9f, 0.93f, 1.0f};
        }
        // Probes: a polar grid around the sun (the ring and its inside), small grids on both dogs and, with a moon,
        // around it; pixel footprints from 0.3 to 25 milliradians (the edge floor and the fade), half precision.
        Array<vec3> dirs;
        Array<f32> pix;
        const auto add = [&](vec3 d, u32 i) {
            dirs.push_back(vec3{halfed(d.x), halfed(d.y), halfed(d.z)});
            pix.push_back(halfed(0.0003f + 0.0247f * f32((i * 7) % 29) / 28.0f * f32((i * 3) % 5 == 0 ? 1 : 0) +
                                 0.004f * f32((i * 11) % 7) / 6.0f));
        };
        u32 idx = 0;
        for (u32 i = 0; i < 70; ++i)
            for (u32 j = 0; j < 12; ++j)
                add(around(sun, (14.0f + 18.0f * f32(i) / 69.0f) * kDeg, 6.2831853f * (f32(j) + 0.3f) / 12.0f), idx++);
        for (const f32 side : {-1.0f, 1.0f}) {
            const vec3 dog = halo_sun_dog_center(sun, side);
            for (u32 i = 0; i < 9; ++i)
                for (u32 j = 0; j < 9; ++j)
                    add(around(dog, 3.0f * kDeg * f32(i) / 8.0f, 6.2831853f * f32(j) / 9.0f), idx++);
        }
        if (k.moon)
            for (u32 i = 0; i < 40; ++i)
                for (u32 j = 0; j < 8; ++j)
                    add(around(moon_dir, (18.0f + 10.0f * f32(i) / 39.0f) * kDeg, 6.2831853f * f32(j) / 8.0f), idx++);
        const HaloParams p = halo_shader_params(k.h);
        REQUIRE(p.strength > 0.0f);
        TwinBlock u{};
        const f32 blk[6][4] = {{l.light.x, l.light.y, l.light.z, 0.0f},
                               {k.moon ? 0.998f : 0.0f, l.moon_intensity, 0.5f, l.night_fade},
                               {l.moon_dir.x, l.moon_dir.y, l.moon_dir.z, 0.0f},
                               {l.moon_tint.x, l.moon_tint.y, l.moon_tint.z, 0.0f},
                               {p.strength, p.sun_dogs, p.moon, p.dispersion},
                               {sun.x, sun.y, sun.z, k.opacity}};
        std::memcpy(&u, blk, sizeof u);
        const Array<f32> gpu = run_twin(f, dirs, pix, u);
        for (usize i = 0; i < dirs.size(); ++i) {
            const vec3 d = dirs[i];
            const f32 cs = d.x * sun.x + d.y * sun.y + d.z * sun.z;
            const vec3 c = halo_term(d, sun, cs, k.opacity, pix[i], p, l);
            const f32 ring = halo_ring(std::acos(std::fmin(std::fmax(cs, -1.0f), 1.0f)) - kHaloRadius, pix[i]);
            const f32 e3 = std::max(
                {std::fabs(gpu[i * 4] - c.x), std::fabs(gpu[i * 4 + 1] - c.y), std::fabs(gpu[i * 4 + 2] - c.z)});
            const f32 er = std::fabs(gpu[i * 4 + 3] - ring);
            // The sharp inner edge rises by 1 over 4 milliradians: the driver's acos (a polynomial, a few 1e-5 rad
            // off) moves a probe on it by a few 1e-3 of the light; allow a few probes 1e-2, none a gross one.
            if (e3 > 1e-2f || er > 1e-2f) ++off;
            CHECK_MESSAGE(e3 < 0.05f, "probe " << i << ": colour off by " << e3);
            CHECK_MESSAGE(er < 0.05f, "probe " << i << ": ring off by " << er);
            worst = std::max(worst, e3);
            lit += c.y > 1e-3f ? 1 : 0;
            ++compared;
        }
        dogs_hit += k.sun_el > 0.0f && k.sun_el < 60.0f && !k.held ? 1 : 0;
    }
    MESSAGE("halo GLSL vs CPU twin: " << compared << " probes (" << lit << " lit), worst |dcolour| " << worst
                                      << ", off by more than 1e-2 at " << off);
    CHECK(lit > compared / 4);
    CHECK(off <= compared / 200);
    CHECK(dogs_hit == 3);
}
