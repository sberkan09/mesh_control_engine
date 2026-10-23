// M-WATER 15c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the water plane of the
// main pass (kWaterVS / kWaterFS, p_water* / p_wave). No WaterSurface, `enabled` false, a non-finite level and
// `r.water 0` draw the frame without the plane bit for bit; the plane changes only pixels below the horizon; on calm
// water the reflection brightens toward the horizon (fresnel); a cloud deck and fog reach the reflection; the drawn
// normals are kit::water_normal's (a sky that encodes the reflected ray's direction in its colour, against the kit
// helper at every probe pixel); the Low tier is flat and foamless; foam lies only within foam_width_m of the opaque
// surface behind the water; geometry in front still occludes it; a floating-origin shift and the wrapped clock leave
// the pattern where it is; a one-degree camera turn keeps the glint's light steady (no crawl). One frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/kit/water.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

f64 luma(const Array<u8>& px, usize i) { return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]; }

template <class T>
bool same(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize i) {
    return a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2];
}

// The view ray through each pixel's center (top row first), from the last frame's view.
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

// One frame of the water scene (water_day_scene), bloom off: the glint's glow would spread over the horizon.
Array<u8> draw_water(Fixture3D& f, const WaterSurface* w, const std::function<void(Scene3D&)>& more = {},
                     f32 fog_density = 0.0f) {
    f.render(fog_density, true, [&](Scene3D& s) {
        water_day_scene(s, w ? *w : WaterSurface{});  // WaterSurface{}: disabled
        if (more) more(s);
        PostFx post = s.post();
        post.bloom = false;
        s.set_post(post);
    });
    return f.pixels;
}

// Pixels whose bytes differ, split by the ray: above the horizon (y > 0) or at and below it.
struct Change {
    u32 above = 0, below = 0;
};
Change change(const Array<u8>& a, const Array<u8>& b, const Array<vec3>& dirs) {
    Change c;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (same_px(a, b, p * 4)) continue;
        (dirs[p].y > 0.0f ? c.above : c.below) += 1;
    }
    return c;
}

// The world point where a pixel's ray meets the plane y = level (the camera is above it), or false.
bool on_plane(const Fixture3D& f, vec3 dir, f32 level, vec3& out) {
    if (dir.y >= -1e-4f) return false;
    const f32 t = (level - f.view.position.y) / dir.y;
    out = f.view.position + dir * t;
    return true;
}

// A 64 x 32 equirect (RGBA8, one level) whose red says the direction's x is positive and green its z: through the
// water's reflection the frame shows the signs of the reflected ray's x and z (kWaterFS samples sky_radiance along
// it), against which the test sets kit::water_normal's reflection.
u32 upload_sign_sky(Fixture3D& f) {
    Array<u8> px;
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 64; ++x) {
            const f64 phi = ((f64(x) + 0.5) / 64.0 - 0.5) * 6.283185307179586;  // equirect_uv's atan(z, x)
            px.push_back(std::cos(phi) > 0.0 ? 255 : 0);
            px.push_back(std::sin(phi) > 0.0 ? 255 : 0);
            px.push_back(0);
            px.push_back(255);
        }
    const u32 key = f.renderer.textures().allocate_key();
    const TextureMipUpload mip{64, 32, std::as_bytes(px.span())};
    TextureUpload up;
    up.format = rhi::TextureFormat::RGBA8_UNorm;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
    return key;
}

// kit::Water with the carrier's wave fields (the shader's other inputs do not shape the waves).
kit::Water kit_water(const WaterSurface& w, f32 wind_deg) {
    kit::Water k;
    k.level_y = w.level_y;
    k.wave_count = static_cast<u8>(w.wave_count);
    k.amplitude = w.amplitude;
    k.wavelength = w.wavelength;
    k.steepness = w.steepness;
    k.wind_dir_deg = wind_deg;
    k.speed_scale = w.speed_scale;
    return k;
}

}  // namespace

TEST_CASE("render3d_gl: no water, a disabled one and r.water 0 draw the frame without the plane bit for bit") {
    Fixture3D f;
    water_camera(f);
    const Array<u8> plain = draw_water(f, nullptr);
    WaterSurface off = water_carrier(0.4f);
    off.enabled = false;
    CHECK(same(draw_water(f, &off), plain));
    WaterSurface nan_level = water_carrier(0.4f);
    nan_level.level_y = std::nanf("");
    CHECK(same(draw_water(f, &nan_level), plain));
    ConVar* cv = ConVar::find(StringId("r.water"));
    REQUIRE(cv != nullptr);
    cv->set_string("0");
    const WaterSurface cut = apply_quality(quality_settings(QualityTier::High), water_carrier(0.4f));
    cv->reset();
    CHECK_FALSE(cut.enabled);
    CHECK(same(draw_water(f, &cut), plain));
    // ... and the plane, on, does draw.
    const WaterSurface on = water_carrier(0.4f);
    CHECK_FALSE(same(draw_water(f, &on), plain));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the water changes only pixels below the horizon, and the box above it still occludes it") {
    Fixture3D f;
    water_camera(f);
    const Array<u8> plain = draw_water(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const WaterSurface w = water_carrier(0.4f);
    const Array<u8> sea = draw_water(f, &w);
    const Change c = change(plain, sea, dirs);
    MESSAGE("pixels changed above / below the horizon: " << c.above << " / " << c.below);
    CHECK(c.above == 0);
    CHECK(c.below > 8000);
    // The box's top (y 1, above the 0.6 m level) and the upper half of its front face are drawn over the water: the
    // same bytes as without it, away from its silhouette (where the multisampled edge blends the new background).
    for (vec3 p : {vec3{0.0f, 1.0f, 0.0f}, vec3{0.1f, 1.0f, -0.2f}, vec3{0.0f, 0.85f, 0.5f}, vec3{-0.2f, 0.8f, 0.5f}}) {
        const math::vec4 cl = f.view.view_proj * math::vec4(p, 1.0f);
        const usize x = usize((cl.x / cl.w * 0.5f + 0.5f) * kW), y = usize((0.5f - cl.y / cl.w * 0.5f) * kH);
        CHECK(same_px(plain, sea, (y * kW + x) * 4));
    }
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: calm water reflects more toward the horizon (fresnel), and a cloud deck and fog reach it") {
    Fixture3D f;
    water_camera(f);
    // A calm black body under an even sky: what the water shows is the sky times the fresnel term. The level above
    // the box: nothing stands in the water.
    WaterSurface calm = water_carrier(0.0f);
    calm.level_y = 1.2f;
    calm.shallow_color = calm.deep_color = {0.0f, 0.0f, 0.0f};
    calm.foam_amount = 0.0f;
    const auto even_sky = [](Scene3D& s) {
        no_sun(s);
        s.set_sky(constant_sky(0.8f, false));
    };
    const Array<u8> px = draw_water(f, &calm, even_sky);
    const Array<vec3> dirs = ray_dirs(f);
    // Down a column at the left edge (clear of the box's): the luma rises toward the horizon.
    u32 falls = 0;
    f64 bottom = -1.0, top = -1.0, prev = -1.0;
    for (u32 y = kH; y-- > 0;) {
        const usize p = usize(y) * kW + 8;
        if (dirs[p].y > -0.01f) break;
        const f64 l = luma(px, p * 4);
        if (bottom < 0.0) bottom = l;
        if (prev >= 0.0 && l + 1.0 < prev) ++falls;
        prev = top = l;
    }
    MESSAGE("calm water column: bottom luma " << bottom << ", next to the horizon " << top << "; steps down " << falls);
    CHECK(top > bottom + 60.0);
    CHECK(falls == 0);
    // Clouds are drawn in the sky only: below the horizon only their reflection can move a pixel.
    const auto cloudy = [&](Scene3D& s) {
        even_sky(s);
        Clouds c;
        c.coverage = 0.9f;
        c.density = 1.0f;
        s.set_clouds(c);
    };
    WaterSurface wavy = water_carrier(0.4f);
    wavy.foam_amount = 0.0f;
    const Array<u8> clear = draw_water(f, &wavy, even_sky);
    const Array<u8> clouded = draw_water(f, &wavy, cloudy);
    const Array<u8> clouded_plain = draw_water(f, nullptr, cloudy);
    const Array<u8> clear_plain = draw_water(f, nullptr, even_sky);
    const Change by_clouds = change(clear, clouded, dirs);
    u32 ground_moved = 0;  // without water the clouds move no pixel below the horizon
    for (usize p = 0; p < dirs.size(); ++p)
        if (dirs[p].y < -0.02f && !same_px(clear_plain, clouded_plain, p * 4)) ++ground_moved;
    MESSAGE("a cloud deck moves " << by_clouds.below << " water pixels (" << ground_moved << " without water)");
    CHECK(ground_moved == 0);
    CHECK(by_clouds.below > 2000);
    // Fog: the exp2 haze of the reflected sky and the fog over the surface itself.
    const Array<u8> foggy = draw_water(f, &wavy, even_sky, 0.02f);
    CHECK(change(clear, foggy, dirs).below > 4000);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the drawn normals are kit::water_normal's (the reflected ray's direction at every probe)") {
    Fixture3D f;
    // Straight down onto the sea from 7 m, the level above everything (no shore, no depth); black body, no sun, no
    // fog, no foam: a pixel is the sign sky along the reflection, times the fresnel term.
    f.look_at = {0.0f, 2.0f, 0.0f};
    f.pitch_deg = 80.0f;
    const u32 sky_tex = upload_sign_sky(f);
    for (const f32 wind_deg : {0.0f, 130.0f}) {
        WaterSurface w = water_carrier(0.5f, 7.25f);
        w.level_y = 2.0f;
        w.wavelength = 12.0f;
        w.foam_amount = 0.0f;
        w.shallow_color = w.deep_color = {0.0f, 0.0f, 0.0f};
        const f32 rad = math::radians(wind_deg);
        w.wind_dir = {std::cos(rad), std::sin(rad)};
        draw_water(f, &w, [&](Scene3D& s) {
            no_sun(s);
            Sky sky = constant_sky(0.0f, false);
            sky.texture = sky_tex;
            sky.texture_intensity = 40.0f;
            s.set_sky(sky);
            s.set_exposure(1.0f);
        });
        const Array<vec3> dirs = ray_dirs(f);
        const kit::WaterWaveSet set = kit::water_wave_set(kit_water(w, wind_deg));
        u32 probes = 0, agree = 0, flat_agree = 0;
        for (u32 y = 4; y + 4 < kH; y += 3)
            for (u32 x = 4; x + 4 < kW; x += 3) {
                const usize p = usize(y) * kW + x;
                vec3 at;
                if (!on_plane(f, dirs[p], w.level_y, at)) continue;
                const vec3 n = kit::water_normal(set, math::vec2{at.x, at.z}, w.time);
                const vec3 v = -dirs[p];
                const vec3 r = n * (2.0f * math::dot(n, v)) - v;
                const vec3 r_flat{-v.x, v.y, -v.z};
                if (std::fabs(r.x) < 0.12f || std::fabs(r.z) < 0.12f) continue;  // near a colour edge of the sky
                ++probes;
                const bool red = f.pixels[p * 4] > 128, green = f.pixels[p * 4 + 1] > 128;
                agree += (red == (r.x > 0.0f) && green == (r.z > 0.0f)) ? 1u : 0u;
                flat_agree += (red == (r_flat.x > 0.0f) && green == (r_flat.z > 0.0f)) ? 1u : 0u;
            }
        MESSAGE("wind " << wind_deg << " deg: " << agree << " of " << probes << " probes agree with kit::water_normal ("
                        << flat_agree << " with a flat mirror)");
        REQUIRE(probes > 300);
        CHECK(f64(agree) > 0.97 * f64(probes));
        CHECK(f64(flat_agree) < 0.8 * f64(probes));  // the waves do turn the reflection: the probe is not vacuous
    }
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the Low tier draws a flat water plane without foam") {
    Fixture3D f;
    water_camera(f);
    const WaterSurface high = apply_quality(quality_settings(QualityTier::High), water_carrier(0.4f));
    const WaterSurface low = apply_quality(quality_settings(QualityTier::Low), water_carrier(0.4f));
    REQUIRE(low.enabled);
    CHECK(low.amplitude == 0.0f);
    CHECK(low.foam_amount == 0.0f);
    // The Low frame is the flat, foamless plane's bit for bit (the distance tint: no depth read either) ...
    WaterSurface flat = high;
    flat.amplitude = 0.0f;
    flat.foam_amount = 0.0f;
    const Array<u8> px_low = draw_water(f, &low);
    CHECK(same(px_low, draw_water(f, &flat)));
    // ... and not the High one's, whose waves move most of the sea.
    const Array<vec3> dirs = ray_dirs(f);
    const Change c = change(px_low, draw_water(f, &high), dirs);
    MESSAGE("High against Low: " << c.below << " pixels below the horizon differ");
    CHECK(c.above == 0);
    CHECK(c.below > 8000);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: shore foam lies only within foam_width_m of the opaque surface behind the water") {
    Fixture3D f;  // the fixture's camera: 4.5 m up, 35 degrees down onto the box
    WaterSurface w = water_carrier(0.0f);
    w.level_y = 0.5f;  // half the box out of the water, the slab's top 0.5 m under it (about 0.8 m along these rays)
    w.foam_width_m = 0.3f;
    w.shallow_color = w.deep_color = {0.0f, 0.0f, 0.0f};
    const auto dark = [](Scene3D& s) { s.set_sky(constant_sky(0.05f, false)); };
    WaterSurface no_foam = w;
    no_foam.foam_amount = 0.0f;
    WaterSurface foam = w;
    foam.foam_amount = 1.0f;
    const Array<u8> a = draw_water(f, &no_foam, dark), b = draw_water(f, &foam, dark);
    const Array<vec3> dirs = ray_dirs(f);
    // The distance along each ray from the water to the first opaque surface behind it: the box ([-0.5, 0.5] x [0, 1]
    // x [-0.5, 0.5]) or the slab's top (y 0, |x|, |z| < 6).
    const auto path_behind = [&](vec3 dir, vec3 at) -> f32 {
        f32 best = 1e9f;
        f32 t0 = 0.0f, t1 = 1e9f;
        const vec3 lo{-0.5f, 0.0f, -0.5f}, hi{0.5f, 1.0f, 0.5f};
        bool hit = true;
        for (int i = 0; i < 3 && hit; ++i) {
            const f32 o = (&at.x)[i], d = (&dir.x)[i];
            if (std::fabs(d) < 1e-6f) {
                hit = o >= (&lo.x)[i] && o <= (&hi.x)[i];
                continue;
            }
            f32 ta = ((&lo.x)[i] - o) / d, tb = ((&hi.x)[i] - o) / d;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            hit = t0 <= t1;
        }
        if (hit) best = t0;
        const f32 tg = -at.y / dir.y;
        const vec3 g = at + dir * tg;
        if (std::fabs(g.x) < 6.0f && std::fabs(g.z) < 6.0f) best = std::min(best, tg);
        return best;
    };
    // The analytic distance per pixel (1e9: no water there, or the box's own top).
    Array<f32> paths;
    paths.resize(dirs.size());
    for (usize p = 0; p < dirs.size(); ++p) {
        vec3 at;
        paths[p] = on_plane(f, dirs[p], w.level_y, at) && !(std::fabs(at.x) <= 0.5f && std::fabs(at.z) <= 0.5f)
                       ? path_behind(dirs[p], at)
                       : 1e9f;
    }
    // The depth is read at half size, so a foam pixel may take the box's depth from a half-size texel next to it:
    // a foam pixel is inside the band when a pixel within 2 of it is.
    u32 foamed = 0, outside = 0, near = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (paths[p] >= 1e8f) continue;
        near += paths[p] < w.foam_width_m ? 1u : 0u;
        if (luma(b, p * 4) <= luma(a, p * 4) + 6.0) continue;
        ++foamed;
        const i32 x = i32(p % kW), y = i32(p / kW);
        f32 best = 1e9f;
        for (i32 dy = -2; dy <= 2; ++dy)
            for (i32 dx = -2; dx <= 2; ++dx) {
                const i32 nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < i32(kW) && ny < i32(kH))
                    best = std::min(best, paths[usize(ny) * kW + nx]);
            }
        if (best > w.foam_width_m) {
            ++outside;
            MESSAGE("foam at " << x << "," << y << ": " << paths[p] << " m behind (" << best << " within 2 pixels)");
        }
    }
    MESSAGE(foamed << " foam pixels, " << outside << " beyond the band; " << near << " pixels within it");
    CHECK(foamed > 20);  // measured 40 of the 88 pixels within the band: the stripes break it up
    CHECK(outside == 0);
    CHECK(foamed <= near + near / 2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the water pattern stays put across an origin shift and over the clock's wrap") {
    Fixture3D f;
    water_camera(f);
    const WaterSurface w = water_carrier(0.4f, 12.5f);
    // No shadows here: their fit snaps to world-space texels, which a 1000 m move does not keep (not what is tested).
    const auto draw = [&](const WaterSurface& s, const std::function<void(Scene3D&)>& more = {}) {
        f.render(0.0f, false, [&](Scene3D& sc) {
            water_day_scene(sc, s);
            if (more) more(sc);
        });
        return f.pixels;
    };
    const Array<u8> home = draw(w);
    // The clock: the period is 64 s and every wave a whole number of cycles in it, wrapped in f64 on the CPU, so a
    // time 10 periods (and 1000) later is the same frame bit for bit; ten simulated minutes on is the frame of the
    // wrapped time.
    for (const f32 later : {12.5f + 640.0f, 12.5f + 64000.0f}) {
        WaterSurface l = w;
        l.time = later;
        CHECK(same(draw(l), home));
    }
    WaterSurface ten = w, ten_wrapped = w;
    ten.time = 12.5f + 600.0f;
    ten_wrapped.time = kit::water_wrap_time(12.5 + 600.0);
    CHECK(same(draw(ten), draw(ten_wrapped)));
    // A floating-origin rebase of 1000 m: the world (camera, ground, box) moves by -shift in render space and the
    // renderer is told; the waves are where they were (within the f32 precision of positions 1000 m out).
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    f.world_offset = -shift;
    const Array<u8> unannounced = draw(w);
    const Array<u8> shifted = draw(w, [&](Scene3D& s) { s.set_origin_shift(shift); });
    f.world_offset = {0.0f, 0.0f, 0.0f};
    const Array<vec3> dirs = ray_dirs(f);
    const auto moved = [&](const Array<u8>& x) {
        u32 n = 0;
        for (usize p = 0; p < dirs.size(); ++p)
            if (dirs[p].y < 0.0f && std::fabs(luma(home, p * 4) - luma(x, p * 4)) > 8.0) ++n;
        return n;
    };
    const u32 m_shift = moved(shifted), m_control = moved(unannounced);
    MESSAGE("water pixels moved by more than 8 luma after a 1000 m rebase: " << m_shift << " (unannounced " << m_control
                                                                             << ")");
    CHECK(m_shift < 200);
    CHECK(m_control > 5 * m_shift + 500);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the water's sun glint keeps its light under a slow camera turn (no crawl)") {
    Fixture3D f;
    water_camera(f);
    WaterSurface w = water_carrier(0.4f);
    w.foam_amount = 0.0f;
    f64 lo = 1e30, hi = 0.0;
    for (u32 i = 0; i <= 10; ++i) {  // a one-degree turn in tenth-degree steps
        f.yaw_deg = 0.1f * f32(i);
        const Array<u8> bare = draw_water(f, &w, [](Scene3D& s) {
            DirectionalLight dark;  // the same frame without the glint: the light off, the ambient kept
            dark.direction = {0.25f, -0.35f, 0.9f};
            dark.intensity = 0.0f;
            s.set_light(dark);
        });
        const Array<u8> lit = draw_water(f, &w);
        const Array<vec3> dirs = ray_dirs(f);
        f64 flux = 0.0;
        for (usize p = 0; p < dirs.size(); ++p)
            if (dirs[p].y < 0.0f) flux += std::max(0.0, luma(lit, p * 4) - luma(bare, p * 4));
        lo = std::min(lo, flux);
        hi = std::max(hi, flux);
    }
    f.yaw_deg = 0.0f;
    MESSAGE("glint flux over a one-degree turn: " << lo << " .. " << hi);
    REQUIRE(lo > 0.0);
    CHECK(hi / lo < 1.05);  // measured 1.018
    CHECK(f.device->validation_error_count() == 0);
}
