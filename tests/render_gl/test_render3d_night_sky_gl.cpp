// M-NIGHTSKY 14d against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the moon disk and the
// star field in the sky pass (p_night / p_night2 / p_moon / p_moon2, kSkyFS). Star density 0 with moon intensity 0,
// `enabled` false, a night fade of 0 and the Low tier's stars draw the frame without the night sky bit for bit; the
// stars change only the sky above the horizon; the moon disk is brighter than the sky around it and its lit share
// follows the phase, on the side the phase says; a cloud deck in front dims the moon and the stars, a thick one hides
// them; a dense volumetric fog hides the moon and the exp2 horizon haze dims a low one; a floating-origin shift leaves
// the sky unchanged; the twinkle clock wraps without a seam, so ten simulated minutes draw the same pattern; a slow
// camera turn keeps the stars' total brightness steady (no crawl). The scene is the night sky (fixture3d.h). One frame
// per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "fixture3d.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

f64 luma(const Array<u8>& px, usize i) { return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]; }

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

template <class T>
bool same(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize i) {
    return a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2];
}

struct Change {
    u32 above = 0;  // pixels whose ray goes up and whose bytes moved
    u32 below = 0;  // pixels whose ray points at or below the horizon whose bytes moved
    u32 sky = 0;    // pixels whose ray goes up
};

Change change(const Array<u8>& a, const Array<u8>& b, const Array<vec3>& dirs) {
    Change c;
    for (usize p = 0; p < dirs.size(); ++p) {
        const bool moved = !same_px(a, b, p * 4);
        if (dirs[p].y <= 0.0f) {
            c.below += moved ? 1u : 0u;
        } else {
            ++c.sky;
            c.above += moved ? 1u : 0u;
        }
    }
    return c;
}

// Total luma the stars add over the starless frame `bare`, over the sky pixels whose ray climbs more than `min_up`.
f64 star_flux(const Array<u8>& px, const Array<u8>& bare, const Array<vec3>& dirs, f32 min_up = 0.0f) {
    f64 sum = 0.0;
    for (usize p = 0; p < dirs.size(); ++p)
        if (dirs[p].y > min_up) sum += std::max(0.0, luma(px, p * 4) - luma(bare, p * 4));
    return sum;
}

// The moon disk's pixels (rays within 0.85 of its angular radius of `moon`) and a ring around it (1.6 to 2.4 radii).
struct Disk {
    f64 center = 0.0;  // mean luma of the disk's middle (within 0.3 radii)
    f64 ring = 0.0;    // mean luma of the ring
    u32 pixels = 0;    // disk pixels
    u32 lit = 0;       // disk pixels brighter than the half-way luma `lit_luma`
    f64 lit_x = 0.0;   // mean screen x of the lit pixels minus the disk's mean x (pixels; + is right)
};

Disk disk(const Array<u8>& px, const Array<vec3>& dirs, vec3 moon, f32 cos_size, f64 lit_luma = 128.0) {
    moon = math::normalize(moon);
    const f64 radius = std::acos(f64(cos_size));
    Disk d;
    u32 center_n = 0, ring_n = 0;
    f64 x_all = 0.0, x_lit = 0.0;
    for (usize p = 0; p < dirs.size(); ++p) {
        const f64 a = std::acos(std::min(1.0, f64(math::dot(dirs[p], moon)))) / radius;
        const f64 l = luma(px, p * 4);
        const f64 x = f64(p % kW);
        if (a < 0.85) {
            ++d.pixels;
            x_all += x;
            if (l > lit_luma) {
                ++d.lit;
                x_lit += x;
            }
            if (a < 0.3) {
                d.center += l;
                ++center_n;
            }
        } else if (a > 1.6 && a < 2.4) {
            d.ring += l;
            ++ring_n;
        }
    }
    if (center_n) d.center /= f64(center_n);
    if (ring_n) d.ring /= f64(ring_n);
    if (d.lit && d.pixels) d.lit_x = x_lit / f64(d.lit) - x_all / f64(d.pixels);
    return d;
}

// No bloom: the moon's glow would spill below the horizon and into the ring (not what is measured).
Array<u8> draw_night(Fixture3D& f, const NightSky* night, const std::function<void(Scene3D&)>& more = {},
                     f32 fog_density = 0.0f) {
    f.render(fog_density, true, [&](Scene3D& s) {
        night_sky_scene(s, night ? *night : NightSky{});  // NightSky{}: moon intensity and star density 0
        if (more) more(s);
        PostFx post = s.post();
        post.bloom = false;
        s.set_post(post);
    });
    return f.pixels;
}

NightSky stars_only(f32 density) { return night_sky_carrier(0.5f, density, 0.0f); }

NightSky moon_only(f32 phase, f32 cos_size = 0.995f) {
    NightSky n = night_sky_carrier(phase, 0.0f);
    n.moon_cos_size = cos_size;
    return n;
}

}  // namespace

TEST_CASE(
    "render3d_gl: a night sky with no moon and no stars, disabled, faded out or Low-tier stars is the plain frame") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_night(f, nullptr);
    NightSky zero = night_sky_carrier(0.3f, 0.0f, 0.0f);
    zero.star_time = 77.0f;
    CHECK(same(draw_night(f, &zero), none));
    NightSky off = night_sky_carrier(0.5f, 0.8f);
    off.enabled = false;
    CHECK(same(draw_night(f, &off), none));
    NightSky day = night_sky_carrier(0.5f, 0.8f);
    day.night_fade = 0.0f;
    CHECK(same(draw_night(f, &day), none));
    const NightSky low = apply_quality(quality_settings(QualityTier::Low), stars_only(0.8f));
    CHECK(low.star_density == 0.0f);
    CHECK(same(draw_night(f, &low), none));    // Low keeps the moon (none here) and drops the stars
    NightSky nan = stars_only(std::nanf(""));  // the upload clamps again: a NaN density is no stars
    CHECK(same(draw_night(f, &nan), none));
    const NightSky high = apply_quality(quality_settings(QualityTier::High), stars_only(0.8f));
    CHECK_FALSE(same(draw_night(f, &high), none));
    const NightSky low_moon = apply_quality(quality_settings(QualityTier::Low), night_sky_carrier(0.5f, 0.8f));
    CHECK_FALSE(same(draw_night(f, &low_moon), none));  // the Low tier still draws the moon
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the stars change only the sky above the horizon, and more density lights more of it") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_night(f, nullptr);
    const Array<vec3> dirs = ray_dirs(f);
    const f32 dens[3] = {0.2f, 0.6f, 1.0f};
    Change c[3];
    f64 flux[3];
    for (u32 i = 0; i < 3; ++i) {
        const NightSky n = stars_only(dens[i]);
        const Array<u8> px = draw_night(f, &n);
        c[i] = change(none, px, dirs);
        flux[i] = star_flux(px, none, dirs);
    }
    MESSAGE("sky pixels " << c[0].sky << "; changed by the stars at density 0.2 / 0.6 / 1: " << c[0].above << " / "
                          << c[1].above << " / " << c[2].above << " (flux " << flux[0] << " / " << flux[1] << " / "
                          << flux[2] << "); pixels at or below the horizon changed: " << c[0].below << " / "
                          << c[1].below << " / " << c[2].below);
    REQUIRE(c[0].sky > 4000);
    for (u32 i = 0; i < 3; ++i) CHECK(c[i].below == 0);
    CHECK(c[0].above > 0);
    CHECK(c[1].above > c[0].above);
    CHECK(c[2].above > c[1].above);
    CHECK(flux[2] > flux[1]);
    CHECK(f64(c[2].above) < 0.5 * f64(c[2].sky));  // points, not a glow: most of the sky stays as it was
    // Brightness scales the stars' light, not where they are.
    NightSky dim = stars_only(0.6f), bright = stars_only(0.6f);
    dim.star_brightness = 0.5f;
    bright.star_brightness = 2.0f;
    CHECK(star_flux(draw_night(f, &dim), none, dirs) < flux[1]);
    CHECK(star_flux(draw_night(f, &bright), none, dirs) > flux[1]);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the stars are as dense toward a cube corner of the cell grid as toward a face centre") {
    // The star cells are gnomonic cube-face cells, about 5x smaller in solid angle at the cube's corners than at a
    // face's centre; the presence threshold scales with the cell's solid angle, so the stars per steradian are even.
    // Two views of the same field of view, high above the ground: one straight up (the +y face's centre), one toward
    // the corner (1, 1, -1) (35.3 degrees up, 45 degrees of yaw). Measured: the share of sky pixels a star lights
    // (over the rays that climb more than 0.3, clear of the horizon fade), at density 0.5 and 1.
    Fixture3D f;
    f.look_at = {0.0f, 20.0f, 0.0f};
    const auto share = [&](f32 yaw, f32 pitch, f32 density, vec3& center) {
        f.yaw_deg = yaw;
        f.pitch_deg = pitch;
        const Array<u8> bare = draw_night(f, nullptr);
        const Array<vec3> dirs = ray_dirs(f);
        const NightSky n = stars_only(density);
        const Array<u8> px = draw_night(f, &n);
        u32 lit = 0, sky = 0;
        for (usize p = 0; p < dirs.size(); ++p) {
            if (dirs[p].y <= 0.3f) continue;
            ++sky;
            lit += same_px(px, bare, p * 4) ? 0u : 1u;
        }
        center = dirs[usize(kH / 2) * kW + kW / 2];
        return sky > 0 ? f64(lit) / f64(sky) : 0.0;
    };
    for (const f32 density : {0.5f, 1.0f}) {
        vec3 c_face, c_corner_a, c_corner_b;
        const f64 face = share(0.0f, -89.0f, density, c_face);
        const f64 corner_a = share(45.0f, -35.26f, density, c_corner_a);
        const f64 corner_b = share(-45.0f, -35.26f, density, c_corner_b);
        MESSAGE("density " << density << ": star pixel share face centre " << face << ", corners " << corner_a << " / "
                           << corner_b << " (view centres " << c_face.x << " " << c_face.y << " " << c_face.z << " / "
                           << c_corner_a.x << " " << c_corner_a.y << " " << c_corner_a.z << ")");
        CHECK(c_face.y > 0.99f);                                           // straight up
        CHECK(std::fabs(std::fabs(c_corner_a.x) - c_corner_a.y) < 0.02f);  // a cube corner direction
        CHECK(std::fabs(std::fabs(c_corner_a.z) - c_corner_a.y) < 0.02f);
        REQUIRE(face > 0.0);
        for (const f64 corner : {corner_a, corner_b}) {
            CHECK(corner / face < 1.5);
            CHECK(face / corner < 1.5);
        }
    }
    f.yaw_deg = 0.0f;
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the moon disk is brighter than the sky around it, and its lit share follows the phase") {
    Fixture3D f;
    cloud_sky_camera(f);
    const NightSky full_n = moon_only(0.5f);
    const Array<u8> full = draw_night(f, &full_n);
    const Array<vec3> dirs = ray_dirs(f);
    const Disk d_full = disk(full, dirs, kNightMoonDir, full_n.moon_cos_size);
    MESSAGE("full moon: " << d_full.pixels << " disk pixels, " << d_full.lit << " lit, center luma " << d_full.center
                          << ", ring " << d_full.ring);
    REQUIRE(d_full.pixels > 300);  // a disk of about 15 pixels radius
    CHECK(d_full.center > 200.0);
    CHECK(d_full.ring < 0.25 * d_full.center);
    CHECK(f64(d_full.lit) > 0.95 * f64(d_full.pixels));
    // The moon is cut at the horizon: nothing below it changes.
    const Array<u8> none = draw_night(f, nullptr);
    CHECK(change(none, full, dirs).below == 0);

    // Waxing toward full and waning back: the lit share grows with the illumination (1 - cos(2 pi phase)) / 2.
    const f32 phases[5] = {0.03f, 0.12f, 0.25f, 0.32f, 0.5f};
    u32 lit[5];
    for (u32 i = 0; i < 5; ++i) {
        const NightSky n = moon_only(phases[i]);
        lit[i] = disk(draw_night(f, &n), dirs, kNightMoonDir, n.moon_cos_size).lit;
    }
    MESSAGE("lit disk pixels at phase 0.03 / 0.12 / 0.25 / 0.32 / 0.5: " << lit[0] << " / " << lit[1] << " / " << lit[2]
                                                                         << " / " << lit[3] << " / " << lit[4]);
    for (u32 i = 0; i + 1 < 5; ++i) CHECK(lit[i + 1] > lit[i]);
    CHECK(f64(lit[0]) < 0.08 * f64(d_full.pixels));  // nearly new: a sliver at most
    const f64 quarter = f64(lit[2]) / f64(d_full.pixels);
    CHECK(quarter > 0.35);
    CHECK(quarter < 0.65);
    // Waxing and waning quarters light opposite halves; the same illumination lights the same share.
    const NightSky first_n = moon_only(0.25f), last_n = moon_only(0.75f);
    const Disk first = disk(draw_night(f, &first_n), dirs, kNightMoonDir, first_n.moon_cos_size);
    const Disk last = disk(draw_night(f, &last_n), dirs, kNightMoonDir, last_n.moon_cos_size);
    MESSAGE("first / last quarter: lit " << first.lit << " / " << last.lit << ", lit side x offset " << first.lit_x
                                         << " / " << last.lit_x << " px");
    CHECK(std::abs(i32(first.lit) - i32(last.lit)) < i32(d_full.pixels / 20));
    CHECK(first.lit_x * last.lit_x < 0.0);
    CHECK(std::abs(first.lit_x) > 3.0);
    // The new moon: only the faint earthshine, far under the lit disk.
    const NightSky new_n = moon_only(0.0f);
    const Disk d_new = disk(draw_night(f, &new_n), dirs, kNightMoonDir, new_n.moon_cos_size);
    CHECK(d_new.lit == 0);
    CHECK(d_new.center < 0.5 * d_full.center);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a cloud deck in front dims the moon and hides the stars; fog hides the moon") {
    Fixture3D f;
    cloud_sky_camera(f);
    const NightSky full = moon_only(0.5f);
    const Array<u8> clear = draw_night(f, &full);
    const Array<vec3> dirs = ray_dirs(f);
    // The moon is composited under the clouds, as the sun disk is: a broken deck of the default density lets part of
    // its light through, a thick one (density 1: opaque where it covers) hides it.
    Clouds thin, deck;
    thin.coverage = deck.coverage = 1.0f;
    deck.density = 1.0f;
    const auto broken = [&](Scene3D& s) { s.set_clouds(thin); };
    const auto overcast = [&](Scene3D& s) { s.set_clouds(deck); };
    const Disk d_clear = disk(clear, dirs, kNightMoonDir, full.moon_cos_size);
    const Disk d_thin = disk(draw_night(f, &full, broken), dirs, kNightMoonDir, full.moon_cos_size);
    const Disk d_cloud = disk(draw_night(f, &full, overcast), dirs, kNightMoonDir, full.moon_cos_size);
    const Disk d_cloud_bare = disk(draw_night(f, nullptr, overcast), dirs, kNightMoonDir, full.moon_cos_size);
    MESSAGE("moon center luma clear / thin deck / thick deck / thick deck without a moon: "
            << d_clear.center << " / " << d_thin.center << " / " << d_cloud.center << " / " << d_cloud_bare.center);
    CHECK(d_thin.center < d_clear.center);
    CHECK(d_cloud.center < 0.25 * d_clear.center);
    CHECK(d_cloud.center - d_cloud_bare.center < 10.0);
    // Stars: what a clear sky adds over the starless one, against what an overcast one adds over its own.
    const NightSky stars = stars_only(0.8f);
    const Array<u8> none = draw_night(f, nullptr), none_cloud = draw_night(f, nullptr, overcast);
    const Change c_clear = change(none, draw_night(f, &stars), dirs);
    const Change c_cloud = change(none_cloud, draw_night(f, &stars, overcast), dirs);
    const Array<u8> none_thin = draw_night(f, nullptr, broken);
    const Change c_thin = change(none_thin, draw_night(f, &stars, broken), dirs);
    // Flux over the rays that climb more than 0.25, above the band where the deck fades into the horizon haze.
    const f64 flux_clear = star_flux(draw_night(f, &stars), none, dirs, 0.25f);
    const f64 flux_thin = star_flux(draw_night(f, &stars, broken), none_thin, dirs, 0.25f);
    const f64 flux_cloud = star_flux(draw_night(f, &stars, overcast), none_cloud, dirs, 0.25f);
    MESSAGE("star pixels clear / thin deck / thick deck: " << c_clear.above << " / " << c_thin.above << " / "
                                                           << c_cloud.above << " (flux " << flux_clear << " / "
                                                           << flux_thin << " / " << flux_cloud << ")");
    CHECK(c_thin.above < c_clear.above);
    CHECK(c_cloud.above < c_thin.above);
    CHECK(flux_thin < 0.5 * flux_clear);
    CHECK(flux_cloud <
          0.15 * flux_clear);  // what is left shines through the deck's thin spots (coverage 1 is not solid)

    // Fog is applied over the night sky. A dense volumetric fog hides the moon. The exp2 fog reaches the sky pass only
    // as its horizon haze (a blend toward the fog colour by the ray's height, whatever the density), so it dims a moon
    // low over the horizon, about 4 degrees up (to about a tenth of its light, still bright after the tonemap).
    const auto night_fog = [](Scene3D& s) {
        Fog fog = s.fog();
        fog.color = {0.02f, 0.025f, 0.035f};
        s.set_fog(fog);
    };
    const auto dense = [&](Scene3D& s) {
        night_fog(s);
        Volumetrics v;
        v.enabled = true;
        v.density = 0.2f;
        v.height_falloff = 0.0f;
        s.set_volumetrics(v);
    };
    const Disk d_vol = disk(draw_night(f, &full, dense), dirs, kNightMoonDir, full.moon_cos_size);
    MESSAGE("moon center / ring luma: clear " << d_clear.center << " / " << d_clear.ring << ", dense volumetric fog "
                                              << d_vol.center << " / " << d_vol.ring);
    CHECK(d_clear.center - d_clear.ring > 150.0);
    CHECK(d_vol.center - d_vol.ring < 0.1 * (d_clear.center - d_clear.ring));
    const vec3 low_dir{0.2f, 0.07f, -0.977f};
    NightSky low = moon_only(0.5f, 0.999f);
    low.moon_dir = low_dir;
    const Disk d_low = disk(draw_night(f, &low), dirs, low_dir, low.moon_cos_size);
    const Disk d_haze = disk(draw_night(f, &low, night_fog, 0.02f), dirs, low_dir, low.moon_cos_size);
    MESSAGE("low moon center / ring luma: clear " << d_low.center << " / " << d_low.ring << ", exp2 haze "
                                                  << d_haze.center << " / " << d_haze.ring);
    REQUIRE(d_low.pixels > 40);
    CHECK(d_low.center - d_low.ring > 150.0);
    CHECK(d_haze.center < d_low.center - 30.0);
    CHECK(d_haze.center - d_haze.ring < 0.85 * (d_low.center - d_low.ring));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the night sky stays put across an origin shift and over ten simulated minutes") {
    Fixture3D f;
    cloud_sky_camera(f);
    const NightSky n = night_sky_carrier(0.3f, 0.7f);
    const Array<u8> home = draw_night(f, &n);
    const Array<vec3> dirs = ray_dirs(f);
    // A floating-origin rebase (the renderer's origin moves 1000 m; the camera and the world stay where they are in
    // render space): the layer is a function of the view direction alone, so the frame is the same bit for bit.
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    CHECK(same(draw_night(f, &n, [&](Scene3D& s) { s.set_origin_shift(shift); }), home));
    // The camera (with the world) 1000 m away in render space: only the f32 precision of the reconstructed ray moves.
    f.world_offset = -shift;
    const Array<u8> moved = draw_night(f, &n, [&](Scene3D& s) { s.set_origin_shift(shift); });
    f.world_offset = {0.0f, 0.0f, 0.0f};
    const Array<u8> none = draw_night(f, nullptr);
    const f64 flux_home = star_flux(home, none, dirs), flux_moved = star_flux(moved, none, dirs);
    u32 off = 0;
    for (usize p = 0; p < dirs.size(); ++p)
        if (dirs[p].y > 0.0f && std::fabs(luma(home, p * 4) - luma(moved, p * 4)) > 8.0) ++off;
    MESSAGE("camera 1000 m away: " << off << " sky pixels moved by more than 8 luma; star flux " << flux_home << " / "
                                   << flux_moved);
    CHECK(f64(off) < 0.01 * f64(change(home, home, dirs).sky));
    CHECK(std::fabs(flux_moved - flux_home) < 0.05 * flux_home);

    // The twinkle clock is wrapped to 60 s on the CPU and every star's rate is a whole number of cycles in it: ten
    // simulated minutes (and three days) later the frame is the same bit for bit, at every whole minute in between.
    for (u32 m = 1; m <= 10; ++m) {
        NightSky later = n;
        later.star_time = n.star_time + 60.0f * f32(m);
        CHECK(same(draw_night(f, &later), home));
    }
    NightSky days = n;
    days.star_time = n.star_time + 3.0f * 86400.0f;
    CHECK(same(draw_night(f, &days), home));
    // Between them the stars twinkle in place: their brightness moves, where they are does not.
    NightSky mid = n;
    mid.star_time = n.star_time + 17.5f;
    const Array<u8> twinkled = draw_night(f, &mid);
    CHECK_FALSE(same(twinkled, home));
    u32 lit_home = 0, lit_both = 0;
    for (usize p = 0; p < dirs.size(); ++p) {
        if (dirs[p].y <= 0.0f || same_px(home, none, p * 4)) continue;
        ++lit_home;
        lit_both += same_px(twinkled, none, p * 4) ? 0u : 1u;
    }
    MESSAGE("star pixels at 12.25 s: " << lit_home << ", still lit at 29.75 s: " << lit_both);
    CHECK(f64(lit_both) > 0.9 * f64(lit_home));
    // Twinkle 0: any clock draws the same frame.
    NightSky still = n, still_later = n;
    still.star_twinkle = still_later.star_twinkle = 0.0f;
    still_later.star_time = 1234.5f;
    CHECK(same(draw_night(f, &still), draw_night(f, &still_later)));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the star field keeps its total brightness under a slow camera turn (no crawl)") {
    Fixture3D f;
    cloud_sky_camera(f);
    const NightSky n = stars_only(0.8f);
    f64 lo = 1e30, hi = 0.0;
    for (u32 i = 0; i <= 10; ++i) {  // a one-degree turn in tenth-degree steps (a quarter to a third of a pixel each)
        f.yaw_deg = 0.1f * f32(i);
        const Array<u8> bare = draw_night(f, nullptr);
        const Array<vec3> dirs = ray_dirs(f);
        const f64 flux = star_flux(draw_night(f, &n), bare, dirs);
        lo = std::min(lo, flux);
        hi = std::max(hi, flux);
    }
    f.yaw_deg = 0.0f;
    MESSAGE("star flux over a one-degree turn: " << lo << " .. " << hi);
    REQUIRE(lo > 0.0);
    CHECK(hi / lo < 1.2);
    CHECK(f.device->validation_error_count() == 0);
}
