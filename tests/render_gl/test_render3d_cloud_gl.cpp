// M-CLOUDS 9b against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the procedural cloud
// layer in the sky pass (p_cloud / p_cloud2, kSkyFS). Coverage 0, `enabled` false and the Low tier draw the cloudless
// frame bit for bit; clouds change only the sky above the horizon; more coverage covers more of it; the layer drifts
// with the scene clock along the wind; it is camera-relative (a floating-origin shift leaves it in place); a night
// sky stays dark. The scene is the cloud sky (fixture3d.h). One frame per render.

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

// The y of the view ray through each pixel's center (top row first), from the last frame's view.
Array<f32> ray_heights(const Fixture3D& f) {
    const math::mat4 inv = math::inverse(f.view.view_proj);
    Array<f32> h;
    h.reserve(usize(kW) * kH);
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const f32 nx = (f32(x) + 0.5f) / f32(kW) * 2.0f - 1.0f, ny = 1.0f - (f32(y) + 0.5f) / f32(kH) * 2.0f;
            const math::vec4 w = inv * math::vec4(nx, ny, 1.0f, 1.0f);
            const vec3 d = math::normalize(vec3{w.x / w.w, w.y / w.w, w.z / w.w} - f.view.position);
            h.push_back(d.y);
        }
    return h;
}

template <class T>
bool same(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

struct Change {
    u32 above = 0;   // pixels whose ray goes up more than `min_up` and whose luma moved by more than 2
    u32 below = 0;   // pixels whose ray points at or below the horizon whose bytes moved at all
    u32 sky = 0;     // pixels whose ray goes up more than `min_up`
    f64 mean = 0.0;  // mean |luma difference| over those
};

Change change(const Array<u8>& a, const Array<u8>& b, const Array<f32>& up, f32 min_up = 0.05f) {
    Change c;
    for (usize p = 0; p < up.size(); ++p) {
        const usize i = p * 4;
        if (up[p] <= 0.0f) {
            c.below += (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) ? 1u : 0u;
        } else if (up[p] > min_up) {
            const f64 d = std::fabs(luma(a, i) - luma(b, i));
            ++c.sky;
            c.mean += d;
            c.above += d > 2.0 ? 1u : 0u;
        }
    }
    if (c.sky) c.mean /= f64(c.sky);
    return c;
}

// The sky's mean and largest luma over the pixels whose ray goes up more than 0.05.
void sky_luma(const Array<u8>& px, const Array<f32>& up, f64& mean, f64& max) {
    mean = max = 0.0;
    u32 n = 0;
    for (usize p = 0; p < up.size(); ++p)
        if (up[p] > 0.05f) {
            const f64 l = luma(px, p * 4);
            mean += l;
            max = std::max(max, l);
            ++n;
        }
    if (n) mean /= f64(n);
}

Clouds windy(Clouds c, f32 x, f32 z) {
    c.wind_dir = {x, z};
    return c;
}

// No bloom: a bright cloud's glow would spill below the horizon (not what is measured). `clouds` null: none.
Array<u8> draw_sky(Fixture3D& f, const Clouds* clouds, f32 time = 0.0f,
                   const std::function<void(Scene3D&)>& more = {}) {
    f.render(0.0f, true, [&](Scene3D& s) {
        cloud_sky_scene(s, clouds ? *clouds : Clouds{}, time);  // Clouds{}: coverage 0, the scene's default
        if (more) more(s);
        PostFx post = s.post();
        post.bloom = false;
        s.set_post(post);
    });
    return f.pixels;
}

// The horizontal pixel shift (-8..8) that best maps frame `a` onto frame `b` over the sky rows whose rays climb 0.3
// to 0.5 (mean |luma difference|): where the cloud pattern moved on screen between the two.
i32 best_shift(const Array<u8>& a, const Array<u8>& b, const Array<f32>& up) {
    i32 best = 0;
    f64 best_err = 1e30;
    for (i32 s = -8; s <= 8; ++s) {
        f64 err = 0.0;
        u32 n = 0;
        for (u32 y = 0; y < kH; ++y)
            for (u32 x = 10; x + 10 < kW; ++x) {
                const usize p = usize(y) * kW + x;
                if (up[p] < 0.3f || up[p] > 0.5f) continue;
                const usize q = usize(y) * kW + usize(i32(x) + s);
                err += std::fabs(luma(a, p * 4) - luma(b, q * 4));
                ++n;
            }
        err /= f64(std::max(n, 1u));
        if (err < best_err) {
            best_err = err;
            best = s;
        }
    }
    return best;
}

}  // namespace

TEST_CASE("render3d_gl: clouds at coverage 0, disabled or on the Low tier draw the cloudless frame bit for bit") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_sky(f, nullptr, 30.0f);
    const Clouds zero = windy(cloud_sky_clouds(0.0f), -0.3f, 0.8f);
    CHECK(same(draw_sky(f, &zero, 30.0f), none));
    Clouds off = cloud_sky_clouds(0.8f);
    off.enabled = false;
    CHECK(same(draw_sky(f, &off, 30.0f), none));
    const Clouds low = apply_quality(quality_settings(QualityTier::Low), cloud_sky_clouds(0.8f));
    CHECK(low.coverage == 0.0f);
    CHECK(same(draw_sky(f, &low, 30.0f), none));
    const Clouds nan = cloud_sky_clouds(std::nanf(""));  // the upload clamps again: NaN coverage is off
    CHECK(same(draw_sky(f, &nan, 30.0f), none));
    const Clouds high = apply_quality(quality_settings(QualityTier::High), cloud_sky_clouds(0.8f));
    CHECK_FALSE(same(draw_sky(f, &high, 30.0f), none));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: clouds change only the sky above the horizon, and more coverage covers more of it") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Array<u8> none = draw_sky(f, nullptr);
    const Array<f32> up = ray_heights(f);
    Change c[4];
    const f32 cov[4] = {0.2f, 0.4f, 0.7f, 1.0f};
    for (u32 i = 0; i < 4; ++i) {
        const Clouds cl = cloud_sky_clouds(cov[i]);
        c[i] = change(none, draw_sky(f, &cl), up);
    }
    MESSAGE("sky pixels " << c[0].sky << "; changed by more than 2 luma at coverage 0.2 / 0.4 / 0.7 / 1: " << c[0].above
                          << " / " << c[1].above << " / " << c[2].above << " / " << c[3].above << " (mean |dluma| "
                          << c[0].mean << " / " << c[1].mean << " / " << c[2].mean << " / " << c[3].mean
                          << "); pixels at or below the horizon changed: " << c[0].below << " / " << c[1].below << " / "
                          << c[2].below << " / " << c[3].below);
    REQUIRE(c[0].sky > 4000);                            // the camera looks up: most of the frame is sky
    for (u32 i = 0; i < 4; ++i) CHECK(c[i].below == 0);  // the ground, the box and the sky under the horizon
    CHECK(c[0].above > 0);
    for (u32 i = 0; i + 1 < 4; ++i) {
        CHECK(c[i + 1].above >= c[i].above);
        CHECK(c[i + 1].mean > c[i].mean);
    }
    CHECK(f64(c[0].above) < 0.5 * f64(c[0].sky));  // fair weather: broken clouds
    CHECK(f64(c[3].above) > 0.9 * f64(c[3].sky));  // overcast: a deck over the whole sky
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the cloud layer drifts with the scene clock along the wind; a 180 degree turn reverses it") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Clouds east = windy(cloud_sky_clouds(0.5f), 1.0f, 0.0f), west = windy(east, -1.0f, 0.0f);
    const Array<u8> t0 = draw_sky(f, &east, 0.0f);
    const Array<f32> up = ray_heights(f);
    CHECK(same(draw_sky(f, &west, 0.0f), t0));  // no drift yet: the wind's way does not matter
    const Array<u8> east10 = draw_sky(f, &east, 10.0f), west10 = draw_sky(f, &west, 10.0f);
    CHECK_FALSE(same(east10, t0));
    CHECK_FALSE(same(west10, t0));
    CHECK_FALSE(same(east10, west10));
    // The drift is wind_dir * speed * time: west at +10 s is east at -10 s, twice the speed is twice the time.
    CHECK(same(west10, draw_sky(f, &east, -10.0f)));
    Clouds fast = east;
    fast.speed = 2.0f;
    CHECK(same(draw_sky(f, &fast, 5.0f), east10));
    // On screen (the camera looks toward -z, +x to its right): a wind toward +x moves the pattern right, -x left.
    const i32 s_east = best_shift(t0, east10, up), s_west = best_shift(t0, west10, up);
    MESSAGE("pattern shift after 10 s (pixels): wind +x " << s_east << ", wind -x " << s_west);
    CHECK(s_east > 0);
    CHECK(s_west < 0);
    CHECK(s_east == -s_west);
    // A wind along z moves it along the view axis, neither right nor left.
    const Clouds north = windy(east, 0.0f, -1.0f);
    CHECK(std::abs(best_shift(t0, draw_sky(f, &north, 10.0f), up)) < s_east);
    // Speed 0 holds it still at any time.
    Clouds still = east;
    still.speed = 0.0f;
    CHECK(same(draw_sky(f, &still, 1234.5f), t0));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the cloud layer stays put across a 1000 m origin shift; a night sky stays dark") {
    Fixture3D f;
    cloud_sky_camera(f);
    const Clouds c = windy(cloud_sky_clouds(0.6f), 0.6f, 0.8f);
    const Array<u8> home = draw_sky(f, &c, 20.0f);
    const Array<f32> up = ray_heights(f);
    const Array<u8> home_bare = draw_sky(f, nullptr, 20.0f);
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    const auto rebase = [&](Scene3D& s) { s.set_origin_shift(shift); };
    // A floating-origin rebase (the renderer's origin moves 1000 m; the camera and the world stay where they are in
    // render space, as after a real rebase): the layer is not keyed to the origin, so the sky is the same bit for bit.
    const Array<u8> rebased = draw_sky(f, &c, 20.0f, rebase);
    CHECK(change(home, rebased, up, 0.0f).mean == 0.0);
    CHECK(change(home, draw_sky(f, &c, 20.0f), up, 0.0f).mean == 0.0);  // and the frame after it
    // The camera (with the world) 1000 m away in render space: the layer is camera-relative, so it stays where it
    // is in the sky too; only the f32 precision of the reconstructed view ray at 1000 m differs (the gradient sky
    // shows it too, less: the cloud edges are sharper than the gradient).
    f.world_offset = -shift;
    const Array<u8> shifted = draw_sky(f, &c, 20.0f, rebase);
    const Array<u8> shifted_bare = draw_sky(f, nullptr, 20.0f, rebase);
    f.world_offset = {0.0f, 0.0f, 0.0f};
    const Change moved = change(home, shifted, up), bare_moved = change(home_bare, shifted_bare, up);
    MESSAGE("camera 1000 m away: sky pixels moved by more than 2 luma "
            << moved.above << " of " << moved.sky << " (mean |dluma| " << moved.mean << "), cloudless "
            << bare_moved.above << " (" << bare_moved.mean << ")");
    CHECK(f64(moved.above) < 0.01 * f64(moved.sky));
    CHECK(moved.mean < 0.25);

    // Night: a dim moon-like light, a dark sky and ambient. An overcast deck stays dark (it is lit by the frame's
    // light and ambient, not by a fixed white).
    const auto night = [](Scene3D& s) {
        DirectionalLight moon;
        moon.direction = {-0.45f, -0.55f, 0.7f};
        moon.intensity = 0.05f;
        moon.ambient_sky = {0.01f, 0.012f, 0.02f};
        moon.ambient_ground = {0.005f, 0.005f, 0.005f};
        s.set_light(moon);
        Sky sky;
        sky.zenith = {0.004f, 0.005f, 0.012f};
        sky.horizon = {0.01f, 0.012f, 0.02f};
        sky.ground = {0.005f, 0.005f, 0.005f};
        s.set_sky(sky);
    };
    const Clouds deck = cloud_sky_clouds(0.95f);
    f64 day_mean, day_max, clear_mean, clear_max, night_mean, night_max;
    sky_luma(draw_sky(f, &deck, 0.0f), up, day_mean, day_max);
    sky_luma(draw_sky(f, nullptr, 0.0f, night), up, clear_mean, clear_max);
    sky_luma(draw_sky(f, &deck, 0.0f, night), up, night_mean, night_max);
    MESSAGE("overcast sky luma mean / max: day " << day_mean << " / " << day_max << ", night " << night_mean << " / "
                                                 << night_max << " (clear night " << clear_mean << " / " << clear_max
                                                 << ")");
    CHECK(night_mean < 15.0);
    CHECK(night_mean < 0.1 * day_mean);
    CHECK(night_max <= clear_max);  // the brightest night pixel is the moon's disk, not a cloud
    CHECK(f.device->validation_error_count() == 0);
}
