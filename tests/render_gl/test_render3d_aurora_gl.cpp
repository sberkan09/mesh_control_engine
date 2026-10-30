// M-AURORA 16c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the aurora curtains in
// the sky pass (p_aurora .. p_aurora4, aurora_layer in MCE_GLSL_SKY_LAYERS) and in the water's reflection. Level 0,
// `enabled` false, a NaN level and the Low tier draw the frame without the aurora bit for bit; the curtains change
// only the sky above the horizon and only inside the arc; a higher level is brighter; the lower band is greener than
// the top; a cloud deck in front dims them and a dense fog hides them; the sea reflects them; the drawn lower edge and
// band colour agree with kit::aurora_curtain_height and kit::aurora_band_color at probe points; a floating-origin
// shift leaves the sky unchanged; the clock wraps without a seam; a slow camera turn keeps the curtains' light and
// the frame difference per step bounded (no crawl). The scene is the aurora night (fixture3d.h). One frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "fixture3d.h"
#include "mce/kit/aurora.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

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

template <class T>
bool same(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize i) {
    return a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2];
}

// The signed azimuth of d from the arc centre `centre` (xz unit), radians, counter-clockwise from above (the shader's).
f64 azimuth_from(vec3 d, math::vec2 centre) {
    const f64 hl = std::sqrt(f64(d.x) * d.x + f64(d.z) * d.z);
    const f64 ex = d.x / hl, ez = d.z / hl;
    return std::atan2(centre.y * ex - centre.x * ez, centre.x * ex + centre.y * ez);
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

struct Change {
    u32 above = 0;    // sky pixels (ray up) whose bytes moved
    u32 below = 0;    // pixels whose ray points at or below the horizon whose bytes moved
    u32 outside = 0;  // sky pixels outside the arc (beyond `half` radians from the centre, plus a pixel) that moved
    u32 sky = 0;
};

Change change(const Array<u8>& a, const Array<u8>& b, const Array<vec3>& dirs, const AuroraSky& arc) {
    const f64 half = std::acos(f64(arc.half_width_cos));
    Change c;
    for (usize p = 0; p < dirs.size(); ++p) {
        const bool moved = !same_px(a, b, p * 4);
        if (dirs[p].y <= 0.0f) {
            c.below += moved ? 1u : 0u;
            continue;
        }
        ++c.sky;
        c.above += moved ? 1u : 0u;
        if (moved && std::fabs(azimuth_from(dirs[p], arc.azimuth)) > half + 0.01) ++c.outside;
    }
    return c;
}

// Total luma the aurora adds over the frame without it, over the pixels selected by `pick` (all by default).
f64 flux(const Array<u8>& px, const Array<u8>& bare, const Array<vec3>& dirs, f32 min_up = -2.0f, f32 max_up = 2.0f) {
    f64 sum = 0.0;
    for (usize p = 0; p < dirs.size(); ++p)
        if (dirs[p].y > min_up && dirs[p].y <= max_up) sum += std::max(0.0, luma(px, p * 4) - luma(bare, p * 4));
    return sum;
}

// No bloom (a glow would spill across the horizon and the arc ends: not what is measured).
Array<u8> draw_aurora(Fixture3D& f, const AuroraSky* aurora, const std::function<void(Scene3D&)>& more = {},
                      f32 fog_density = 0.0f) {
    f.render(fog_density, true, [&](Scene3D& s) {
        aurora_sky_scene(s, aurora ? *aurora : AuroraSky{});  // AuroraSky{}: level 0
        if (more) more(s);
        PostFx post = s.post();
        post.bloom = false;
        s.set_post(post);
    });
    return f.pixels;
}

Array<u8> draw_sea(Fixture3D& f, const AuroraSky* aurora) {
    f.render(0.0f, true, [&](Scene3D& s) {
        water_night_scene(s, night_sky_carrier(0.5f, 0.6f, 0.0f), water_carrier(0.05f));
        if (aurora) s.set_aurora(*aurora);
        PostFx post = s.post();
        post.bloom = false;
        s.set_post(post);
    });
    return f.pixels;
}

// The kit's Aurora that the carrier `a` was made from (the fields the curtain shape reads).
kit::Aurora kit_of(const AuroraSky& a) {
    kit::Aurora k;
    k.base_height = a.base_height;
    k.height_span = a.height_span;
    k.speed = a.speed;
    k.curtain_count = u8(a.curtain_count);
    k.width_deg = f32(2.0 * std::acos(f64(a.half_width_cos)) * 180.0 / 3.14159265358979323846);
    k.low_color = a.low_color;
    k.high_color = a.high_color;
    return k;
}

}  // namespace

TEST_CASE("render3d_gl: an aurora at level 0, disabled, with a NaN level or on the Low tier is the plain frame") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_aurora(f, nullptr);
    AuroraSky zero = aurora_carrier(0.0f, 33.0f);
    CHECK(same(draw_aurora(f, &zero), none));
    AuroraSky off = aurora_carrier(2.0f);
    off.enabled = false;
    CHECK(same(draw_aurora(f, &off), none));
    AuroraSky nan = aurora_carrier(std::nanf(""));  // the upload clamps again: a NaN level is off
    CHECK(same(draw_aurora(f, &nan), none));
    const AuroraSky low = apply_quality(quality_settings(QualityTier::Low), aurora_carrier(2.0f));
    CHECK(low.level == 0.0f);
    CHECK(same(draw_aurora(f, &low), none));
    const AuroraSky high = apply_quality(quality_settings(QualityTier::High), aurora_carrier(1.0f));
    CHECK(high.level == 1.0f);
    CHECK_FALSE(same(draw_aurora(f, &high), none));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the curtains change only the sky above the horizon and inside the arc; a higher level is "
          "brighter") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_aurora(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    // A 30 degree arc: both ends inside the 58 degree wide frame.
    AuroraSky narrow = aurora_carrier(1.0f);
    narrow.half_width_cos = 0.96592583f;  // cos 15 degrees
    const Change cn = change(none, draw_aurora(f, &narrow), dirs, narrow);
    const AuroraSky wide = aurora_carrier(1.0f);
    const Array<u8> one = draw_aurora(f, &wide);
    const Change cw = change(none, one, dirs, wide);
    MESSAGE("sky pixels " << cw.sky << "; changed by a 30 / 70 degree arc: " << cn.above << " / " << cw.above
                          << ", outside the arc " << cn.outside << " / " << cw.outside << ", at or below the horizon "
                          << cn.below << " / " << cw.below);
    REQUIRE(cw.sky > 4000);
    CHECK(cn.below == 0);
    CHECK(cw.below == 0);
    CHECK(cn.outside == 0);
    CHECK(cn.above > 300);
    CHECK(cw.above > cn.above);
    // Brighter at a higher level, everywhere it shows.
    const AuroraSky two = aurora_carrier(2.0f), four = aurora_carrier(4.0f);
    const f64 f1 = flux(one, none, dirs), f2 = flux(draw_aurora(f, &two), none, dirs),
              f4 = flux(draw_aurora(f, &four), none, dirs);
    MESSAGE("aurora flux at level 1 / 2 / 4: " << f1 << " / " << f2 << " / " << f4);
    CHECK(f1 > 0.0);
    CHECK(f2 > 1.3 * f1);
    CHECK(f4 > 1.3 * f2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the drawn curtains follow kit::aurora_curtain_height and kit::aurora_band_color") {
    // Probe points against the kit's helpers: pure green at the base and pure blue at the top, so the blue share of
    // the light the aurora adds is the band colour's mix; a 180 degree arc (no end fade in the frame, 10 pixel ray
    // cells) at level 4. Per pixel column: the lowest sky pixel the aurora lights must sit on the kit's lower edge at
    // that column's arc parameter (within the edge's smoothstep and the pixel's height); per lit pixel well inside the
    // curtain: the blue share must be the kit's band mix at that pixel's relative height.
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_aurora(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    for (const f32 time : {12.25f, 40.5f}) {
        AuroraSky a = aurora_carrier(4.0f, time);
        a.half_width_cos = 0.0f;
        a.low_color = {0.0f, 1.0f, 0.0f};
        a.high_color = {0.0f, 0.0f, 1.0f};
        const kit::Aurora k = kit_of(a);
        const f64 half = std::acos(f64(a.half_width_cos));
        const Array<u8> px = draw_aurora(f, &a);
        u32 columns = 0, on_edge = 0, colour_n = 0, colour_ok = 0;
        f64 worst_edge = 0.0, worst_colour = 0.0;
        for (u32 x = 4; x + 4 < kW; x += 3) {
            // the lowest lit sky pixel of the column (rows run top first)
            i32 found = -1;
            for (i32 y = i32(kH) - 1; y >= 0; --y) {
                const usize p = usize(y) * kW + x;
                if (dirs[p].y <= 0.0f) continue;
                if (i32(px[p * 4 + 1]) - i32(none[p * 4 + 1]) > 6) {
                    found = y;
                    break;
                }
            }
            const usize p0 = usize(std::max(found, 0)) * kW + x;
            const vec3 d = dirs[p0];
            const f32 u = f32(0.5 + azimuth_from(d, a.azimuth) / (2.0 * half));
            const math::vec2 edges = kit::aurora_curtain_height(k, u, time);
            ++columns;
            if (found < 0) continue;
            // the pixel's height in sines of the elevation (the row above minus this one)
            const f64 pixy = found > 0 ? f64(dirs[p0 - kW].y - d.y) : 0.01;
            const f64 err = f64(d.y) - f64(edges.x);  // >= -pixy / 2: the edge can light the pixel from below its centre
            worst_edge = std::max(worst_edge, std::fabs(err));
            if (err > -0.5 * pixy - 1e-3 && err < 0.08 * f64(edges.y - edges.x) + 1.5 * pixy) ++on_edge;
            // the band colour up the column, where the aurora adds enough light to measure its blue share
            for (i32 y = found; y >= 0; --y) {
                const usize p = usize(y) * kW + x;
                const f64 h01 = (f64(dirs[p].y) - f64(edges.x)) / f64(edges.y - edges.x);
                if (h01 < 0.15 || h01 > 0.9) continue;
                const f64 g = linear_of(px[p * 4 + 1]) - linear_of(none[p * 4 + 1]);
                const f64 b = linear_of(px[p * 4 + 2]) - linear_of(none[p * 4 + 2]);
                if (g + b < 0.03) continue;
                const math::vec3 band = kit::aurora_band_color(k, f32(h01));
                const f64 want = f64(band.z) / f64(band.y + band.z);
                const f64 got = b / (g + b);
                ++colour_n;
                worst_colour = std::max(worst_colour, std::fabs(got - want));
                if (std::fabs(got - want) < 0.12) ++colour_ok;
            }
        }
        MESSAGE("t " << time << ": lower edge on the kit's in " << on_edge << " of " << columns
                     << " columns (worst |d.y - lower| " << worst_edge << "); band colour within 0.12 in " << colour_ok
                     << " of " << colour_n << " pixels (worst " << worst_colour << ")");
        CHECK(on_edge == columns);
        REQUIRE(colour_n > 50);
        CHECK(f64(colour_ok) >= 0.95 * f64(colour_n));
    }
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the lower band of the curtains is greener than the top") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_aurora(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const AuroraSky a = aurora_carrier(4.0f);
    const Array<u8> px = draw_aurora(f, &a);
    // Linear light added by the aurora in a low band (just over the base 0.18 .. 0.26) and a high one (0.42 .. 0.55).
    f64 low_g = 0.0, low_rb = 0.0, high_g = 0.0, high_rb = 0.0;
    for (usize p = 0; p < dirs.size(); ++p) {
        const f32 y = dirs[p].y;
        const bool lo = y > 0.2f && y < 0.26f, hi = y > 0.42f && y < 0.55f;
        if (!lo && !hi) continue;
        const f64 r = std::max(0.0, linear_of(px[p * 4]) - linear_of(none[p * 4]));
        const f64 g = std::max(0.0, linear_of(px[p * 4 + 1]) - linear_of(none[p * 4 + 1]));
        const f64 b = std::max(0.0, linear_of(px[p * 4 + 2]) - linear_of(none[p * 4 + 2]));
        (lo ? low_g : high_g) += g;
        (lo ? low_rb : high_rb) += r + b;
    }
    MESSAGE("green / (red + blue) of the added light: low band " << low_g / std::max(low_rb, 1e-9) << ", high band "
                                                                 << high_g / std::max(high_rb, 1e-9));
    REQUIRE(low_g > 0.0);
    REQUIRE(high_g + high_rb > 0.0);
    CHECK(low_g / std::max(low_rb, 1e-9) > 1.5 * (high_g / std::max(high_rb, 1e-9)));
    CHECK(low_g > high_g);  // brightest at the base
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a cloud deck in front dims the curtains and a dense fog hides them") {
    Fixture3D f;
    cloud_sky_camera(f);
    const AuroraSky a = aurora_carrier(2.0f);
    const Array<u8> none = draw_aurora(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const f64 clear = flux(draw_aurora(f, &a), none, dirs, 0.25f);
    // Over the rays that climb more than 0.25, above the band where the deck fades into the horizon haze.
    Clouds thin, deck;
    thin.coverage = deck.coverage = 1.0f;
    deck.density = 1.0f;
    const auto broken = [&](Scene3D& s) { s.set_clouds(thin); };
    const auto overcast = [&](Scene3D& s) { s.set_clouds(deck); };
    const f64 f_thin = flux(draw_aurora(f, &a, broken), draw_aurora(f, nullptr, broken), dirs, 0.25f);
    const f64 f_deck = flux(draw_aurora(f, &a, overcast), draw_aurora(f, nullptr, overcast), dirs, 0.25f);
    // A dense volumetric fog in front of the whole sky.
    const auto dense = [](Scene3D& s) {
        Fog fog = s.fog();
        fog.color = {0.02f, 0.025f, 0.035f};
        s.set_fog(fog);
        Volumetrics v;
        v.enabled = true;
        v.density = 0.2f;
        v.height_falloff = 0.0f;
        s.set_volumetrics(v);
    };
    const f64 f_fog = flux(draw_aurora(f, &a, dense), draw_aurora(f, nullptr, dense), dirs);
    const f64 clear_all = flux(draw_aurora(f, &a), none, dirs);
    MESSAGE("aurora flux above 0.25: clear " << clear << ", thin deck " << f_thin << ", thick deck " << f_deck
                                             << "; whole sky: clear " << clear_all << ", dense fog " << f_fog);
    REQUIRE(clear > 0.0);
    CHECK(f_thin < 0.6 * clear);
    CHECK(f_deck < 0.2 * clear);
    CHECK(f_fog < 0.05 * clear_all);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the sea reflects the curtains, and the sea without them is unchanged") {
    Fixture3D f;
    water_camera(f);
    const Array<u8> none = draw_sea(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    AuroraSky zero = aurora_carrier(0.0f);
    CHECK(same(draw_sea(f, &zero), none));
    const AuroraSky low = apply_quality(quality_settings(QualityTier::Low), aurora_carrier(2.0f));
    CHECK(same(draw_sea(f, &low), none));
    const AuroraSky a = aurora_carrier(2.0f);
    const Array<u8> lit = draw_sea(f, &a);
    u32 sky = 0, sea = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (same_px(lit, none, p * 4)) continue;
        (dirs[p].y > 0.0f ? sky : sea) += 1u;
    }
    const f64 sea_flux = flux(lit, none, dirs, -2.0f, -0.02f);
    MESSAGE("pixels the aurora changes: sky " << sky << ", sea " << sea << " (sea flux " << sea_flux << ")");
    CHECK(sky > 100);
    CHECK(sea > 200);
    CHECK(sea_flux > 0.0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the curtains stay put across an origin shift and wrap their clock without a seam") {
    Fixture3D f;
    cloud_sky_camera(f);
    const AuroraSky a = aurora_carrier(1.0f, 12.25f);
    const Array<u8> home = draw_aurora(f, &a);
    const Array<u8> none = draw_aurora(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    // A floating-origin rebase: the layer is a function of the view direction alone.
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    CHECK(same(draw_aurora(f, &a, [&](Scene3D& s) { s.set_origin_shift(shift); }), home));
    // The clock is wrapped to 64 s on the CPU and every rate is a whole number of cycles in it: whole periods later
    // (up to ten minutes, and three days) the frame is the same bit for bit.
    for (const f32 periods : {1.0f, 5.0f, 10.0f, 4050.0f}) {
        AuroraSky later = a;
        later.time = a.time + 64.0f * periods;
        CHECK(same(draw_aurora(f, &later), home));
    }
    // Ten simulated minutes on: the clock the shader sees is the wrapped one (bit for bit what the wrapped time
    // draws), and the curtains are still there with about the same light (they sway and shimmer, they do not drift).
    AuroraSky ten = a, wrapped = a;
    ten.time = a.time + 600.0f;
    wrapped.time = f32(std::fmod(f64(a.time) + 600.0, 64.0));
    const Array<u8> later = draw_aurora(f, &ten);
    CHECK(same(later, draw_aurora(f, &wrapped)));
    CHECK_FALSE(same(later, home));
    const f64 f_home = flux(home, none, dirs), f_later = flux(later, none, dirs);
    MESSAGE("aurora flux at 12.25 s / 612.25 s: " << f_home << " / " << f_later);
    REQUIRE(f_home > 0.0);
    CHECK(f_later > 0.7 * f_home);
    CHECK(f_later < 1.3 * f_home);
    // Speed 0: any clock draws the same frame.
    AuroraSky still = a, still_later = a;
    still.speed = still_later.speed = 0.0f;
    still_later.time = 47.5f;
    CHECK(same(draw_aurora(f, &still), draw_aurora(f, &still_later)));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the curtains keep their light under a slow camera turn (no crawl)") {
    // A one-degree turn in tenth-degree steps (about a quarter of a pixel each). The curtains' total light must stay
    // put, and the light they add to a pixel may move only as far as a quarter-pixel shift of a smooth pattern moves
    // it: the mean change per sky pixel between neighbouring steps, against the mean light the aurora adds per lit
    // pixel. Striations sized under a pixel would crawl: their pixels would flip between bright and dark ray cells.
    Fixture3D f;
    cloud_sky_camera(f);
    // The default curtains (ray cells of about 4 pixels) and six curtains on a 30 degree arc (96 cells of under a
    // pixel: the rays fade to their mean).
    AuroraSky fine = aurora_carrier(2.0f);
    fine.curtain_count = 6;
    fine.half_width_cos = 0.96592583f;  // cos 15 degrees
    for (const AuroraSky& a : {aurora_carrier(2.0f), fine}) {
    f64 lo = 1e30, hi = 0.0, worst_step = 0.0, mean_lit = 0.0;
    Array<f64> prev;
    for (u32 i = 0; i <= 10; ++i) {
        f.yaw_deg = 0.1f * f32(i);
        const Array<u8> bare = draw_aurora(f, nullptr);
        const Array<vec3> dirs = ray_dirs(f);
        const Array<u8> px = draw_aurora(f, &a);
        Array<f64> added;  // the luma the aurora adds per pixel (0 at or below the horizon)
        added.reserve(dirs.size());
        f64 fl = 0.0;
        u32 lit = 0;
        for (usize p = 0; p < dirs.size(); ++p) {
            const f64 v = dirs[p].y > 0.0f ? luma(px, p * 4) - luma(bare, p * 4) : 0.0;
            added.push_back(v);
            fl += std::max(v, 0.0);
            lit += v > 1.0 ? 1u : 0u;
        }
        lo = std::min(lo, fl);
        hi = std::max(hi, fl);
        if (i == 0) mean_lit = fl / f64(std::max(lit, 1u));
        if (!prev.empty()) {
            f64 sum = 0.0;
            u32 n = 0;
            for (usize p = 0; p < dirs.size(); ++p) {
                if (dirs[p].y <= 0.0f) continue;
                sum += std::fabs(added[p] - prev[p]);
                ++n;
            }
            worst_step = std::max(worst_step, sum / f64(std::max(n, 1u)));
        }
        prev = std::move(added);
    }
    f.yaw_deg = 0.0f;
    MESSAGE(a.curtain_count << " curtains: aurora flux over a one-degree turn " << lo << " .. " << hi
                            << "; worst mean change of the added luma per sky pixel and step " << worst_step
                            << " (mean added luma per lit pixel " << mean_lit << ")");
    REQUIRE(lo > 0.0);
    CHECK(hi / lo < 1.01);  // measured 1.001 for both; 1.003 and 1.016 without the footprint filter
    CHECK(worst_step < 0.015 * mean_lit);  // measured 0.011 and 0.0055; 0.013 and 0.018 without the filter
    }
    CHECK(f.device->validation_error_count() == 0);
}
