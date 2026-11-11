// M-CIRRUS 53b against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the high cirrus streaks
// in the sky pass (p_cirrus / p_cirrus2, cirrus_layer in MCE_GLSL_SKY_LAYERS) and in the water's reflection
// (water_sky). Coverage 0, `enabled` false, a NaN coverage and the Low tier draw the frame without the layer bit for
// bit; the streaks change only the sky above the horizon; the opacity stays at or under 0.5; they are longer along the
// wind than across it; a thick low deck in front hides them; they are brighter toward the sun; they are still lit (and
// warm) at a sun elevation where the low layer has gone dark; the sea reflects them and the sea without them is
// unchanged; a floating-origin shift leaves the sky unchanged; the drift wraps without a seam; and the layer's GLSL
// alone agrees with the CPU twin (mce/render/cirrus.h) on a grid of directions. The scenes are the cirrus day and dusk
// (fixture3d.h).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

#include "fixture3d.h"
#include "mce/render/cirrus.h"
#include "mce/render/cloud_shadow.h"
#include "mce/render/quality.h"
#include "shaders3d.h"  // render's 3D shaders: the cirrus layer's text runs alone

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

struct Change {
    u32 above = 0;   // pixels whose ray climbs more than 0.02 whose bytes moved
    u32 below = 0;   // pixels whose ray climbs 0.02 or less (the layer's limit) whose bytes moved
    u32 sky = 0;     // pixels whose ray climbs more than 0.02
    f64 mean = 0.0;  // mean signed luma difference over the moved sky pixels
};

Change change(const Array<u8>& a, const Array<u8>& b, const Array<vec3>& dirs) {
    Change c;
    for (usize p = 0; p < dirs.size(); ++p) {
        const bool moved = !same_px(a, b, p * 4);
        if (dirs[p].y <= kCirrusMinUp) {
            c.below += moved ? 1u : 0u;
            continue;
        }
        ++c.sky;
        if (!moved) continue;
        ++c.above;
        c.mean += luma(b, p * 4) - luma(a, p * 4);
    }
    if (c.above) c.mean /= f64(c.above);
    return c;
}

// No bloom and no FXAA: each carries a changed pixel to its neighbours (across the horizon too).
void plain_post(Scene3D& s) {
    PostFx post = s.post();
    post.bloom = false;
    post.fxaa = false;
    s.set_post(post);
}

// The cirrus day (cirrus null: none) with the low layer `clouds`.
Array<u8> draw_day(Fixture3D& f, const Cirrus* cirrus, const Clouds& clouds = Clouds{}, f32 time = 30.0f,
                   const std::function<void(Scene3D&)>& more = {}) {
    f.render(0.0f, true, [&](Scene3D& s) {
        cirrus_sky_scene(s, cirrus ? *cirrus : Cirrus{}, clouds, time);  // Cirrus{}: coverage 0
        if (more) more(s);
        plain_post(s);
    });
    return f.pixels;
}

Array<u8> draw_dusk(Fixture3D& f, const Cirrus* cirrus, const Clouds& clouds = Clouds{}) {
    f.render(0.0f, true, [&](Scene3D& s) {
        cirrus_dusk_scene(s, cirrus ? *cirrus : Cirrus{}, clouds);
        plain_post(s);
    });
    return f.pixels;
}

Cirrus with_wind(Cirrus c, f32 x, f32 z) {
    c.wind_dir = {x, z};
    return c;
}

// The correlation of the layer's luma change (frame `a` over frame `bare`) with itself shifted by `lag` pixels along x
// (dx 1) or y (dy 1), over the sky rows whose rays climb more than `min_up`.
f64 autocorr(const Array<u8>& a, const Array<u8>& bare, const Array<vec3>& dirs, i32 dx, i32 dy, f32 min_up) {
    Array<f64> v;
    v.resize(usize(kW) * kH);
    f64 mean = 0.0;
    u32 n = 0;
    for (usize p = 0; p < v.size(); ++p) {
        v[p] = luma(a, p * 4) - luma(bare, p * 4);
        if (dirs[p].y > min_up) {
            mean += v[p];
            ++n;
        }
    }
    mean /= f64(std::max(n, 1u));
    f64 num = 0.0, den = 0.0;
    for (i32 y = 0; y < i32(kH); ++y)
        for (i32 x = 0; x < i32(kW); ++x) {
            const i32 x2 = x + dx, y2 = y + dy;
            if (x2 >= i32(kW) || y2 >= i32(kH)) continue;
            const usize p = usize(y) * kW + usize(x), q = usize(y2) * kW + usize(x2);
            if (dirs[p].y <= min_up || dirs[q].y <= min_up) continue;
            num += (v[p] - mean) * (v[q] - mean);
            den += (v[p] - mean) * (v[p] - mean);
        }
    return den > 0.0 ? num / den : 0.0;
}

}  // namespace

TEST_CASE("render3d_gl: cirrus at coverage 0, disabled, NaN or on the Low tier draws the plain frame bit for bit") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_day(f, nullptr);
    const Cirrus zero = with_wind(cirrus_carrier(0.0f), -0.3f, 0.8f);
    CHECK(same(draw_day(f, &zero), none));
    Cirrus off = cirrus_carrier(0.9f);
    off.enabled = false;
    CHECK(same(draw_day(f, &off), none));
    const Cirrus nan = cirrus_carrier(std::nanf(""));  // the upload clamps again: a NaN coverage is off
    CHECK(same(draw_day(f, &nan), none));
    const Cirrus low = apply_quality(quality_settings(QualityTier::Low), cirrus_carrier(0.9f));
    CHECK(low.coverage == 0.0f);
    CHECK(same(draw_day(f, &low), none));
    const Cirrus high = apply_quality(quality_settings(QualityTier::High), cirrus_carrier(0.9f));
    CHECK(high.coverage == 0.9f);
    CHECK_FALSE(same(draw_day(f, &high), none));
    // The same with the low layer on: the cirrus off is the low layer's frame bit for bit.
    const Clouds low_deck = cloud_sky_clouds(0.4f);
    const Array<u8> deck = draw_day(f, nullptr, low_deck);
    CHECK(same(draw_day(f, &zero, low_deck), deck));
    CHECK(same(draw_day(f, &low, low_deck), deck));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE(
    "render3d_gl: cirrus changes only the sky above the horizon, more coverage covers more, the opacity stays "
    "under 0.5") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_day(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    Change c[3];
    const f32 cov[3] = {0.3f, 0.6f, 1.0f};
    for (u32 i = 0; i < 3; ++i) {
        const Cirrus ci = cirrus_carrier(cov[i]);
        c[i] = change(none, draw_day(f, &ci), dirs);
    }
    MESSAGE("sky pixels " << c[0].sky << "; moved at coverage 0.3 / 0.6 / 1: " << c[0].above << " / " << c[1].above
                          << " / " << c[2].above << " (mean dluma " << c[0].mean << " / " << c[1].mean << " / "
                          << c[2].mean << "); at or under the limit: " << c[0].below << " / " << c[1].below << " / "
                          << c[2].below);
    REQUIRE(c[0].sky > 4000);
    for (const Change& k : c) CHECK(k.below == 0);
    CHECK(c[0].above > 0);
    CHECK(c[1].above > c[0].above);
    CHECK(c[2].above > c[1].above);
    CHECK(f64(c[0].above) < 0.5 * f64(c[0].sky));  // fair weather: broken streaks
    // The opacity cap: a black layer (no light, no ambient) at full coverage and density keeps at least half of the sky
    // behind it, in linear light.
    Cirrus dark = cirrus_carrier(1.0f);
    dark.density = 1.0f;
    const auto black = [](Scene3D& s) {
        DirectionalLight l = s.light();
        l.intensity = 0.0f;
        l.ambient_sky = {0.0f, 0.0f, 0.0f};
        s.set_light(l);
    };
    const Array<u8> bare = draw_day(f, nullptr, Clouds{}, 30.0f, black);
    const Array<u8> veiled = draw_day(f, &dark, Clouds{}, 30.0f, black);
    f64 least = 1.0;
    u32 dimmed = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (dirs[p].y <= 0.05f) continue;
        for (u32 ch = 0; ch < 3; ++ch) {
            const u8 b = bare[p * 4 + ch];
            if (b < 40) continue;
            const f64 r = linear_of(veiled[p * 4 + ch]) / linear_of(b);
            least = std::min(least, r);
            dimmed += r < 0.98 ? 1u : 0u;
        }
    }
    MESSAGE("black layer at full cover: least linear ratio " << least << " over " << dimmed << " dimmed channels");
    CHECK(dimmed > 3000);
    CHECK(least > 0.47);  // 0.5 and the 8-bit steps
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the cirrus streaks are longer along the wind than across it") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_day(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const auto ratio = [&](const Cirrus& c, i32 lag, f64& h, f64& v) {
        const Array<u8> px = draw_day(f, &c);
        h = autocorr(px, none, dirs, lag, 0, 0.12f);
        v = autocorr(px, none, dirs, 0, lag, 0.12f);
        return h / std::max(v, 1e-3);
    };
    f64 hx, vx, hz, vz, h1, v1, h16, v16;
    // The wind across the view (+x): the streaks lie across the frame. Along the view (-z): toward the vanishing point.
    const f64 rx = ratio(with_wind(cirrus_carrier(0.6f), 1.0f, 0.0f), 4, hx, vx);
    const f64 rz = ratio(with_wind(cirrus_carrier(0.6f), 0.0f, -1.0f), 4, hz, vz);
    // A longer stretch keeps the pattern alike further along the wind (the fibres are long at any stretch: compare the
    // broad patches, 16 pixels apart).
    Cirrus round = with_wind(cirrus_carrier(0.6f), 1.0f, 0.0f), combed = round;
    round.stretch = 1.0f;
    combed.stretch = 16.0f;
    const f64 r1 = ratio(round, 16, h1, v1), r16 = ratio(combed, 16, h16, v16);
    MESSAGE("autocorrelation across / down the frame at 4 px: wind +x "
            << hx << " / " << vx << " (" << rx << "), wind -z " << hz << " / " << vz << " (" << rz
            << "); at 16 px, wind +x, stretch 1 " << h1 << " / " << v1 << " (" << r1 << "), stretch 16 " << h16 << " / "
            << v16 << " (" << r16 << ")");
    CHECK(hx > vx);
    CHECK(rx > 1.5 * rz);
    CHECK(h16 > h1);
    CHECK(r16 > r1);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a thick low deck in front hides the cirrus; the streaks are brighter toward the sun") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<vec3> dirs = [&] {
        draw_day(f, nullptr);
        return ray_dirs(f);
    }();
    Clouds deck = cloud_sky_clouds(1.0f);
    deck.density = 1.0f;
    const Cirrus ci = cirrus_carrier(0.8f);
    const Array<u8> deck_only = draw_day(f, nullptr, deck), both = draw_day(f, &ci, deck);
    const Array<u8> none = draw_day(f, nullptr), cirrus_only = draw_day(f, &ci);
    // Where the low layer is opaque: its CPU twin (cloud_shadow.h: the sky's own noise and mask) at the pixel's point
    // on its plane gives a full opacity, and its horizon fade (smoothstep(0.03, 0.25, d.y)) is over.
    CloudDensityParams dp;
    dp.coverage = deck.coverage;
    dp.density = deck.density;
    dp.scale_m = deck.scale_m;
    dp.drift_cells = cloud_shadow_drift(30.0f, deck.speed, deck.scale_m, deck.wind_dir);
    u32 shown = 0, hidden_moved = 0, checked = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (dirs[p].y <= 0.3f) continue;
        const f32 k = deck.height_m / dirs[p].y;
        if (cloud_density(math::vec2{dirs[p].x * k, dirs[p].z * k}, dp) < 0.9999f) continue;
        ++checked;
        shown += same_px(none, cirrus_only, p * 4) ? 0u : 1u;
        for (u32 ch = 0; ch < 3; ++ch)
            hidden_moved += std::abs(i32(both[p * 4 + ch]) - i32(deck_only[p * 4 + ch])) > 1 ? 1u : 0u;
    }
    MESSAGE("over " << checked << " high sky pixels: the cirrus alone moved " << shown
                    << ", behind a full low deck channels moved by more than 1: " << hidden_moved);
    REQUIRE(checked > 300);
    CHECK(shown > checked / 4);
    CHECK(hidden_moved == 0);

    // The sun on the camera's right front (the scene's) and the same elevation behind the camera: the forward scatter
    // and the side term light the streaks more toward the sun.
    const auto behind = [](Scene3D& s) {
        DirectionalLight l = s.light();
        l.direction = {-0.45f, -0.55f, -0.7f};
        s.set_light(l);
    };
    const Array<u8> none_back = draw_day(f, nullptr, Clouds{}, 30.0f, behind);
    const Array<u8> cirrus_back = draw_day(f, &ci, Clouds{}, 30.0f, behind);
    f64 front = 0.0, back = 0.0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (dirs[p].y <= 0.05f) continue;
        front += luma(cirrus_only, p * 4) - luma(none, p * 4);
        back += luma(cirrus_back, p * 4) - luma(none_back, p * 4);
    }
    MESSAGE("luma the streaks add with the sun in front / behind: " << front << " / " << back);
    CHECK(front > 0.0);
    CHECK(front > 1.3 * back);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: at dusk the cirrus is still lit, warm, while the low layer has gone dark") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_dusk(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const Cirrus ci = cirrus_carrier(0.7f);
    const Array<u8> streaks = draw_dusk(f, &ci);
    const Array<u8> deck = draw_dusk(f, nullptr, cloud_sky_clouds(0.7f));
    // Where each layer covers the sky most (the pixels it moved most), its colour against the sky behind it.
    f64 c_dl = 0.0, c_dr = 0.0, c_db = 0.0, d_dl = 0.0;
    u32 c_n = 0, d_n = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (dirs[p].y <= 0.1f) continue;
        const f64 dc = luma(streaks, p * 4) - luma(none, p * 4);
        if (std::fabs(dc) > 4.0) {
            c_dl += dc;
            c_dr += f64(streaks[p * 4]) - f64(none[p * 4]);
            c_db += f64(streaks[p * 4 + 2]) - f64(none[p * 4 + 2]);
            ++c_n;
        }
        const f64 dd = luma(deck, p * 4) - luma(none, p * 4);
        if (std::fabs(dd) > 4.0) {
            d_dl += dd;
            ++d_n;
        }
    }
    MESSAGE("dusk: the cirrus moved " << c_n << " sky pixels, mean dluma " << c_dl / std::max(c_n, 1u) << " (dR "
                                      << c_dr / std::max(c_n, 1u) << ", dB " << c_db / std::max(c_n, 1u)
                                      << "); the low layer moved " << d_n << ", mean dluma "
                                      << d_dl / std::max(d_n, 1u));
    REQUIRE(c_n > 300);
    REQUIRE(d_n > 300);
    CHECK(c_dl > 0.0);   // the streaks are brighter than the sky behind them: still lit
    CHECK(c_dr > c_db);  // warm: more red than blue added
    CHECK(d_dl < 0.0);   // the low deck darkens the sky: its light is gone
    CHECK(c_dl / f64(c_n) > d_dl / f64(d_n) + 8.0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the sea reflects the cirrus, and the sea without it is unchanged") {
    Fixture3D f;
    water_camera(f);
    const auto sea = [&](const Cirrus* c) {
        f.render(0.0f, true, [&](Scene3D& s) {
            water_day_scene(s, water_carrier(0.0f));
            if (c) s.set_cirrus(*c);
            plain_post(s);
        });
        return f.pixels;
    };
    const Array<u8> none = sea(nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const Cirrus zero = cirrus_carrier(0.0f);
    CHECK(same(sea(&zero), none));
    Cirrus ci = cirrus_carrier(0.8f);
    ci.density = 1.0f;
    const Array<u8> with = sea(&ci);
    u32 sky = 0, water = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (same_px(none, with, p * 4)) continue;
        if (dirs[p].y > 0.0f)
            ++sky;
        else
            ++water;
    }
    MESSAGE("calm sea: pixels the cirrus moved above / under the horizon: " << sky << " / " << water);
    CHECK(sky > 200);
    CHECK(water > 100);  // the reflection
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the cirrus stays put across an origin shift, drifts with the clock, and wraps without a seam") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Cirrus ci = cirrus_carrier(0.6f);
    const Array<u8> home = draw_day(f, &ci, Clouds{}, 20.0f);
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    const auto rebase = [&](Scene3D& s) { s.set_origin_shift(shift); };
    CHECK(same(draw_day(f, &ci, Clouds{}, 20.0f, rebase), home));  // the layer is not keyed to the origin
    // The drift: the scene clock moves the streaks, speed 0 holds them.
    CHECK_FALSE(same(draw_day(f, &ci, Clouds{}, 500.0f), home));
    Cirrus still = ci;
    still.speed = 0.0f;
    CHECK(same(draw_day(f, &still, Clouds{}, 20.0f), draw_day(f, &still, Clouds{}, 900.0f)));
    // One whole period later (64 cells along the wind: 64 * 3000 * 6 / (2 * 10) = 57600 s) the drift is the same f32,
    // so the frame is the same bit for bit.
    CHECK(cirrus_drift(20.0f, 2.0f, 3000.0f, 6.0f) == cirrus_drift(57620.0f, 2.0f, 3000.0f, 6.0f));
    CHECK(same(draw_day(f, &ci, Clouds{}, 57620.0f), home));
    CHECK(f.device->validation_error_count() == 0);
}

// ---- the layer's GLSL alone against the CPU twin
// ----------------------------------------------------------------------

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

// The test's own PassU: the names the layer's text reads. t_sun: xyz toward the sun; t_bg: the sky colour behind.
constexpr const char* kTwinHead = R"(#version 450 core
layout(std140, binding = 1) uniform PassU {
    vec4 p_light_color;
    vec4 p_amb_sky;
    vec4 p_cirrus;
    vec4 p_cirrus2;
    vec4 t_sun;
    vec4 t_bg;
};
)";
// Texel x of the 4 N x 1 target: direction x / 4 (u_dirs: xyz the direction, w the radians per pixel), output x % 4
// (0 the opacity, 1..3 cirrus_layer's rgb), its float bits as four bytes.
constexpr const char* kTwinMain = R"(
layout(binding = 6) uniform sampler2D u_dirs;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    int x = int(gl_FragCoord.x);
    vec4 dp = texelFetch(u_dirs, ivec2(x / 4, 0), 0);
    vec3 d = dp.xyz;
    int k = x % 4;
    float v;
    if (k == 0) {
        v = cirrus_alpha(d, dp.w);
    } else {
        vec3 c = cirrus_layer(t_bg.rgb, d, t_sun.xyz, dot(d, t_sun.xyz), dp.w);
        v = k == 1 ? c.r : (k == 2 ? c.g : c.b);
    }
    o_color = unpackUnorm4x8(floatBitsToUint(v));
}
)";

struct TwinBlock {
    f32 light[4], amb[4], cirrus[4], cirrus2[4], sun[4], bg[4];
};

// The layer's text over directions `dirs` with pixel footprints `pix` (each exact in half precision): per direction the
// opacity and the composited rgb, 4 floats.
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
    dd.debug_name = "test.cirrus.dirs";
    const rhi::TextureHandle tex =
        f.device->create_texture(dd, Span<const byte>(reinterpret_cast<const byte*>(data.data()), data.size() * 2));
    const std::string fs = std::string(kTwinHead) + shaders3d::kCirrusLayerGLSL + kTwinMain;
    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = {shaders3d::kFullscreenVS, "fullscreen.vert"};
    pd.fragment_shader = {fs.c_str(), "test.cirrus_twin.frag"};
    pd.state.depth = {};
    pd.debug_name = "test.cirrus_twin";
    auto pipe = f.device->create_pipeline(pd);
    REQUIRE(pipe.has_value());
    rhi::TextureDesc td;
    td.size = {w, 1};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.cirrus_twin.out";
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
    p.debug_name = "test.cirrus_twin";
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

}  // namespace

TEST_CASE("render3d_gl: the cirrus GLSL agrees with the CPU twin on a grid of directions") {
    Fixture3D f;
    // Directions over the upper hemisphere (and a few under the limit) with pixel footprints from 0.5 to 12
    // milliradians (the fibre fade from full detail to none), each rounded to half precision so the GPU reads the very
    // values the CPU evaluates.
    Array<vec3> dirs;
    Array<f32> pix;
    for (u32 i = 0; i < 24; ++i)
        for (u32 j = 0; j < 40; ++j) {
            const f32 el = -0.05f + 1.45f * (f32(i) + 0.37f) / 24.0f;  // -3 to 80 degrees
            const f32 az = 6.2831853f * (f32(j) + 0.11f * f32(i)) / 40.0f;
            const vec3 d{std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
            dirs.push_back(vec3{from_half(to_half(d.x)), from_half(to_half(d.y)), from_half(to_half(d.z))});
            pix.push_back(from_half(to_half(0.0005f + 0.0115f * f32((i * 7 + j * 3) % 23) / 22.0f)));
        }
    struct Case {
        Cirrus c;
        f32 time;
        vec3 toward, light, amb;
    };
    const auto carrier = [](f32 cov, f32 dens, f32 scale, f32 stretch, f32 speed, math::vec2 wind, f32 h) {
        Cirrus c;
        c.coverage = cov;
        c.density = dens;
        c.scale_m = scale;
        c.stretch = stretch;
        c.speed = speed;
        c.wind_dir = wind;
        c.height_m = h;
        return c;
    };
    const Case cases[] = {
        {carrier(0.55f, 0.35f, 3000.0f, 6.0f, 2.0f, {0.8f, 0.6f}, 9000.0f),
         30.0f,
         {0.45f, 0.55f, -0.7f},
         {2.0f, 1.92f, 1.8f},
         {0.35f, 0.42f, 0.55f}},
        {carrier(1.0f, 1.0f, 800.0f, 12.0f, 7.5f, {-0.3f, 1.0f}, 15000.0f),
         12345.5f,
         {0.33f, -0.05f, -0.94f},
         {0.85f, 0.42f, 0.23f},
         {0.2f, 0.17f, 0.24f}},
        {carrier(0.3f, 0.7f, 20000.0f, 1.0f, 0.5f, {0.0f, -1.0f}, 2500.0f),
         4.0e6f,
         {-0.1f, 0.02f, 0.99f},
         {1.0f, 0.6f, 0.4f},
         {0.05f, 0.05f, 0.08f}},
        {carrier(0.8f, 0.5f, 3000.0f, 16.0f, 10.0f, {1.0f, 0.0f}, 30000.0f),
         777.0f,
         {0.0f, 1.0f, 0.0f},
         {3.0f, 3.0f, 3.0f},
         {0.4f, 0.45f, 0.6f}},
    };
    const vec3 bg{0.2f, 0.4f, 0.8f};
    usize compared = 0, covered = 0, fibre_faded = 0, fibre_full = 0;
    f32 worst_a = 0.0f, worst_c = 0.0f;
    u32 off_a = 0;
    for (const Case& k : cases) {
        const CirrusParams p = cirrus_shader_params(k.c, k.time);
        REQUIRE(p.coverage > 0.0f);
        const vec3 sun = math::normalize(k.toward);
        TwinBlock u{};
        const f32 blk[6][4] = {{k.light.x, k.light.y, k.light.z, 0.0f},
                               {k.amb.x, k.amb.y, k.amb.z, 0.0f},
                               {p.coverage, p.density, p.inv_cell, p.height_m},
                               {p.wind.x, p.wind.y, p.stretch, p.drift},
                               {sun.x, sun.y, sun.z, 0.0f},
                               {bg.x, bg.y, bg.z, 0.0f}};
        std::memcpy(&u, blk, sizeof u);
        const Array<f32> gpu = run_twin(f, dirs, pix, u);
        for (usize i = 0; i < dirs.size(); ++i) {
            const vec3 d = dirs[i];
            const f32 a = cirrus_opacity(d, p, pix[i]);
            const vec3 c = cirrus_apply(bg, d, sun, k.light, k.amb, p, pix[i]);
            const f32 fp = cirrus_fibre_pixels(d, p, pix[i]);
            if (d.y > kCirrusMinUp) {
                fibre_faded += fp < 1.5f ? 1 : 0;
                fibre_full += fp > 4.0f ? 1 : 0;
            }
            CHECK(a <= kCirrusMaxOpacity);
            const f32 ea = std::fabs(gpu[i * 4] - a);
            const f32 ec = std::max(
                {std::fabs(gpu[i * 4 + 1] - c.x), std::fabs(gpu[i * 4 + 2] - c.y), std::fabs(gpu[i * 4 + 3] - c.z)});
            // A lattice cell edge or a smoothstep knee can land on either side of a float rounding: allow a few
            // points a larger difference, none a gross one.
            if (ea > 2e-3f) ++off_a;
            CHECK_MESSAGE(ea < 0.05f, "dir " << i << ": gpu a " << gpu[i * 4] << ", cpu " << a);
            CHECK_MESSAGE(ec < 0.05f + 4.0f * ea, "dir " << i << ": colour off by " << ec);
            if (d.y <= kCirrusMinUp) {
                CHECK(gpu[i * 4] == 0.0f);  // under the limit: exactly nothing
                CHECK(gpu[i * 4 + 1] == bg.x);
            }
            worst_a = std::max(worst_a, ea);
            worst_c = std::max(worst_c, ec);
            covered += a > 0.0f ? 1 : 0;
            ++compared;
        }
    }
    MESSAGE("cirrus GLSL vs CPU twin: " << compared << " directions (" << covered << " covered), worst |da| " << worst_a
                                        << ", worst |dcolour| " << worst_c << ", |da| > 2e-3 at " << off_a
                                        << "; fibres faded out / at full detail at " << fibre_faded << " / "
                                        << fibre_full);
    CHECK(fibre_faded > compared / 20);
    CHECK(fibre_full > compared / 20);
    CHECK(covered > compared / 5);
    CHECK(off_a <= compared / 200);
}
