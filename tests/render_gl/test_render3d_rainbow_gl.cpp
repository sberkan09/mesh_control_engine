// M-RAINBOW 17c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the rainbow in the sky
// pass (p_rainbow / p_rainbow2, rainbow_layer in MCE_GLSL_SKY_LAYERS) and in the water's reflection. Level 0,
// `enabled` false, a NaN level and the Low tier draw the frame without the rainbow bit for bit; the bows change only
// the sky above the horizon and only inside their bands (none between them: the Alexander band); the primary sits at
// 42 degrees from the anti-solar point, red outside and violet inside; the secondary is fainter and reversed; a
// higher level is brighter; a cloud deck in front dims the bows and a dense fog hides them; the sea reflects them;
// the drawn light agrees with kit::rainbow_intensity and kit::rainbow_color at probe points; a floating-origin shift
// leaves the sky unchanged; a slow camera turn keeps the bows' light and the frame difference per step bounded (no
// crawl). The scene is the rainbow sky (fixture3d.h). One frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "fixture3d.h"
#include "mce/kit/rainbow.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

constexpr f64 kDeg = 180.0 / 3.14159265358979323846;
constexpr f64 kGain = 0.3;  // shaders3d.h kRainbowGain: the emission at level 1 and full intensity

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

// The angle of d from the anti-solar point, degrees (f64, as the shader's acos(dot(d, anti_solar))).
f64 angle_of(vec3 d, vec3 anti) {
    const f64 n = std::sqrt(f64(anti.x) * anti.x + f64(anti.y) * anti.y + f64(anti.z) * anti.z);
    const f64 c = (f64(d.x) * anti.x + f64(d.y) * anti.y + f64(d.z) * anti.z) / n;
    return std::acos(std::clamp(c, -1.0, 1.0)) * kDeg;
}

template <class T>
bool same(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize i) {
    return a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2];
}

// The linear radiance behind an 8-bit sRGB value after the default tonemap (ACES fit on 0.6 x, exposure 1, which the
// rainbow scene uses).
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

// The linear light the rainbow adds to pixel p, per channel (never negative).
vec3 added(const Array<u8>& px, const Array<u8>& bare, usize p) {
    const auto ch = [&](usize c) { return f32(std::max(0.0, linear_of(px[p * 4 + c]) - linear_of(bare[p * 4 + c]))); };
    return {ch(0), ch(1), ch(2)};
}

// Total luma the rainbow adds over the frame without it, over the pixels whose ray climbs in (min_up, max_up].
f64 flux(const Array<u8>& px, const Array<u8>& bare, const Array<vec3>& dirs, f32 min_up = -2.0f, f32 max_up = 2.0f) {
    f64 sum = 0.0;
    for (usize p = 0; p < dirs.size(); ++p)
        if (dirs[p].y > min_up && dirs[p].y <= max_up) sum += std::max(0.0, luma(px, p * 4) - luma(bare, p * 4));
    return sum;
}

// No bloom (a glow would spill across the horizon and the band edges: not what is measured).
void no_bloom(Scene3D& s) {
    PostFx post = s.post();
    post.bloom = false;
    s.set_post(post);
}

Array<u8> draw_bow(Fixture3D& f, const RainbowSky* rainbow, const std::function<void(Scene3D&)>& more = {}) {
    f.render(0.0f, true, [&](Scene3D& s) {
        rainbow_sky_scene(s, rainbow ? *rainbow : RainbowSky{});  // RainbowSky{}: level 0
        if (more) more(s);
        no_bloom(s);
    });
    return f.pixels;
}

Array<u8> draw_sea(Fixture3D& f, const RainbowSky* rainbow) {
    f.render(0.0f, true, [&](Scene3D& s) {
        rainbow_water_scene(s, rainbow ? *rainbow : RainbowSky{}, water_carrier(0.05f));
        no_bloom(s);
    });
    return f.pixels;
}

// The kit's Rainbow that the carrier `r` was made from (the fields the bows' shape reads).
kit::Rainbow kit_of(const RainbowSky& r) {
    kit::Rainbow k;
    k.band_width = r.band_width;
    k.secondary = r.secondary;
    return k;
}

// The kit's emission (intensity times colour) at `a` degrees, the band width 1 the kit's colour describes.
vec3 kit_emission(const kit::Rainbow& k, f64 a) {
    const f32 i = kit::rainbow_intensity(k, f32(a));
    const math::vec3 c = kit::rainbow_color(f32(a));
    return {i * c.x, i * c.y, i * c.z};
}

// The share of red (x) and blue (z) in the light the bows add over the pixels whose angle is in [lo, hi), above 0.1.
struct Share {
    f64 r = 0.0, g = 0.0, b = 0.0;
    f64 red() const { return r / std::max(r + g + b, 1e-9); }
    f64 blue() const { return b / std::max(r + g + b, 1e-9); }
    f64 sum() const { return r + g + b; }
};
Share share(const Array<u8>& px, const Array<u8>& bare, const Array<vec3>& dirs, f64 lo, f64 hi) {
    Share s;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (dirs[p].y <= 0.1f) continue;
        const f64 a = angle_of(dirs[p], kRainbowAntiSolar);
        if (a < lo || a >= hi) continue;
        const vec3 e = added(px, bare, p);
        s.r += e.x;
        s.g += e.y;
        s.b += e.z;
    }
    return s;
}

}  // namespace

TEST_CASE("render3d_gl: a rainbow at level 0, disabled, with a NaN level or on the Low tier is the plain frame") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_bow(f, nullptr);
    RainbowSky zero = rainbow_carrier(0.0f);
    CHECK(same(draw_bow(f, &zero), none));
    RainbowSky off = rainbow_carrier(2.0f);
    off.enabled = false;
    CHECK(same(draw_bow(f, &off), none));
    RainbowSky nan = rainbow_carrier(std::nanf(""));  // the upload clamps again: a NaN level is off
    CHECK(same(draw_bow(f, &nan), none));
    const RainbowSky low = apply_quality(quality_settings(QualityTier::Low), rainbow_carrier(2.0f));
    CHECK(low.level == 0.0f);
    CHECK(same(draw_bow(f, &low), none));
    const RainbowSky high = apply_quality(quality_settings(QualityTier::High), rainbow_carrier(1.0f));
    CHECK(high.level == 1.0f);
    CHECK_FALSE(same(draw_bow(f, &high), none));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the bows change only the sky above the horizon, inside their bands; a higher level is "
          "brighter") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_bow(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const RainbowSky one = rainbow_carrier(1.0f);
    const Array<u8> px = draw_bow(f, &one);
    // Bands at band width 1: the primary 40.3 .. 43.48 (the outer side steepened), the secondary 48.57 .. 53.8; a
    // pixel is 0.38 degrees across here, the filter reaches half a pixel either side.
    u32 sky = 0, below = 0, primary = 0, secondary = 0, gap = 0, outside = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        const bool moved = !same_px(px, none, p * 4);
        if (dirs[p].y <= 0.0f) {
            below += moved ? 1u : 0u;
            continue;
        }
        ++sky;
        if (!moved) continue;
        const f64 a = angle_of(dirs[p], kRainbowAntiSolar);
        if (a > 40.3 - 0.3 && a < 43.48 + 0.3) ++primary;
        else if (a > 48.57 - 0.3 && a < 53.8 + 0.3) ++secondary;
        else if (a > 43.48 + 0.3 && a < 48.57 - 0.3) ++gap;
        else ++outside;
    }
    MESSAGE("sky pixels " << sky << "; changed: primary band " << primary << ", secondary band " << secondary
                          << ", between the bows " << gap << ", elsewhere " << outside << ", at or below the horizon "
                          << below);
    REQUIRE(sky > 8000);
    CHECK(below == 0);
    CHECK(gap == 0);
    CHECK(outside == 0);
    CHECK(primary > 400);
    CHECK(secondary > 400);
    // Brighter at a higher level, everywhere it shows.
    const RainbowSky two = rainbow_carrier(2.0f), four = rainbow_carrier(4.0f);
    const f64 f1 = flux(px, none, dirs), f2 = flux(draw_bow(f, &two), none, dirs),
              f4 = flux(draw_bow(f, &four), none, dirs);
    MESSAGE("rainbow flux at level 1 / 2 / 4: " << f1 << " / " << f2 << " / " << f4);
    CHECK(f1 > 0.0);
    CHECK(f2 > 1.3 * f1);
    CHECK(f4 > 1.3 * f2);
    // No secondary: only the primary's band moves.
    const RainbowSky single = rainbow_carrier(2.0f, 1.0f, 0.0f);
    const Array<u8> one_bow = draw_bow(f, &single);
    u32 sec_moved = 0;
    for (usize p = 0; p < dirs.size(); ++p)
        if (dirs[p].y > 0.0f && angle_of(dirs[p], kRainbowAntiSolar) > 44.0 && !same_px(one_bow, none, p * 4))
            ++sec_moved;
    CHECK(sec_moved == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the primary bow is at 42 degrees, red outside and violet inside; the secondary is fainter and "
          "reversed") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_bow(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const RainbowSky r = rainbow_carrier(2.0f);
    const Array<u8> px = draw_bow(f, &r);
    // The mean added light per pixel in quarter-degree bins of the angle, over the channel sum of the kit's colour at
    // the bin (the ramp is brightest at yellow): the bows' intensity, whose peak between 38 and 46 is the primary.
    constexpr f64 kBin = 0.25, kFrom = 36.0;
    constexpr u32 kBins = 80;  // 36 .. 56 degrees
    f64 sum[kBins] = {}, cnt[kBins] = {};
    for (usize p = 0; p < dirs.size(); ++p) {
        if (dirs[p].y <= 0.1f) continue;
        const f64 a = angle_of(dirs[p], kRainbowAntiSolar);
        const i32 b = i32(std::floor((a - kFrom) / kBin));
        if (b < 0 || b >= i32(kBins)) continue;
        const vec3 e = added(px, none, p);
        sum[b] += f64(e.x + e.y + e.z);
        cnt[b] += 1.0;
    }
    const auto peak_in = [&](f64 lo, f64 hi, f64& at) {
        f64 best = 0.0;
        for (u32 b = 0; b < kBins; ++b) {
            const f64 c = kFrom + (f64(b) + 0.5) * kBin;
            if (c < lo || c > hi || cnt[b] < 5.0) continue;
            const math::vec3 k = kit::rainbow_color(f32(c));
            const f64 v = sum[b] / cnt[b] / f64(k.x + k.y + k.z);
            if (v > best) {
                best = v;
                at = c;
            }
        }
        return best;
    };
    f64 at_p = 0.0, at_s = 0.0;
    const f64 peak_p = peak_in(38.0, 46.0, at_p), peak_s = peak_in(47.0, 55.0, at_s);
    // Colour across the bows: the primary's outer edge against its inner edge, the secondary's the other way round.
    const Share p_out = share(px, none, dirs, 42.6, 43.3), p_in = share(px, none, dirs, 40.6, 41.4);
    const Share s_in = share(px, none, dirs, 48.8, 49.8), s_out = share(px, none, dirs, 52.2, 53.4);
    MESSAGE("primary peak " << peak_p << " at " << at_p << " degrees, secondary peak " << peak_s << " at " << at_s
                            << "; primary red share outside / inside " << p_out.red() << " / " << p_in.red()
                            << ", blue " << p_out.blue() << " / " << p_in.blue() << "; secondary red inside / outside "
                            << s_in.red() << " / " << s_out.red() << ", blue " << s_in.blue() << " / "
                            << s_out.blue());
    REQUIRE(peak_p > 0.0);
    CHECK(std::fabs(at_p - 42.0) <= 0.3);
    CHECK(std::fabs(at_s - 51.0) <= 0.5);
    CHECK(peak_s < 0.6 * peak_p);  // the kit's 0.35 peak, under the same horizon fade
    CHECK(peak_s > 0.1 * peak_p);
    REQUIRE(p_out.sum() > 0.0);
    REQUIRE(p_in.sum() > 0.0);
    CHECK(p_out.red() > p_in.red() + 0.2);
    CHECK(p_in.blue() > p_out.blue() + 0.2);
    REQUIRE(s_in.sum() > 0.0);
    REQUIRE(s_out.sum() > 0.0);
    CHECK(s_in.red() > s_out.red() + 0.2);
    CHECK(s_out.blue() > s_in.blue() + 0.2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the drawn bows follow kit::rainbow_intensity and kit::rainbow_color") {
    // Probe points against the kit's helpers: per sky pixel the linear light the bows add (the tonemap inverted) must
    // be level * kRainbowGain * the horizon fade * the kit's intensity times colour, averaged over the pixel's angular
    // footprint as the shader's filter does (4 samples across the angle between this pixel's ray and the next one's).
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_bow(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const RainbowSky r = rainbow_carrier(2.0f);
    const kit::Rainbow k = kit_of(r);
    const Array<u8> px = draw_bow(f, &r);
    u32 n = 0, ok = 0, colour_n = 0, colour_ok = 0;
    f64 worst = 0.0, worst_colour = 0.0;
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x + 1 < kW; ++x) {
            const usize p = usize(y) * kW + x;
            const vec3 d = dirs[p];
            if (d.y <= 0.0f) continue;
            const f64 a = angle_of(d, kRainbowAntiSolar);
            if (a < 39.0 || a > 55.0) continue;
            const f64 pd = std::acos(std::clamp(f64(math::dot(d, dirs[p + 1])), -1.0, 1.0)) * kDeg;
            vec3 want{0.0f, 0.0f, 0.0f};
            for (int s = 0; s < 4; ++s) want = want + kit_emission(k, a + pd * (f64(s) * 0.25 - 0.375));
            const f64 t = std::clamp(f64(d.y) / 0.1, 0.0, 1.0);
            const f32 scale = f32(0.25 * f64(r.level) * kGain * t * t * (3.0 - 2.0 * t));
            want = want * scale;
            const vec3 got = added(px, none, p);
            ++n;
            const f64 err = std::max({std::fabs(got.x - want.x), std::fabs(got.y - want.y), std::fabs(got.z - want.z)});
            const f64 tol = 0.02 + 0.08 * f64(std::max({want.x, want.y, want.z}));
            worst = std::max(worst, err);
            ok += err < tol ? 1u : 0u;
            const f64 ws = f64(want.x + want.y + want.z), gs = f64(got.x + got.y + got.z);
            if (ws < 0.15) continue;  // the colour where the bows add enough light to measure its chromaticity
            ++colour_n;
            const f64 ce = std::max(std::fabs(got.x / gs - want.x / ws), std::fabs(got.z / gs - want.z / ws));
            worst_colour = std::max(worst_colour, ce);
            colour_ok += ce < 0.06 ? 1u : 0u;
        }
    MESSAGE("light within tolerance in " << ok << " of " << n << " probe pixels (worst " << worst
                                         << "); red and blue share within 0.06 in " << colour_ok << " of " << colour_n
                                         << " (worst " << worst_colour << ")");
    REQUIRE(n > 2000);
    CHECK(f64(ok) >= 0.97 * f64(n));
    REQUIRE(colour_n > 200);
    CHECK(f64(colour_ok) >= 0.95 * f64(colour_n));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a cloud deck in front dims the bows and a dense fog hides them") {
    Fixture3D f;
    cloud_sky_camera(f);
    const RainbowSky r = rainbow_carrier(2.0f);
    const Array<u8> none = draw_bow(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    // Over the rays that climb more than 0.25, above the band where the deck fades into the horizon haze.
    const f64 clear = flux(draw_bow(f, &r), none, dirs, 0.25f);
    Clouds thin, deck;
    thin.coverage = deck.coverage = 1.0f;
    deck.density = 1.0f;
    const auto broken = [&](Scene3D& s) { s.set_clouds(thin); };
    const auto overcast = [&](Scene3D& s) { s.set_clouds(deck); };
    const f64 f_thin = flux(draw_bow(f, &r, broken), draw_bow(f, nullptr, broken), dirs, 0.25f);
    const f64 f_deck = flux(draw_bow(f, &r, overcast), draw_bow(f, nullptr, overcast), dirs, 0.25f);
    // A dense volumetric fog in front of the whole sky.
    const auto dense = [](Scene3D& s) {
        Volumetrics v;
        v.enabled = true;
        v.density = 0.2f;
        v.height_falloff = 0.0f;
        s.set_volumetrics(v);
    };
    const f64 f_fog = flux(draw_bow(f, &r, dense), draw_bow(f, nullptr, dense), dirs);
    const f64 clear_all = flux(draw_bow(f, &r), none, dirs);
    MESSAGE("rainbow flux above 0.25: clear " << clear << ", thin deck " << f_thin << ", thick deck " << f_deck
                                              << "; whole sky: clear " << clear_all << ", dense fog " << f_fog);
    REQUIRE(clear > 0.0);
    CHECK(f_thin < 0.6 * clear);
    CHECK(f_deck < 0.2 * clear);
    CHECK(f_fog < 0.05 * clear_all);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the sea reflects the bows, and the sea without them is unchanged") {
    Fixture3D f;
    water_camera(f);
    const Array<u8> none = draw_sea(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    RainbowSky zero = rainbow_carrier(0.0f);
    CHECK(same(draw_sea(f, &zero), none));
    RainbowSky off = rainbow_carrier(2.0f);
    off.enabled = false;
    CHECK(same(draw_sea(f, &off), none));
    const RainbowSky low = apply_quality(quality_settings(QualityTier::Low), rainbow_carrier(2.0f));
    CHECK(same(draw_sea(f, &low), none));
    const RainbowSky r = rainbow_carrier(2.0f);
    const Array<u8> lit = draw_sea(f, &r);
    u32 sky = 0, sea = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (same_px(lit, none, p * 4)) continue;
        (dirs[p].y > 0.0f ? sky : sea) += 1u;
    }
    const f64 sea_flux = flux(lit, none, dirs, -2.0f, -0.02f);
    MESSAGE("pixels the rainbow changes: sky " << sky << ", sea " << sea << " (sea flux " << sea_flux << ")");
    CHECK(sky > 100);
    CHECK(sea > 200);
    CHECK(sea_flux > 0.0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the bows stay put across an origin shift") {
    Fixture3D f;
    cloud_sky_camera(f);
    const RainbowSky r = rainbow_carrier(1.0f);
    const Array<u8> home = draw_bow(f, &r);
    // A floating-origin rebase: the layer is a function of the view direction alone.
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    CHECK(same(draw_bow(f, &r, [&](Scene3D& s) { s.set_origin_shift(shift); }), home));
    CHECK(f.device->validation_error_count() == 0);
}

namespace {

// A one-degree camera turn in tenth-degree steps: the bows' total light (lo .. hi over the steps) and the worst mean
// change of the luma they add per sky pixel between neighbouring steps, against the mean they add per lit pixel.
struct Crawl {
    f64 lo = 1e30, hi = 0.0, worst_step = 0.0, mean_lit = 0.0;
};
Crawl turn(Fixture3D& f, const RainbowSky& r) {
    Crawl c;
    Array<f64> prev;
    for (u32 i = 0; i <= 10; ++i) {
        f.yaw_deg = 0.1f * f32(i);
        const Array<u8> bare = draw_bow(f, nullptr);
        const Array<vec3> dirs = ray_dirs(f);
        const Array<u8> px = draw_bow(f, &r);
        Array<f64> add;  // the luma the bows add per pixel (0 at or below the horizon)
        add.reserve(dirs.size());
        f64 fl = 0.0;
        u32 lit = 0;
        for (usize p = 0; p < dirs.size(); ++p) {
            const f64 v = dirs[p].y > 0.0f ? luma(px, p * 4) - luma(bare, p * 4) : 0.0;
            add.push_back(v);
            fl += std::max(v, 0.0);
            lit += v > 1.0 ? 1u : 0u;
        }
        c.lo = std::min(c.lo, fl);
        c.hi = std::max(c.hi, fl);
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

TEST_CASE("render3d_gl: the bows keep their light under a slow camera turn (no crawl)") {
    // The bows' total light must stay put (it moves only as the bows slide along the frame's edges), and the light
    // they add to a pixel may move only as far as a sub-pixel shift of a smooth pattern moves it. The default bows
    // (3.2 and 5.2 degrees across at half height, 8 and 14 of the frame's 0.38 degree pixels) and the narrowest (band
    // width 0.5: the primary's violet segment under a pixel) are both measured.
    Fixture3D f;
    cloud_sky_camera(f);
    for (const RainbowSky& r : {rainbow_carrier(2.0f), rainbow_carrier(2.0f, 0.5f)}) {
        const Crawl c = turn(f, r);
        MESSAGE("band width " << r.band_width << ": rainbow flux over a one-degree turn " << c.lo << " .. " << c.hi
                              << "; worst mean change of the added luma per sky pixel and step " << c.worst_step
                              << " (mean added luma per lit pixel " << c.mean_lit << ")");
        REQUIRE(c.lo > 0.0);
        CHECK(c.hi / c.lo < 1.02);  // measured 1.014 for both
        CHECK(c.worst_step < 0.02 * c.mean_lit);  // measured 0.010 and 0.011
    }
    CHECK(f.device->validation_error_count() == 0);
}
