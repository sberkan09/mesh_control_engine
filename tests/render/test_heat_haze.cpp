// M-HEATHAZE 43b: the pure heat haze helpers (projection, frame, falloff, hash) and how the quality tiers and
// `r.heat_haze` map onto PostFx::haze. No GPU.

#include <doctest/doctest.h>

#include <limits>

#include "mce/convar.h"
#include "mce/render/heat_haze.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

// 90 degree vertical fov, aspect 1, at the origin looking down -z.
math::mat4 view_mat() {
    return math::look_at_rh(math::vec3{0.0f, 0.0f, 0.0f}, math::vec3{0.0f, 0.0f, -1.0f}, math::vec3{0.0f, 1.0f, 0.0f});
}
math::mat4 vp_mat() { return math::perspective_rh_no(1.5707963f, 1.0f, 0.1f, 100.0f) * view_mat(); }

HeatHazeVolume vol(math::vec3 c, f32 radius = 1.0f) {
    HeatHazeVolume v;
    v.center = c;
    v.radius = radius;
    return v;
}

HeatHaze one(const HeatHazeVolume& v) {
    HeatHaze h;
    h.count = 1;
    h.volumes[0] = v;
    return h;
}

ConVar& haze_cvar() {
    ConVar* cv = ConVar::find(StringId("r.heat_haze"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetHazeCvar {
    ~ResetHazeCvar() { haze_cvar().reset(); }
};

PostFx applied(QualityTier tier, const HeatHaze& haze) {
    PostFx post;
    post.haze = haze;
    ShadowSettings shadows;
    apply_quality(quality_settings(tier), shadows, post);
    return post;
}

HeatHaze many(u32 n) {
    HeatHaze h;
    h.count = n;
    for (u32 i = 0; i < kHeatHazeMaxVolumes; ++i) h.volumes[i] = vol(math::vec3{f32(i), 0.0f, -10.0f}, f32(i + 1));
    return h;
}

}  // namespace

TEST_CASE("heat haze: the hash goldens") {
    // Computed by the implementation and cross-checked with an independent Python port; the GLSL port must reproduce these
    // constant for constant. (0, 0, 0) hashes to 0: the lowbias32 mix has 0 as a fixed point.
    CHECK(heat_haze_hash(0, 0, 0) == 0x00000000u);
    CHECK(heat_haze_hash(1, 0, 0) == 0x24009c6du);
    CHECK(heat_haze_hash(0, 1, 0) == 0xa11e2aeau);
    CHECK(heat_haze_hash(0, 0, 1) == 0x61d77ce2u);
    CHECK(heat_haze_hash(1, 1, 1) == 0xc5f04049u);
    CHECK(heat_haze_hash(5, 7, 1) == 0x13162d94u);
    CHECK(heat_haze_hash(123, 456, 789) == 0x3bcce4c7u);
    CHECK(heat_haze_hash(0xffffffffu, 0xffffffffu, 0xffffffffu) == 0x828050a8u);
}

TEST_CASE("heat haze: the hash depends on every input and is stable") {
    const u32 h = heat_haze_hash(3, 4, 5);
    CHECK(h == heat_haze_hash(3, 4, 5));
    CHECK(h != heat_haze_hash(4, 4, 5));
    CHECK(h != heat_haze_hash(3, 5, 5));
    CHECK(h != heat_haze_hash(3, 4, 6));
    CHECK(heat_haze_hash(3, 4, 5) != heat_haze_hash(4, 3, 5));
}

TEST_CASE("heat haze: the falloff is 1 inside, 0 outside and soft between") {
    CHECK(heat_haze_falloff(0.0f, 0.5f) == 1.0f);
    CHECK(heat_haze_falloff(0.5f, 0.5f) == 1.0f);  // the rim starts at 1 - softness
    CHECK(heat_haze_falloff(1.0f, 0.5f) == 0.0f);
    CHECK(heat_haze_falloff(2.0f, 0.5f) == 0.0f);
    CHECK(heat_haze_falloff(0.75f, 0.5f) == doctest::Approx(0.5f));  // the middle of the smoothstep
    CHECK(heat_haze_falloff(0.0f, 1.0f) == 1.0f);
    CHECK(heat_haze_falloff(0.5f, 1.0f) == doctest::Approx(0.5f));
    f32 prev = 1.0f;
    for (int i = 0; i <= 40; ++i) {  // monotone non-increasing
        const f32 w = heat_haze_falloff(f32(i) * 0.05f, 0.6f);
        CHECK(w <= prev);
        CHECK(w >= 0.0f);
        CHECK(w <= 1.0f);
        prev = w;
    }
}

TEST_CASE("heat haze: the falloff edge cases") {
    CHECK(heat_haze_falloff(0.99f, 0.0f) == 1.0f);  // hard edge
    CHECK(heat_haze_falloff(1.0f, 0.0f) == 0.0f);
    CHECK(heat_haze_falloff(-3.0f, 0.5f) == 1.0f);  // negative distance counts as 0
    CHECK(heat_haze_falloff(kNaN, 0.5f) == 0.0f);
    CHECK(heat_haze_falloff(kInf, 0.5f) == 0.0f);
    CHECK(heat_haze_falloff(0.75f, kNaN) == doctest::Approx(0.5f));  // NaN softness = 0.5
    CHECK(heat_haze_falloff(0.0f, -1.0f) == 1.0f);                   // clamped to 0: hard edge
    CHECK(heat_haze_falloff(0.5f, 9.0f) == doctest::Approx(0.5f));   // clamped to 1
}

TEST_CASE("heat haze: a sphere straight ahead projects to the middle of the screen") {
    HazeScreen s;
    REQUIRE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, -10.0f}, 1.0f, s));
    CHECK(s.center.x == doctest::Approx(0.5f));
    CHECK(s.center.y == doctest::Approx(0.5f));
    // tan(asin(0.1)) = 0.1005; fov 90: ndc radius 0.1005, a fraction of the view height 0.0503.
    CHECK(s.radius == doctest::Approx(0.5f * 1.0f / std::sqrt(99.0f)).epsilon(1e-4));
    CHECK(s.near_depth == doctest::Approx(9.0f));
}

TEST_CASE("heat haze: the screen radius shrinks with distance and grows with the radius") {
    HazeScreen near_s, far_s, big;
    REQUIRE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, -5.0f}, 1.0f, near_s));
    REQUIRE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, -20.0f}, 1.0f, far_s));
    REQUIRE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, -20.0f}, 2.0f, big));
    CHECK(near_s.radius > far_s.radius);
    CHECK(big.radius > far_s.radius);
}

TEST_CASE("heat haze: an off-centre sphere lands off-centre in the right direction") {
    HazeScreen s;
    REQUIRE(heat_haze_project(vp_mat(), view_mat(), math::vec3{5.0f, 5.0f, -10.0f}, 1.0f, s));
    CHECK(s.center.x == doctest::Approx(0.75f));
    CHECK(s.center.y == doctest::Approx(0.75f));
    REQUIRE(heat_haze_project(vp_mat(), view_mat(), math::vec3{-5.0f, -5.0f, -10.0f}, 1.0f, s));
    CHECK(s.center.x == doctest::Approx(0.25f));
    CHECK(s.center.y == doctest::Approx(0.25f));
}

TEST_CASE("heat haze: culling") {
    HazeScreen s;
    CHECK_FALSE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, 10.0f}, 1.0f, s));    // behind the camera
    CHECK_FALSE(heat_haze_project(vp_mat(), view_mat(), math::vec3{50.0f, 0.0f, -10.0f}, 1.0f, s));  // off screen right
    CHECK_FALSE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, -50.0f, -10.0f}, 1.0f, s)); // off screen below
    CHECK(heat_haze_project(vp_mat(), view_mat(), math::vec3{11.0f, 0.0f, -10.0f}, 2.0f, s));        // edge overlaps the screen
    CHECK_FALSE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, -10.0f}, 0.0f, s));
    CHECK_FALSE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, -10.0f}, -1.0f, s));
    CHECK_FALSE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, -10.0f}, kNaN, s));
    CHECK_FALSE(heat_haze_project(vp_mat(), view_mat(), math::vec3{kNaN, 0.0f, -10.0f}, 1.0f, s));
    CHECK_FALSE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, kInf, -10.0f}, 1.0f, s));
    math::mat4 bad = vp_mat();
    bad.c[1][1] = kNaN;
    CHECK_FALSE(heat_haze_project(bad, view_mat(), math::vec3{0.0f, 0.0f, -10.0f}, 1.0f, s));
    // Centre behind the camera plane with the camera outside the sphere: dropped.
    CHECK_FALSE(heat_haze_project(vp_mat(), view_mat(), math::vec3{1.0f, 0.0f, 0.5f}, 1.1f, s));
}

TEST_CASE("heat haze: the nearest depth is clamped and a camera inside covers the screen") {
    HazeScreen s;
    REQUIRE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, -1.0f}, 0.98f, s));
    CHECK(s.near_depth == doctest::Approx(kHeatHazeNearPlane));  // 1.0 - 0.98 = 0.02 -> the near plane
    REQUIRE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, -0.5f}, 2.0f, s));
    CHECK(s.radius == 2.0f);
    CHECK(s.center.x == doctest::Approx(0.5f));
    CHECK(s.near_depth == doctest::Approx(kHeatHazeNearPlane));
    // Inside with the centre behind the camera: the middle of the screen.
    REQUIRE(heat_haze_project(vp_mat(), view_mat(), math::vec3{0.0f, 0.0f, 0.5f}, 2.0f, s));
    CHECK(s.center.x == 0.5f);
    CHECK(s.center.y == 0.5f);
    CHECK(s.radius == 2.0f);
}

TEST_CASE("heat haze: the frame keeps the visible volumes in order and clamps their look") {
    HeatHaze h;
    h.count = 3;
    h.volumes[0] = vol(math::vec3{0.0f, 0.0f, -10.0f});
    h.volumes[1] = vol(math::vec3{0.0f, 0.0f, 10.0f});  // behind the camera
    h.volumes[2] = vol(math::vec3{2.0f, 0.0f, -10.0f});
    h.volumes[0].strength = 5.0f;
    h.volumes[0].rise = -1.0f;
    h.volumes[0].scale = 100.0f;
    h.volumes[0].softness = 7.0f;
    h.volumes[2].strength = kNaN;
    h.volumes[2].rise = kNaN;
    h.volumes[2].scale = kNaN;
    h.volumes[2].softness = kNaN;
    HeatHazeFrame f;
    CHECK(heat_haze_frame(h, vp_mat(), view_mat(), 1.5f, 4.25f, f) == 2u);
    CHECK(f.count == 2u);
    CHECK(f.time == 4.25f);
    CHECK(f.aspect == 1.5f);
    CHECK(f.volumes[0].center.x == doctest::Approx(0.5f));
    CHECK(f.volumes[0].strength == 2.0f);
    CHECK(f.volumes[0].rise == 0.0f);
    CHECK(f.volumes[0].scale == 4.0f);
    CHECK(f.volumes[0].softness == 1.0f);
    CHECK(f.volumes[1].center.x > 0.5f);
    CHECK(f.volumes[1].strength == 0.5f);
    CHECK(f.volumes[1].rise == 0.6f);
    CHECK(f.volumes[1].scale == 1.0f);
    CHECK(f.volumes[1].softness == 0.5f);
}

TEST_CASE("heat haze: the frame is empty for the default setting and sanitizes time and aspect") {
    HeatHazeFrame f;
    CHECK(heat_haze_frame(HeatHaze{}, vp_mat(), view_mat(), 1.0f, 0.0f, f) == 0u);
    CHECK(f.count == 0u);
    const HeatHaze h = one(vol(math::vec3{0.0f, 0.0f, -10.0f}));
    CHECK(heat_haze_frame(h, vp_mat(), view_mat(), kNaN, kNaN, f) == 1u);
    CHECK(f.time == 0.0f);
    CHECK(f.aspect == 1.0f);
    heat_haze_frame(h, vp_mat(), view_mat(), -2.0f, kInf, f);
    CHECK(f.time == 0.0f);
    CHECK(f.aspect == 1.0f);
    heat_haze_frame(h, vp_mat(), view_mat(), 0.0f, 1.0f, f);
    CHECK(f.aspect == 1.0f);
}

TEST_CASE("heat haze: the frame reads at most 8 volumes and is deterministic") {
    HeatHaze h = many(8);
    for (u32 i = 0; i < 8; ++i) h.volumes[i].center = math::vec3{0.0f, 0.0f, -10.0f - f32(i)};
    h.count = 100;  // more than the array holds
    HeatHazeFrame a, b;
    CHECK(heat_haze_frame(h, vp_mat(), view_mat(), 1.0f, 2.0f, a) == 8u);
    heat_haze_frame(h, vp_mat(), view_mat(), 1.0f, 2.0f, b);
    CHECK(a.count == b.count);
    for (u32 i = 0; i < 8; ++i) {
        CHECK(a.volumes[i].center.x == b.volumes[i].center.x);
        CHECK(a.volumes[i].screen_radius == b.volumes[i].screen_radius);
        CHECK(a.volumes[i].near_depth == b.volumes[i].near_depth);
    }
}

TEST_CASE("heat haze: the quality tiers") {
    const QualitySettings low = quality_settings(QualityTier::Low);
    const QualitySettings med = quality_settings(QualityTier::Medium);
    const QualitySettings high = quality_settings(QualityTier::High);
    CHECK_FALSE(low.heat_haze);
    CHECK(low.heat_haze_volumes == 0u);
    CHECK(med.heat_haze);
    CHECK(med.heat_haze_volumes == 4u);
    CHECK(high.heat_haze);
    CHECK(high.heat_haze_volumes == 8u);
}

TEST_CASE("heat haze: apply_quality honors the tier and r.heat_haze and trims the count") {
    ResetHazeCvar reset;
    const HeatHaze h = many(8);
    CHECK(applied(QualityTier::Low, h).haze.count == 0u);
    CHECK(applied(QualityTier::Medium, h).haze.count == 4u);
    CHECK(applied(QualityTier::High, h).haze.count == 8u);
    CHECK(applied(QualityTier::High, many(3)).haze.count == 3u);
    CHECK(applied(QualityTier::High, HeatHaze{}).haze.count == 0u);
    CHECK(applied(QualityTier::High, many(100)).haze.count == 8u);
    // The first volumes survive the trim, in order.
    const PostFx med = applied(QualityTier::Medium, h);
    CHECK(med.haze.volumes[3].center.x == 3.0f);
    CHECK(med.haze.volumes[4].radius == 1.0f);  // beyond the count: reset to the default

    haze_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(applied(tier, h).haze.count == 0u);
    }
    haze_cvar().set_string("1");
    CHECK(applied(QualityTier::High, h).haze.count == 8u);
}

TEST_CASE("heat haze: apply_quality clamps the ranges and sanitizes NaN") {
    ResetHazeCvar reset;
    HeatHazeVolume v = vol(math::vec3{1.0f, 2.0f, 3.0f}, 1000.0f);
    v.strength = 9.0f;
    v.rise = 99.0f;
    v.scale = 0.0f;
    v.softness = 5.0f;
    HeatHazeVolume c = applied(QualityTier::High, one(v)).haze.volumes[0];
    CHECK(c.center.x == 1.0f);
    CHECK(c.center.y == 2.0f);
    CHECK(c.center.z == 3.0f);
    CHECK(c.radius == 50.0f);
    CHECK(c.strength == 2.0f);
    CHECK(c.rise == 4.0f);
    CHECK(c.scale == 0.25f);
    CHECK(c.softness == 1.0f);

    v.radius = 0.0f;
    v.strength = -1.0f;
    v.rise = -1.0f;
    v.scale = 100.0f;
    v.softness = -1.0f;
    c = applied(QualityTier::High, one(v)).haze.volumes[0];
    CHECK(c.radius == 0.05f);
    CHECK(c.strength == 0.0f);
    CHECK(c.rise == 0.0f);
    CHECK(c.scale == 4.0f);
    CHECK(c.softness == 0.0f);

    v.radius = v.strength = v.rise = v.scale = v.softness = kNaN;
    c = applied(QualityTier::High, one(v)).haze.volumes[0];
    const HeatHazeVolume d;
    CHECK(c.radius == d.radius);
    CHECK(c.strength == d.strength);
    CHECK(c.rise == d.rise);
    CHECK(c.scale == d.scale);
    CHECK(c.softness == d.softness);
}

TEST_CASE("heat haze: apply_quality drops a volume with a non-finite centre and closes up") {
    ResetHazeCvar reset;
    HeatHaze h;
    h.count = 4;
    for (u32 i = 0; i < 4; ++i) h.volumes[i] = vol(math::vec3{f32(i), 0.0f, 0.0f});
    h.volumes[1].center.y = kNaN;
    h.volumes[2].center.x = kInf;
    const HeatHaze c = applied(QualityTier::High, h).haze;
    CHECK(c.count == 2u);
    CHECK(c.volumes[0].center.x == 0.0f);
    CHECK(c.volumes[1].center.x == 3.0f);
    // A dropped volume does not use a slot of the tier cap.
    HeatHaze m;
    m.count = 6;
    for (u32 i = 0; i < 6; ++i) m.volumes[i] = vol(math::vec3{f32(i), 0.0f, 0.0f});
    m.volumes[0].center.x = kNaN;
    const HeatHaze mc = applied(QualityTier::Medium, m).haze;
    CHECK(mc.count == 4u);
    CHECK(mc.volumes[0].center.x == 1.0f);
}

TEST_CASE("heat haze: in-range values and the default pass through apply_quality unchanged") {
    ResetHazeCvar reset;
    HeatHazeVolume v = vol(math::vec3{1.0f, 2.0f, 3.0f}, 2.5f);
    v.strength = 1.25f;
    v.rise = 2.0f;
    v.scale = 1.5f;
    v.softness = 0.25f;
    const HeatHazeVolume c = applied(QualityTier::High, one(v)).haze.volumes[0];
    CHECK(c.center.x == 1.0f);
    CHECK(c.radius == 2.5f);
    CHECK(c.strength == 1.25f);
    CHECK(c.rise == 2.0f);
    CHECK(c.scale == 1.5f);
    CHECK(c.softness == 0.25f);
    CHECK(applied(QualityTier::High, HeatHaze{}).haze.count == 0u);
}
