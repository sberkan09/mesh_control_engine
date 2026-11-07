// M-SUNSHAFTS 45b: the pure sun shafts helpers (project, elevation and view fade, medium, tint, soft clamp, samples, hash,
// frame) and how the quality tiers and `r.sun_shafts` map onto PostFx::shafts. No GPU.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/convar.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mce/render/sun_shafts.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

ConVar& ss_cvar() {
    ConVar* cv = ConVar::find(StringId("r.sun_shafts"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetSsCvar {
    ~ResetSsCvar() { ss_cvar().reset(); }
};

// A camera at the origin looking along -z, 90 degree vertical fov, aspect 1.
math::mat4 camera_vp() {
    const math::mat4 view = math::look_at_rh(math::vec3{0, 0, 0}, math::vec3{0, 0, -1}, math::vec3{0, 1, 0});
    return math::perspective_rh_no(1.5707963f, 1.0f, 0.1f, 100.0f) * view;
}

// A sun light that travels along -toward.
DirectionalLight sun_toward(math::vec3 toward) {
    DirectionalLight l;
    l.direction = math::vec3{-toward.x, -toward.y, -toward.z};
    return l;
}

SunShafts on() {
    SunShafts s;
    s.enabled = true;
    return s;
}

bool frame_of(const SunShafts& s, math::vec3 toward, SunShaftsFrame& out, const Fog& fog = Fog{}, bool vol = false,
              u32 detail = 2, f32 submersion = 0.0f) {
    return sun_shafts_frame(s, sun_toward(toward), fog, Volumetrics{}, vol, camera_vp(), 1.0f, detail, submersion, out);
}

PostFx applied(QualityTier tier, const SunShafts& s) {
    PostFx post;
    post.shafts = s;
    ShadowSettings shadows;
    apply_quality(quality_settings(tier), shadows, post);
    return post;
}

}  // namespace

TEST_CASE("sun shafts: project gives the uv unclamped, and false behind the camera or for non-finite input") {
    math::vec2 uv{9, 9};
    f32 facing = -1;
    CHECK(sun_shafts_project(camera_vp(), math::vec3{0, 0, -1}, uv, facing));
    CHECK(uv.x == doctest::Approx(0.5f));
    CHECK(uv.y == doctest::Approx(0.5f));
    CHECK(facing == doctest::Approx(1.0f));
    // 90 degree fov: a direction at 45 degrees up lands on the top edge; the same length is normalized.
    CHECK(sun_shafts_project(camera_vp(), math::vec3{0, 3, -3}, uv, facing));
    CHECK(uv.y == doctest::Approx(1.0f));
    CHECK(facing == doctest::Approx(std::sqrt(0.5f)));
    // Off screen: the uv is outside 0..1 and not clamped to the screen.
    CHECK(sun_shafts_project(camera_vp(), math::vec3{1, 0, -0.5f}, uv, facing));
    CHECK(uv.x > 1.5f);
    CHECK(sun_shafts_project(camera_vp(), math::vec3{-1, 0, -0.5f}, uv, facing));
    CHECK(uv.x < -0.5f);
    // Almost on the side plane: finite and limited to +-4.
    CHECK(sun_shafts_project(camera_vp(), math::vec3{1, 0, -1.0e-6f}, uv, facing));
    CHECK(uv.x == kSunShaftsMaxUv);
    CHECK(std::isfinite(uv.y));
    // Behind the camera, on the side plane, a zero direction and non-finite input.
    CHECK_FALSE(sun_shafts_project(camera_vp(), math::vec3{0, 0, 1}, uv, facing));
    CHECK_FALSE(sun_shafts_project(camera_vp(), math::vec3{1, 0, 0}, uv, facing));
    CHECK_FALSE(sun_shafts_project(camera_vp(), math::vec3{0, 0, 0}, uv, facing));
    CHECK_FALSE(sun_shafts_project(camera_vp(), math::vec3{kNaN, 0, -1}, uv, facing));
    CHECK_FALSE(sun_shafts_project(camera_vp(), math::vec3{0, kInf, -1}, uv, facing));
    math::mat4 bad = camera_vp();
    bad.c[1][1] = kNaN;
    CHECK_FALSE(sun_shafts_project(bad, math::vec3{0, 0, -1}, uv, facing));
}

TEST_CASE("sun shafts: the elevation fade is 0 at the horizon and below, monotone to the knee, continuous") {
    auto fade = [](f32 y) { return sun_shafts_elevation_fade(math::vec3{0, y, 1}, 0.35f); };
    CHECK(fade(-0.02f) == 0.0f);
    CHECK(fade(-0.5f) == 0.0f);
    CHECK(fade(-1.0f) == 0.0f);
    CHECK(fade(0.04f) == 1.0f);
    CHECK(fade(0.35f) == 1.0f);
    CHECK(fade(0.9f) == doctest::Approx(0.35f));
    CHECK(fade(1.0f) == doctest::Approx(0.35f));
    CHECK(sun_shafts_elevation_fade(math::vec3{0, 1, 0}, 0.8f) == doctest::Approx(0.8f));
    CHECK(sun_shafts_elevation_fade(math::vec3{0, 1, 0}, kNaN) == doctest::Approx(0.35f));
    CHECK(sun_shafts_elevation_fade(math::vec3{0, kNaN, 0}, 0.35f) == 0.0f);
    CHECK(sun_shafts_elevation_fade(math::vec3{0, kInf, 0}, 0.35f) == 0.0f);
    f32 prev = 0.0f;
    for (int i = 0; i <= 62; ++i) {  // monotone up to the knee at 0.04, in steps of 0.001
        const f32 f = fade(-0.02f + f32(i) * 0.001f);
        CHECK(f >= prev);
        prev = f;
    }
    f32 last = fade(-0.1f);
    f32 worst = 0.0f;
    for (int i = 0; i <= 1100; ++i) {  // no step above a bound between adjacent elevations
        const f32 f = fade(-0.1f + f32(i) * 0.001f);
        worst = std::fmax(worst, std::fabs(f - last));
        last = f;
    }
    CHECK(worst < 0.05f);
}

TEST_CASE("sun shafts: the view fade is 0 behind and far off screen, 1 inside, without steps") {
    CHECK(sun_shafts_view_fade(math::vec2{0.5f, 0.5f}, 1.0f) == 1.0f);
    CHECK(sun_shafts_view_fade(math::vec2{0.0f, 1.0f}, 0.3f) == 1.0f);
    CHECK(sun_shafts_view_fade(math::vec2{0.5f, 0.5f}, 0.0f) == 0.0f);   // 90 degrees: gone
    CHECK(sun_shafts_view_fade(math::vec2{0.5f, 0.5f}, -0.5f) == 0.0f);  // behind
    CHECK(sun_shafts_view_fade(math::vec2{1.75f, 0.5f}, 1.0f) == 0.0f);  // three quarters of a screen out
    CHECK(sun_shafts_view_fade(math::vec2{4.0f, 4.0f}, 1.0f) == 0.0f);
    CHECK(sun_shafts_view_fade(math::vec2{1.0f, 0.5f}, 1.0f) == 1.0f);   // on the border: still full
    CHECK(sun_shafts_view_fade(math::vec2{1.375f, 0.5f}, 1.0f) == doctest::Approx(0.5f));
    CHECK(sun_shafts_view_fade(math::vec2{kNaN, 0.5f}, 1.0f) == 0.0f);
    CHECK(sun_shafts_view_fade(math::vec2{0.5f, 0.5f}, kNaN) == 0.0f);
    f32 last = sun_shafts_view_fade(math::vec2{-1.0f, 0.5f}, 1.0f);
    f32 worst = 0.0f;
    for (int i = 0; i <= 3000; ++i) {  // sweep the sun across and past the border
        const f32 f = sun_shafts_view_fade(math::vec2{-1.0f + f32(i) * 0.001f, 0.5f}, 1.0f);
        worst = std::fmax(worst, std::fabs(f - last));
        last = f;
    }
    CHECK(worst < 0.01f);
    last = sun_shafts_view_fade(math::vec2{0.5f, 0.5f}, -0.1f);
    worst = 0.0f;
    for (int i = 0; i <= 600; ++i) {  // facing sweeps from behind to straight ahead
        const f32 f = sun_shafts_view_fade(math::vec2{0.5f, 0.5f}, -0.1f + f32(i) * 0.002f);
        worst = std::fmax(worst, std::fabs(f - last));
        last = f;
    }
    CHECK(worst < 0.02f);
}

TEST_CASE("sun shafts: the medium has a floor on a clear day, grows with the density, and backs off with the volume") {
    const Volumetrics vol;
    Fog fog;
    fog.density = 0.0f;
    CHECK(sun_shafts_medium(fog, vol, false) == doctest::Approx(0.35f));
    f32 prev = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        fog.density = f32(i) * 0.001f;
        const f32 m = sun_shafts_medium(fog, vol, false);
        CHECK(m >= prev);
        CHECK(m >= 0.35f);
        CHECK(m <= 1.0f);
        prev = m;
    }
    fog.density = 0.05f;  // (0.05 * 40)^2 = 4
    CHECK(sun_shafts_medium(fog, vol, false) == doctest::Approx(0.35f + 0.65f * (1.0f - std::exp(-4.0f))));
    CHECK(sun_shafts_medium(fog, vol, true) == doctest::Approx(0.4f * sun_shafts_medium(fog, vol, false)));
    fog.density = -1.0f;
    CHECK(sun_shafts_medium(fog, vol, false) == doctest::Approx(0.35f));
    fog.density = 1.0e3f;
    CHECK(sun_shafts_medium(fog, vol, false) == doctest::Approx(1.0f));
    fog.density = kNaN;
    CHECK(sun_shafts_medium(fog, vol, false) == 0.0f);
    fog.density = kInf;
    CHECK(sun_shafts_medium(fog, vol, true) == 0.0f);
}

TEST_CASE("sun shafts: the tint changes the hue, not the brightness") {
    const math::vec3 white{1, 1, 1};
    const math::vec3 same = sun_shafts_tint(white, math::vec3{0.5f, 0.5f, 0.5f}, 1.0f);
    CHECK(same.x == doctest::Approx(1.0f));
    CHECK(same.y == doctest::Approx(1.0f));
    CHECK(same.z == doctest::Approx(1.0f));
    const math::vec3 blue = sun_shafts_tint(white, math::vec3{0.2f, 0.3f, 0.9f}, 1.0f);
    CHECK(blue.z > blue.y);
    CHECK(blue.y > blue.x);
    const f32 luma = 0.2126f * blue.x + 0.7152f * blue.y + 0.0722f * blue.z;
    CHECK(luma == doctest::Approx(1.0f));
    const math::vec3 none = sun_shafts_tint(white, math::vec3{0.2f, 0.3f, 0.9f}, 0.0f);  // no fog: the sun's own colour
    CHECK(none.x == 1.0f);
    CHECK(none.z == 1.0f);
    const math::vec3 bad = sun_shafts_tint(white, math::vec3{kNaN, 0, 0}, 1.0f);
    CHECK(bad.x == 0.0f);
    CHECK(bad.y == 0.0f);
    CHECK(bad.z == 0.0f);
    CHECK(sun_shafts_tint(white, white, kNaN).x == 0.0f);
}

TEST_CASE("sun shafts: the soft clamp is monotone, below cap and equal to x for small x") {
    CHECK(sun_shafts_soft_clamp(0.0f, 1.5f) == 0.0f);
    CHECK(sun_shafts_soft_clamp(1.0e-4f, 1.5f) == doctest::Approx(1.0e-4f).epsilon(1.0e-3));
    f32 prev = 0.0f;
    for (int i = 1; i <= 200; ++i) {
        const f32 y = sun_shafts_soft_clamp(f32(i) * 0.05f, 1.5f);  // up to x = 10 (strictly rising before f32 saturates)
        CHECK(y > prev);
        CHECK(y <= 1.5f);
        prev = y;
    }
    CHECK(sun_shafts_soft_clamp(1.0e9f, 1.5f) == doctest::Approx(1.5f));
    CHECK(sun_shafts_soft_clamp(kInf, 1.5f) == doctest::Approx(1.5f));
    CHECK(sun_shafts_soft_clamp(-3.0f, 1.5f) == 0.0f);
    CHECK(sun_shafts_soft_clamp(1.0f, 0.0f) == 0.0f);
    CHECK(sun_shafts_soft_clamp(1.0f, -1.0f) == 0.0f);
    CHECK(sun_shafts_soft_clamp(kNaN, 1.5f) == 0.0f);
    CHECK(sun_shafts_soft_clamp(1.0f, kNaN) == 0.0f);
    // C1: the slope is about 1 at 0 and falls continuously.
    const f32 h = 1.0e-3f;
    CHECK((sun_shafts_soft_clamp(h, 2.0f) - sun_shafts_soft_clamp(0.0f, 2.0f)) / h == doctest::Approx(1.0f).epsilon(1.0e-2));
}

TEST_CASE("sun shafts: the tiers' sample counts") {
    CHECK(sun_shafts_samples(0) == 0u);
    CHECK(sun_shafts_samples(1) == 16u);
    CHECK(sun_shafts_samples(2) == 32u);
    CHECK(sun_shafts_samples(3) == 32u);
    CHECK(sun_shafts_samples(0xffffffffu) == 32u);
}

TEST_CASE("sun shafts: the hash goldens") {
    // Same lowbias32 mix as heat_haze_hash, cross-checked with an independent Python port; the GLSL port must reproduce these
    // constant for constant. (0, 0, 0) hashes to 0: 0 is a fixed point of the mix.
    CHECK(sun_shafts_hash(0, 0, 0) == 0x00000000u);
    CHECK(sun_shafts_hash(1, 0, 0) == 0x24009c6du);
    CHECK(sun_shafts_hash(0, 1, 0) == 0xa11e2aeau);
    CHECK(sun_shafts_hash(0, 0, 1) == 0x61d77ce2u);
    CHECK(sun_shafts_hash(1, 1, 1) == 0xc5f04049u);
    CHECK(sun_shafts_hash(5, 7, 1) == 0x13162d94u);
    CHECK(sun_shafts_hash(123, 456, 789) == 0x3bcce4c7u);
    CHECK(sun_shafts_hash(31, 17, 21320) == 0x7c305258u);
    CHECK(sun_shafts_hash(0xffffffffu, 0xffffffffu, 0xffffffffu) == 0x828050a8u);
    const u32 h = sun_shafts_hash(3, 4, 5);
    CHECK(h == sun_shafts_hash(3, 4, 5));
    CHECK(h != sun_shafts_hash(4, 4, 5));
    CHECK(h != sun_shafts_hash(3, 5, 5));
    CHECK(h != sun_shafts_hash(3, 4, 6));
    CHECK(sun_shafts_hash(3, 4, 5) != sun_shafts_hash(4, 3, 5));
}

TEST_CASE("sun shafts: the frame is false for every off case and for non-finite input") {
    SunShaftsFrame f;
    const math::vec3 ahead{0.0f, 0.3f, -1.0f};
    REQUIRE(frame_of(on(), ahead, f));
    CHECK(f.samples == 32u);
    CHECK(f.strength > 0.0f);
    CHECK(f.sun_uv.x == doctest::Approx(0.5f));

    CHECK_FALSE(frame_of(SunShafts{}, ahead, f));  // the default setting is off
    SunShafts s = on();
    s.intensity = 0.0f;
    CHECK_FALSE(frame_of(s, ahead, f));
    s = on();
    s.visibility = 0.0f;
    CHECK_FALSE(frame_of(s, ahead, f));
    CHECK_FALSE(frame_of(on(), ahead, f, Fog{}, false, 0));                        // the Low tier has no samples
    CHECK_FALSE(frame_of(on(), math::vec3{0.0f, -0.3f, -1.0f}, f));                 // below the horizon
    CHECK_FALSE(frame_of(on(), math::vec3{0.0f, 0.3f, 1.0f}, f));                   // behind the camera
    CHECK_FALSE(frame_of(on(), math::vec3{0.0f, 0.3f, -1.0f}, f, Fog{}, false, 2, 1.0f));  // fully submerged
    CHECK_FALSE(frame_of(on(), ahead, f, Fog{}, false, 2, kNaN));
    CHECK_FALSE(frame_of(on(), math::vec3{kNaN, 0.3f, -1.0f}, f));
    CHECK_FALSE(frame_of(on(), math::vec3{0.0f, 0.0f, 0.0f}, f));
    Fog nan_fog;
    nan_fog.density = kNaN;
    CHECK_FALSE(frame_of(on(), ahead, f, nan_fog));
    DirectionalLight bad_color = sun_toward(ahead);
    bad_color.color = math::vec3{kInf, 1, 1};
    CHECK_FALSE(sun_shafts_frame(on(), bad_color, Fog{}, Volumetrics{}, false, camera_vp(), 1.0f, 2, 0.0f, f));
    math::mat4 bad = camera_vp();
    bad.c[0][0] = kNaN;
    CHECK_FALSE(sun_shafts_frame(on(), sun_toward(ahead), Fog{}, Volumetrics{}, false, bad, 1.0f, 2, 0.0f, f));
    // A refused call leaves `out` untouched.
    SunShaftsFrame keep;
    keep.samples = 7;
    CHECK_FALSE(frame_of(SunShafts{}, ahead, keep));
    CHECK(keep.samples == 7u);
}

TEST_CASE("sun shafts: the frame content, the tier's samples and the volume back-off") {
    SunShaftsFrame hi, med, vol, sub;
    const math::vec3 ahead{0.0f, 0.3f, -1.0f};
    REQUIRE(frame_of(on(), ahead, hi, Fog{}, false, 2));
    REQUIRE(frame_of(on(), ahead, med, Fog{}, false, 1));
    CHECK(med.samples == 16u);
    CHECK(hi.strength == med.strength);
    CHECK(hi.length == 0.85f);
    CHECK(hi.decay == 0.97f);
    CHECK(hi.threshold == 1.0f);
    CHECK(hi.cap == 1.5f);
    CHECK(hi.scatter_m == 60.0f);
    CHECK(hi.aspect == 1.0f);
    CHECK(hi.strength == doctest::Approx(0.35f));  // elevation 1 (y about 0.29), view 1, clear-day medium, visibility 1
    CHECK(hi.color.x == doctest::Approx(hi.strength * 1.0f));
    REQUIRE(frame_of(on(), ahead, vol, Fog{}, true));
    CHECK(vol.strength == doctest::Approx(0.4f * hi.strength));
    REQUIRE(frame_of(on(), ahead, sub, Fog{}, false, 2, 0.5f));
    CHECK(sub.strength == doctest::Approx(0.5f * hi.strength));
    SunShafts s = on();
    s.intensity = 2.0f;
    SunShaftsFrame two;
    REQUIRE(frame_of(s, ahead, two));
    CHECK(two.color.x == doctest::Approx(2.0f * hi.color.x));
    CHECK(two.strength == hi.strength);  // the intensity is in the colour, not the strength
    // An aspect that is NaN or not positive counts as 1.
    SunShaftsFrame a;
    REQUIRE(sun_shafts_frame(on(), sun_toward(ahead), Fog{}, Volumetrics{}, false, camera_vp(), kNaN, 2, 0.0f, a));
    CHECK(a.aspect == 1.0f);
    REQUIRE(sun_shafts_frame(on(), sun_toward(ahead), Fog{}, Volumetrics{}, false, camera_vp(), 2.0f, 2, 0.0f, a));
    CHECK(a.aspect == 2.0f);
}

TEST_CASE("sun shafts: the same inputs give the same frame, and the strength has no pop across a sweep") {
    SunShaftsFrame a, b;
    const math::vec3 ahead{0.2f, 0.25f, -1.0f};
    REQUIRE(frame_of(on(), ahead, a));
    REQUIRE(frame_of(on(), ahead, b));
    CHECK(a.sun_uv.x == b.sun_uv.x);
    CHECK(a.sun_uv.y == b.sun_uv.y);
    CHECK(a.strength == b.strength);
    CHECK(a.color.x == b.color.x);
    CHECK(a.color.y == b.color.y);
    CHECK(a.color.z == b.color.z);
    // The sun sweeps from straight ahead to the right and around behind the camera: the strength is continuous until it
    // reaches 0 (the pass then stops recording), with no step above a bound.
    f32 last = -1.0f;
    f32 worst = 0.0f;
    for (int i = 0; i <= 720; ++i) {
        const f32 ang = f32(i) * 0.25f * 3.14159265f / 180.0f;  // 0..180 degrees
        SunShaftsFrame f;
        const bool ok = frame_of(on(), math::vec3{std::sin(ang), 0.25f, -std::cos(ang)}, f);
        const f32 str = ok ? f.strength : 0.0f;
        if (last >= 0.0f) worst = std::fmax(worst, std::fabs(str - last));
        last = str;
    }
    CHECK(worst < 0.02f);
    CHECK(last == 0.0f);  // behind the camera
    // And up through the horizon.
    last = -1.0f;
    worst = 0.0f;
    for (int i = 0; i <= 400; ++i) {
        SunShaftsFrame f;
        const bool ok = frame_of(on(), math::vec3{0.0f, -0.1f + f32(i) * 0.001f, -1.0f}, f);
        const f32 str = ok ? f.strength : 0.0f;
        if (last >= 0.0f) worst = std::fmax(worst, std::fabs(str - last));
        last = str;
    }
    CHECK(worst < 0.02f);
}

TEST_CASE("sun shafts: the quality tiers") {
    const QualitySettings low = quality_settings(QualityTier::Low);
    const QualitySettings med = quality_settings(QualityTier::Medium);
    const QualitySettings high = quality_settings(QualityTier::High);
    CHECK_FALSE(low.sun_shafts);
    CHECK(med.sun_shafts);
    CHECK(high.sun_shafts);
    CHECK(low.sun_shafts_samples == 0u);
    CHECK(med.sun_shafts_samples == 16u);
    CHECK(high.sun_shafts_samples == 32u);
    CHECK(sun_shafts_samples(1) == med.sun_shafts_samples);
    CHECK(sun_shafts_samples(2) == high.sun_shafts_samples);
}

TEST_CASE("sun shafts: apply_quality honors the tier and r.sun_shafts") {
    ResetSsCvar reset;
    const SunShafts s = on();
    CHECK_FALSE(applied(QualityTier::Low, s).shafts.enabled);
    CHECK(applied(QualityTier::Medium, s).shafts.enabled);
    CHECK(applied(QualityTier::High, s).shafts.enabled);
    CHECK_FALSE(applied(QualityTier::High, SunShafts{}).shafts.enabled);  // off stays off
    ss_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK_FALSE(applied(tier, s).shafts.enabled);
    }
    ss_cvar().set_string("1");
    CHECK(applied(QualityTier::High, s).shafts.enabled);
}

TEST_CASE("sun shafts: apply_quality clamps the ranges and sanitizes NaN") {
    ResetSsCvar reset;
    SunShafts s = on();
    s.intensity = 99.0f;
    s.length = 5.0f;
    s.decay = 0.0f;
    s.threshold = 1000.0f;
    s.cap = 100.0f;
    s.visibility = 3.0f;
    s.scatter_m = 1.0e6f;
    SunShafts c = applied(QualityTier::High, s).shafts;
    CHECK(c.intensity == 4.0f);
    CHECK(c.length == 1.0f);
    CHECK(c.decay == 0.8f);
    CHECK(c.threshold == 16.0f);
    CHECK(c.cap == 8.0f);
    CHECK(c.visibility == 1.0f);
    CHECK(c.scatter_m == 500.0f);
    s.intensity = -1.0f;
    s.length = -1.0f;
    s.decay = 2.0f;
    s.threshold = 0.0f;
    s.cap = -1.0f;
    s.visibility = -1.0f;
    s.scatter_m = 0.0f;
    c = applied(QualityTier::High, s).shafts;
    CHECK(c.intensity == 0.0f);
    CHECK(c.length == 0.1f);
    CHECK(c.decay == 1.0f);
    CHECK(c.threshold == 0.05f);
    CHECK(c.cap == 0.0f);
    CHECK(c.visibility == 0.0f);
    CHECK(c.scatter_m == 5.0f);
    s.intensity = s.length = s.decay = s.threshold = s.cap = s.visibility = s.scatter_m = kNaN;
    c = applied(QualityTier::High, s).shafts;
    CHECK(c.enabled);
    CHECK(c.intensity == 1.0f);
    CHECK(c.length == 0.85f);
    CHECK(c.decay == 0.97f);
    CHECK(c.threshold == 1.0f);
    CHECK(c.cap == 1.5f);
    CHECK(c.visibility == 1.0f);
    CHECK(c.scatter_m == 60.0f);
}

TEST_CASE("sun shafts: in-range values and the default pass through apply_quality unchanged") {
    ResetSsCvar reset;
    SunShafts s = on();
    s.intensity = 2.0f;
    s.length = 0.5f;
    s.decay = 0.9f;
    s.threshold = 4.0f;
    s.cap = 3.0f;
    s.visibility = 0.25f;
    s.scatter_m = 120.0f;
    const SunShafts c = applied(QualityTier::High, s).shafts;
    CHECK(c.enabled);
    CHECK(c.intensity == 2.0f);
    CHECK(c.length == 0.5f);
    CHECK(c.decay == 0.9f);
    CHECK(c.threshold == 4.0f);
    CHECK(c.cap == 3.0f);
    CHECK(c.visibility == 0.25f);
    CHECK(c.scatter_m == 120.0f);
    const SunShafts d = applied(QualityTier::High, SunShafts{}).shafts;
    CHECK_FALSE(d.enabled);
    CHECK(d.intensity == 1.0f);
    CHECK(d.length == 0.85f);
    CHECK(d.decay == 0.97f);
    CHECK(d.threshold == 1.0f);
    CHECK(d.cap == 1.5f);
    CHECK(d.visibility == 1.0f);
    CHECK(d.scatter_m == 60.0f);
}
