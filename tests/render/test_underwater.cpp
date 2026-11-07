// M-UNDERWATER 44b: the pure underwater helpers (band, submersion, sigma, transmittance, scatter, light, window, lid
// distance, frame) and how the quality tiers and `r.underwater` map onto PostFx::underwater. No GPU.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/convar.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mce/render/underwater.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

ConVar& uw_cvar() {
    ConVar* cv = ConVar::find(StringId("r.underwater"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetUwCvar {
    ~ResetUwCvar() { uw_cvar().reset(); }
};

WaterSurface water_at(f32 level) {
    WaterSurface w;
    w.enabled = true;
    w.level_y = level;
    return w;
}

// A perspective camera at `pos` looking along -z (no rotation), 90 degree vertical fov.
View3D camera(math::vec3 pos) {
    View3D v;
    v.position = pos;
    v.view = math::look_at_rh(pos, pos + math::vec3{0.0f, 0.0f, -1.0f}, math::vec3{0.0f, 1.0f, 0.0f});
    v.proj = math::perspective_rh_no(1.5707963f, 1.0f, 0.1f, 100.0f);
    v.view_proj = v.proj * v.view;
    v.forward = math::vec3{0.0f, 0.0f, -1.0f};
    return v;
}

bool frame_of(const WaterSurface& w, const Underwater& u, const View3D& v, const f64 origin[3], UnderwaterFrame& out,
              f32 t = 1.0f, u32 detail = 2) {
    const DirectionalLight sun;
    return underwater_frame(w, u, v, origin, sun, t, 1.0f, detail, out);
}

PostFx applied(QualityTier tier, const Underwater& u) {
    PostFx post;
    post.underwater = u;
    ShadowSettings shadows;
    apply_quality(quality_settings(tier), shadows, post);
    return post;
}

}  // namespace

TEST_CASE("underwater: the band is automatic from the amplitude or the setting") {
    CHECK(underwater_band(0.4f, 0.0f) == doctest::Approx(0.6f));
    CHECK(underwater_band(0.0f, 0.0f) == 0.15f);   // lower clamp
    CHECK(underwater_band(10.0f, 0.0f) == 1.0f);   // upper clamp
    CHECK(underwater_band(0.4f, 0.3f) == 0.3f);    // the setting wins
    CHECK(underwater_band(0.4f, 99.0f) == 2.0f);
    CHECK(underwater_band(0.4f, 0.001f) == 0.05f);
    CHECK(underwater_band(kNaN, 0.0f) == 0.15f);
    CHECK(underwater_band(0.4f, kNaN) == doctest::Approx(0.6f));
    CHECK(underwater_band(0.4f, -1.0f) == doctest::Approx(0.6f));
}

TEST_CASE("underwater: submersion is 0 above the band, 1 below, monotone with no step") {
    const f32 band = 0.5f;
    CHECK(underwater_submersion(-band, band) == 0.0f);
    CHECK(underwater_submersion(-5.0f, band) == 0.0f);
    CHECK(underwater_submersion(0.0f, band) == doctest::Approx(0.5f));
    CHECK(underwater_submersion(band, band) == 1.0f);
    CHECK(underwater_submersion(5.0f, band) == 1.0f);
    f32 prev = 0.0f, worst = 0.0f;
    for (i32 i = -300; i <= 300; ++i) {  // 0.01 m steps from 3 m above to 3 m below
        const f32 s = underwater_submersion(f32(i) * 0.01f, band);
        CHECK(s >= 0.0f);
        CHECK(s <= 1.0f);
        CHECK(s >= prev);
        if (i > -300) worst = std::fmax(worst, s - prev);
        prev = s;
    }
    // The steepest slope of the smoothstep is 1.5 / (2 band) per metre: 1.5 per metre at band 0.5.
    CHECK(worst <= 1.5f * 0.01f / (2.0f * band) + 1.0e-4f);
    // Non-finite input: no NaN out.
    CHECK(underwater_submersion(kNaN, band) == doctest::Approx(0.5f));
    CHECK(underwater_submersion(0.3f, kNaN) == underwater_submersion(0.3f, 0.15f));
    CHECK(underwater_submersion(kInf, band) == 1.0f);
}

TEST_CASE("underwater: sigma orders red above blue for the default colours") {
    const WaterSurface w;
    const math::vec3 s = underwater_sigma(w.shallow_color, w.depth_scale_m, 1.0f);
    CHECK(s.x > s.y);
    CHECK(s.y > s.z);
    CHECK(s.z > 0.0f);
    CHECK(s.x == doctest::Approx(-std::log(0.10f) / 8.0f));
    // A larger fog_scale or depth scale lowers sigma; clamps and NaN.
    CHECK(underwater_sigma(w.shallow_color, 8.0f, 2.0f).x == doctest::Approx(s.x * 0.5f));
    CHECK(underwater_sigma(w.shallow_color, 16.0f, 1.0f).x == doctest::Approx(s.x * 0.5f));
    CHECK(underwater_sigma(w.shallow_color, kNaN, kNaN).x == doctest::Approx(s.x));
    CHECK(underwater_sigma(math::vec3{1.0f, 1.0f, 1.0f}, 8.0f, 1.0f).x == 0.0f);  // a clear channel does not absorb
    CHECK(underwater_sigma(math::vec3{0.0f, 0.0f, 0.0f}, 8.0f, 1.0f).x == doctest::Approx(-std::log(0.02f) / 8.0f));
}

TEST_CASE("underwater: transmittance is in 0..1 and falls with distance") {
    const WaterSurface w;
    const math::vec3 sigma = underwater_sigma(w.shallow_color, w.depth_scale_m, 1.0f);
    CHECK(underwater_transmittance(sigma, 0.0f).x == 1.0f);
    math::vec3 prev{1.0f, 1.0f, 1.0f};
    for (f32 d : {0.5f, 1.0f, 3.0f, 8.0f, 20.0f, 100.0f, 1.0e4f}) {
        const math::vec3 t = underwater_transmittance(sigma, d);
        for (f32 c : {t.x, t.y, t.z}) {
            CHECK(c >= 0.0f);
            CHECK(c <= 1.0f);
        }
        CHECK(t.x <= prev.x);
        CHECK(t.y <= prev.y);
        CHECK(t.z <= prev.z);
        CHECK(t.x <= t.y);  // red dies first
        CHECK(t.y <= t.z);
        prev = t;
    }
    // One absorption length leaves the shallow tint (a tolerance: exp is not bit-pinned).
    const math::vec3 one = underwater_transmittance(sigma, 8.0f);
    CHECK(one.x == doctest::Approx(0.10f).epsilon(1.0e-3));
    CHECK(one.y == doctest::Approx(0.38f).epsilon(1.0e-3));
    CHECK(one.z == doctest::Approx(0.40f).epsilon(1.0e-3));
    CHECK(underwater_transmittance(sigma, kNaN).x == 1.0f);
    CHECK(underwater_transmittance(sigma, -5.0f).x == 1.0f);
}

TEST_CASE("underwater: the scatter colour darkens with depth and with the light") {
    const WaterSurface w;
    const math::vec3 shallow = underwater_scatter(w, 0.0f, 1.0f);
    CHECK(shallow.x == doctest::Approx(w.shallow_color.x));
    CHECK(shallow.z == doctest::Approx(w.shallow_color.z));
    const math::vec3 deep = underwater_scatter(w, 1000.0f, 1.0f);
    CHECK(deep.x == doctest::Approx(w.deep_color.x).epsilon(1.0e-3));
    CHECK(deep.y < shallow.y);
    const math::vec3 dim = underwater_scatter(w, 4.0f, 0.5f), full = underwater_scatter(w, 4.0f, 1.0f);
    CHECK(dim.y == doctest::Approx(full.y * 0.5f));
    CHECK(underwater_scatter(w, -3.0f, 1.0f).y == shallow.y);  // above the surface counts as depth 0
    CHECK(underwater_scatter(w, kNaN, kNaN).y == doctest::Approx(shallow.y * 0.04f));
}

TEST_CASE("underwater: the light is dark at night and bright at noon") {
    DirectionalLight noon;
    noon.direction = math::vec3{0.0f, -1.0f, 0.0f};
    DirectionalLight night = noon;
    night.intensity = 0.0f;
    night.ambient_sky = math::vec3{0.0f, 0.0f, 0.0f};
    CHECK(underwater_light(night) == 0.04f);
    CHECK(underwater_light(noon) > 0.5f);
    CHECK(underwater_light(noon) <= 1.0f);
    DirectionalLight grazing = noon;
    grazing.direction = math::vec3{1.0f, 0.0f, 0.0f};
    CHECK(underwater_light(grazing) < underwater_light(noon));
    DirectionalLight below = noon;  // a sun under the horizon gives no sun term
    below.direction = math::vec3{0.0f, 1.0f, 0.0f};
    CHECK(underwater_light(below) == doctest::Approx(0.2126f * 0.35f + 0.7152f * 0.42f + 0.0722f * 0.55f));
    DirectionalLight bad = noon;
    bad.direction = math::vec3{kNaN, 0.0f, 0.0f};
    CHECK(std::isfinite(underwater_light(bad)));
}

TEST_CASE("underwater: the Snell window is 1 straight up and 0 horizontal") {
    CHECK(underwater_window(1.0f, 0.08f) == 1.0f);
    CHECK(underwater_window(0.0f, 0.08f) == 0.0f);
    CHECK(underwater_window(-1.0f, 0.08f) == 0.0f);
    CHECK(underwater_window(kUnderwaterWindowCos, 0.08f) == doctest::Approx(0.5f));
    CHECK(underwater_window(std::cos(1.0471976f), 0.08f) < 0.01f);  // 60 degrees off vertical: outside the cone
    f32 prev = 0.0f;
    for (i32 i = 0; i <= 100; ++i) {
        const f32 w = underwater_window(f32(i) * 0.01f, 0.08f);
        CHECK(w >= prev);
        prev = w;
    }
    CHECK(underwater_window(kNaN, 0.08f) == 0.0f);
    CHECK(underwater_window(1.0f, kNaN) == 1.0f);
}

TEST_CASE("underwater: the lid distance is depth over the up component") {
    CHECK(underwater_lid_distance(5.0f, 1.0f) == 5.0f);
    CHECK(underwater_lid_distance(5.0f, 0.5f) == doctest::Approx(10.0f));
    CHECK(underwater_lid_distance(5.0f, 0.0f) == 5000.0f);   // horizontal: depth / 1e-3
    CHECK(underwater_lid_distance(5.0f, -1.0f) == 5000.0f);  // down-going: the same (the pass never asks)
    CHECK(underwater_lid_distance(50.0f, 0.0f) == 1.0e4f);   // the far cap
    CHECK(underwater_lid_distance(0.0f, 0.5f) == 0.0f);
    CHECK(underwater_lid_distance(-2.0f, 0.5f) == 0.0f);
    CHECK(underwater_lid_distance(kNaN, 0.5f) == 0.0f);
    CHECK(underwater_lid_distance(5.0f, kNaN) == 5000.0f);
}

TEST_CASE("underwater: the frame is false above the band, for non-finite input and with the setting off") {
    const f64 origin[3] = {0.0, 0.0, 0.0};
    const Underwater u;
    UnderwaterFrame f;
    const WaterSurface w = water_at(0.0f);  // amplitude 0.4 -> band 0.6
    CHECK_FALSE(frame_of(w, u, camera({0.0f, 2.0f, 0.0f}), origin, f));
    CHECK_FALSE(frame_of(w, u, camera({0.0f, 0.6f, 0.0f}), origin, f));  // exactly at the band's top edge
    CHECK(frame_of(w, u, camera({0.0f, 0.0f, 0.0f}), origin, f));
    CHECK(f.submersion == doctest::Approx(0.5f));
    CHECK(frame_of(w, u, camera({0.0f, -5.0f, 0.0f}), origin, f));
    CHECK(f.submersion == 1.0f);
    CHECK(f.cam_depth == 5.0f);

    WaterSurface off = w;
    off.enabled = false;
    CHECK_FALSE(frame_of(off, u, camera({0.0f, -5.0f, 0.0f}), origin, f));
    WaterSurface bad = w;
    bad.level_y = kNaN;
    CHECK_FALSE(frame_of(bad, u, camera({0.0f, -5.0f, 0.0f}), origin, f));
    Underwater setting = u;
    setting.enabled = false;
    CHECK_FALSE(frame_of(w, setting, camera({0.0f, -5.0f, 0.0f}), origin, f));
    setting = u;
    setting.strength = 0.0f;
    CHECK_FALSE(frame_of(w, setting, camera({0.0f, -5.0f, 0.0f}), origin, f));
    View3D nan_cam = camera({0.0f, -5.0f, 0.0f});
    nan_cam.position.x = kNaN;
    CHECK_FALSE(frame_of(w, u, nan_cam, origin, f));
    View3D nan_view = camera({0.0f, -5.0f, 0.0f});
    nan_view.view.c[1][1] = kInf;
    CHECK_FALSE(frame_of(w, u, nan_view, origin, f));
    const f64 bad_origin[3] = {0.0, std::numeric_limits<f64>::quiet_NaN(), 0.0};
    CHECK_FALSE(frame_of(w, u, camera({0.0f, -5.0f, 0.0f}), bad_origin, f));
    View3D ortho = camera({0.0f, -5.0f, 0.0f});
    ortho.proj = math::ortho_rh_no(-5.0f, 5.0f, -5.0f, 5.0f, 0.1f, 100.0f);
    CHECK_FALSE(frame_of(w, u, ortho, origin, f));
}

TEST_CASE("underwater: the frame fills the pass inputs") {
    const f64 origin[3] = {0.0, 0.0, 0.0};
    const Underwater u;
    const WaterSurface w = water_at(0.0f);
    UnderwaterFrame f;
    REQUIRE(frame_of(w, u, camera({0.0f, -10.0f, 0.0f}), origin, f, 1.0f, 2));
    CHECK(f.slope_x == doctest::Approx(1.0f));  // 90 degree fov, aspect 1
    CHECK(f.slope_y == doctest::Approx(1.0f));
    CHECK(f.right.x == doctest::Approx(1.0f));
    CHECK(f.up.y == doctest::Approx(1.0f));
    CHECK(f.forward.z == doctest::Approx(-1.0f));
    CHECK(f.sigma.x > f.sigma.z);
    CHECK(f.detail == 2u);
    CHECK(f.wobble_amp > 0.0f);
    CHECK(f.lid.y > f.scatter.y);
    CHECK(f.aspect == 1.0f);
    for (f32 p : f.phase) {
        CHECK(p >= 0.0f);
        CHECK(p <= 6.2831855f);
    }
    // Detail 0: the flat tint, six metres of absorption, no wobble.
    UnderwaterFrame flat;
    REQUIRE(frame_of(w, u, camera({0.0f, -10.0f, 0.0f}), origin, flat, 1.0f, 0));
    CHECK(flat.detail == 0u);
    CHECK(flat.wobble_amp == 0.0f);
    CHECK(flat.sigma.x == doctest::Approx(f.sigma.x * 6.0f));
    // The detail is clamped to 2; the wobble follows the setting; the fog scale lowers sigma.
    UnderwaterFrame hi;
    REQUIRE(frame_of(w, u, camera({0.0f, -10.0f, 0.0f}), origin, hi, 1.0f, 99));
    CHECK(hi.detail == 2u);
    Underwater calm = u;
    calm.wobble = 0.0f;
    calm.fog_scale = 2.0f;
    UnderwaterFrame c;
    REQUIRE(frame_of(w, calm, camera({0.0f, -10.0f, 0.0f}), origin, c));
    CHECK(c.wobble_amp == 0.0f);
    CHECK(c.sigma.x == doctest::Approx(f.sigma.x * 0.5f));
    // strength scales the submersion.
    Underwater half = u;
    half.strength = 0.5f;
    UnderwaterFrame hs;
    REQUIRE(frame_of(w, half, camera({0.0f, -10.0f, 0.0f}), origin, hs));
    CHECK(hs.submersion == 0.5f);
    // A sanitized aspect.
    const DirectionalLight sun;
    REQUIRE(underwater_frame(w, u, camera({0.0f, -10.0f, 0.0f}), origin, sun, kNaN, -3.0f, 1u, f));
    CHECK(f.aspect == 1.0f);
}

TEST_CASE("underwater: the wobble phases are whole-cycle and the same clock gives the same frame") {
    const f64 origin[3] = {0.0, 0.0, 0.0};
    const Underwater u;
    const WaterSurface w = water_at(0.0f);
    const View3D cam = camera({0.0f, -10.0f, 0.0f});
    UnderwaterFrame a, b, later, other;
    REQUIRE(frame_of(w, u, cam, origin, a, 5.0f));
    REQUIRE(frame_of(w, u, cam, origin, b, 5.0f));
    REQUIRE(frame_of(w, u, cam, origin, later, 5.0f + 64.0f));  // one period later: the same phases
    REQUIRE(frame_of(w, u, cam, origin, other, 5.5f));
    for (usize i = 0; i < 4; ++i) {
        CHECK(a.phase[i] == b.phase[i]);
        CHECK(a.phase[i] == doctest::Approx(later.phase[i]).epsilon(1.0e-5));
    }
    bool differs = false;
    for (usize i = 0; i < 4; ++i) differs = differs || a.phase[i] != other.phase[i];
    CHECK(differs);
    // At t = 0 every phase is 0; at 64/3 s (a third of the first phase's cycle count 3 per 64 s: one cycle) it wraps to ~0.
    UnderwaterFrame zero;
    REQUIRE(frame_of(w, u, cam, origin, zero, 0.0f));
    for (f32 p : zero.phase) CHECK(p == 0.0f);
    UnderwaterFrame q;
    REQUIRE(frame_of(w, u, cam, origin, q, 16.0f));  // a quarter period: 3/4 cycle, 5/4 cycle ...
    CHECK(q.phase[0] == doctest::Approx(0.75f * 6.2831853f).epsilon(1.0e-5));
    CHECK(q.phase[1] == doctest::Approx(0.25f * 6.2831853f).epsilon(1.0e-4));
}

TEST_CASE("underwater: shifting the camera, the level and the origin together gives the same frame") {
    const Underwater u;
    const f64 o0[3] = {0.0, 0.0, 0.0};
    const f64 o1[3] = {128.0, 512.0, -64.0};
    UnderwaterFrame a, b;
    // The same world camera and level: the camera is render-relative, the level absolute.
    REQUIRE(frame_of(water_at(10.0f), u, camera({3.0f, 4.0f, 5.0f}), o0, a));
    REQUIRE(frame_of(water_at(10.0f), u, camera({3.0f - 128.0f, 4.0f - 512.0f, 5.0f + 64.0f}), o1, b));
    CHECK(a.cam_depth == b.cam_depth);
    CHECK(a.submersion == b.submersion);
    CHECK(a.scatter.y == b.scatter.y);
    CHECK(a.sigma.x == b.sigma.x);
    CHECK(a.phase[0] == b.phase[0]);
    // The level shifted with the origin too.
    UnderwaterFrame c;
    REQUIRE(frame_of(water_at(10.0f + 512.0f), u, camera({3.0f, 4.0f, 5.0f}), o1, c));
    CHECK(c.cam_depth == a.cam_depth);
    // And the crossing is decided by the render-relative height: above the band with the origin moved, false.
    CHECK_FALSE(frame_of(water_at(10.0f), u, camera({3.0f, -500.0f, 5.0f}), o1, c));
}

TEST_CASE("underwater: the crossing sweep has no step above a bound") {
    const f64 origin[3] = {0.0, 0.0, 0.0};
    const Underwater u;
    const WaterSurface w = water_at(0.0f);
    f32 prev = 0.0f, worst = 0.0f;
    for (i32 i = 30; i >= -30; --i) {  // 1.5 m above to 1.5 m below in 0.05 m steps
        UnderwaterFrame f;
        const f32 s = frame_of(w, u, camera({0.0f, f32(i) * 0.05f, 0.0f}), origin, f) ? f.submersion : 0.0f;
        if (i < 30) worst = std::fmax(worst, std::fabs(s - prev));
        prev = s;
    }
    CHECK(prev == 1.0f);
    CHECK(worst < 0.15f);
}

TEST_CASE("underwater: the quality tiers") {
    const QualitySettings low = quality_settings(QualityTier::Low);
    const QualitySettings med = quality_settings(QualityTier::Medium);
    const QualitySettings high = quality_settings(QualityTier::High);
    CHECK(low.underwater);
    CHECK(med.underwater);
    CHECK(high.underwater);
    CHECK(low.underwater_detail == 0u);
    CHECK(med.underwater_detail == 1u);
    CHECK(high.underwater_detail == 2u);
}

TEST_CASE("underwater: apply_quality honors the tier and r.underwater") {
    ResetUwCvar reset;
    const Underwater u;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(applied(tier, u).underwater.enabled);
    }
    Underwater off;
    off.enabled = false;
    CHECK_FALSE(applied(QualityTier::High, off).underwater.enabled);
    uw_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK_FALSE(applied(tier, u).underwater.enabled);
    }
    uw_cvar().set_string("1");
    CHECK(applied(QualityTier::High, u).underwater.enabled);
}

TEST_CASE("underwater: apply_quality clamps the ranges and sanitizes NaN") {
    ResetUwCvar reset;
    Underwater u;
    u.strength = 9.0f;
    u.wobble = 99.0f;
    u.fog_scale = 100.0f;
    u.band = 50.0f;
    PostFx p = applied(QualityTier::High, u);
    CHECK(p.underwater.strength == 1.0f);
    CHECK(p.underwater.wobble == 2.0f);
    CHECK(p.underwater.fog_scale == 4.0f);
    CHECK(p.underwater.band == 2.0f);

    u.strength = -1.0f;
    u.wobble = -1.0f;
    u.fog_scale = 0.0f;
    u.band = 0.001f;
    p = applied(QualityTier::High, u);
    CHECK(p.underwater.strength == 0.0f);
    CHECK(p.underwater.wobble == 0.0f);
    CHECK(p.underwater.fog_scale == 0.25f);
    CHECK(p.underwater.band == 0.05f);

    u.band = -1.0f;  // not positive: automatic
    CHECK(applied(QualityTier::High, u).underwater.band == 0.0f);

    u.strength = u.wobble = u.fog_scale = u.band = kNaN;
    p = applied(QualityTier::High, u);
    const Underwater d;
    CHECK(p.underwater.strength == d.strength);
    CHECK(p.underwater.wobble == d.wobble);
    CHECK(p.underwater.fog_scale == d.fog_scale);
    CHECK(p.underwater.band == d.band);
}

TEST_CASE("underwater: in-range values and the default pass through apply_quality unchanged") {
    ResetUwCvar reset;
    Underwater u;
    u.strength = 0.75f;
    u.wobble = 1.25f;
    u.fog_scale = 2.0f;
    u.band = 0.3f;
    const Underwater c = applied(QualityTier::High, u).underwater;
    CHECK(c.enabled);
    CHECK(c.strength == 0.75f);
    CHECK(c.wobble == 1.25f);
    CHECK(c.fog_scale == 2.0f);
    CHECK(c.band == 0.3f);
    const Underwater d = applied(QualityTier::High, Underwater{}).underwater;
    CHECK(d.strength == 1.0f);
    CHECK(d.band == 0.0f);
}
