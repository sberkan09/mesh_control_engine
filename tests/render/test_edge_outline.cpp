// M-OUTLINE 39b: the pure edge outline helpers (tap table and count, the depth, normal and fade terms, the frame) and
// how the quality tiers, `r.edge_outline` and the range clamps map onto PostFx::outline. No GPU.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/convar.h"
#include "mce/render/edge_outline.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

EdgeOutline on() {
    EdgeOutline o;
    o.enabled = true;
    return o;
}

bool is_zero_frame(const EdgeOutlineFrame& f) {
    return same_bits(f.strength, 0.0f) && f.taps == 0u && same_bits(f.color_opacity.w, 0.0f);
}

ConVar& outline_cvar() {
    ConVar* cv = ConVar::find(StringId("r.edge_outline"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetOutlineCvar {
    ~ResetOutlineCvar() { outline_cvar().reset(); }
};

PostFx applied(QualityTier tier, const EdgeOutline& outline) {
    PostFx post;
    post.outline = outline;
    ShadowSettings shadows;
    apply_quality(quality_settings(tier), shadows, post);
    return post;
}

f32 smooth(f32 a, f32 b, f32 x) {
    f32 t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

TEST_CASE("edge outline: the tap table is the cross first, then the diagonals, all unit length") {
    static_assert(kEdgeOutlineMaxTaps == 8);
    CHECK(kEdgeOutlineTaps[0].x == 1.0f);
    CHECK(kEdgeOutlineTaps[0].y == 0.0f);
    CHECK(kEdgeOutlineTaps[1].x == -1.0f);
    CHECK(kEdgeOutlineTaps[1].y == 0.0f);
    CHECK(kEdgeOutlineTaps[2].x == 0.0f);
    CHECK(kEdgeOutlineTaps[2].y == 1.0f);
    CHECK(kEdgeOutlineTaps[3].x == 0.0f);
    CHECK(kEdgeOutlineTaps[3].y == -1.0f);
    for (u32 k = 0; k < kEdgeOutlineMaxTaps; ++k) {
        INFO("tap " << k);
        const f32 len = std::sqrt(kEdgeOutlineTaps[k].x * kEdgeOutlineTaps[k].x + kEdgeOutlineTaps[k].y * kEdgeOutlineTaps[k].y);
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-6));
    }
    for (u32 k = 4; k < kEdgeOutlineMaxTaps; ++k) {
        CHECK(std::fabs(kEdgeOutlineTaps[k].x) == doctest::Approx(0.70710678f));
        CHECK(std::fabs(kEdgeOutlineTaps[k].y) == doctest::Approx(0.70710678f));
    }
    // The four diagonals cover the four quadrants.
    CHECK(kEdgeOutlineTaps[4].x > 0.0f);
    CHECK(kEdgeOutlineTaps[4].y > 0.0f);
    CHECK(kEdgeOutlineTaps[5].x < 0.0f);
    CHECK(kEdgeOutlineTaps[5].y > 0.0f);
    CHECK(kEdgeOutlineTaps[6].x > 0.0f);
    CHECK(kEdgeOutlineTaps[6].y < 0.0f);
    CHECK(kEdgeOutlineTaps[7].x < 0.0f);
    CHECK(kEdgeOutlineTaps[7].y < 0.0f);
}

TEST_CASE("edge outline: the tap count is 4 or 8") {
    CHECK(edge_outline_tap_count(false) == 4u);
    CHECK(edge_outline_tap_count(true) == 8u);
}

TEST_CASE("edge outline: depth term, sky rules") {
    // A sky center never draws; a sky tap draws a silhouette only with sky_silhouette.
    CHECK(edge_outline_depth_term(0.0f, 10.0f, 0.05f, 1.0f, true) == 0.0f);
    CHECK(edge_outline_depth_term(0.0f, 0.0f, 0.05f, 1.0f, true) == 0.0f);
    CHECK(edge_outline_depth_term(5.0f, 0.0f, 0.05f, 1.0f, true) == 1.0f);
    CHECK(edge_outline_depth_term(5.0f, 0.0f, 0.05f, 1.0f, false) == 0.0f);
    CHECK(edge_outline_depth_term(kNaN, 5.0f, 0.05f, 1.0f, true) == 0.0f);
    CHECK(edge_outline_depth_term(5.0f, kNaN, 0.05f, 1.0f, true) == 0.0f);
}

TEST_CASE("edge outline: depth term draws on the nearer side only") {
    CHECK(edge_outline_depth_term(10.0f, 20.0f, 0.05f, 1.0f, true) == 1.0f);  // the tap is farther: the center is in front
    CHECK(edge_outline_depth_term(20.0f, 10.0f, 0.05f, 1.0f, true) == 0.0f);  // the tap is nearer: no doubling
    CHECK(edge_outline_depth_term(10.0f, 10.0f, 0.05f, 1.0f, true) == 0.0f);
}

TEST_CASE("edge outline: depth term smoothstep edges at threshold and 2 * threshold") {
    const f32 t = 0.05f;
    // relative jump = (d_tap - d_center) / d_center with n_dot_v 1
    CHECK(edge_outline_depth_term(10.0f, 10.0f * (1.0f + 0.5f * t), t, 1.0f, true) == 0.0f);  // below the threshold
    CHECK(edge_outline_depth_term(10.0f, 10.0f * (1.0f + 2.0f * t), t, 1.0f, true) == doctest::Approx(1.0f));
    CHECK(edge_outline_depth_term(10.0f, 10.0f * (1.0f + 3.0f * t), t, 1.0f, true) == 1.0f);
    CHECK(edge_outline_depth_term(10.0f, 10.0f * (1.0f + 1.5f * t), t, 1.0f, true) == doctest::Approx(0.5f).epsilon(1e-4));
    const f32 mid = edge_outline_depth_term(10.0f, 10.0f * (1.0f + 1.25f * t), t, 1.0f, true);
    CHECK(mid > 0.0f);
    CHECK(mid < 0.5f);
    f32 prev = 0.0f;
    for (f32 r = 0.0f; r <= 0.12f; r += 0.005f) {
        const f32 v = edge_outline_depth_term(10.0f, 10.0f * (1.0f + r), t, 1.0f, true);
        CHECK(v >= prev);
        CHECK(v <= 1.0f);
        prev = v;
    }
    CHECK(prev == 1.0f);
}

TEST_CASE("edge outline: depth term grazing angle raises the effective threshold") {
    const f32 t = 0.05f;
    const f32 d_tap = 10.0f * (1.0f + 1.5f * t);
    const f32 facing = edge_outline_depth_term(10.0f, d_tap, t, 1.0f, true);
    const f32 grazing = edge_outline_depth_term(10.0f, d_tap, t, 0.2f, true);
    CHECK(facing == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(grazing < facing);
    CHECK(grazing == doctest::Approx(smooth(t, 2.0f * t, 1.5f * t * 0.2f)));
    // n_dot_v below 0.2 (and negative) is clamped to 0.2.
    CHECK(same_bits(edge_outline_depth_term(10.0f, d_tap, t, 0.0f, true), grazing));
    CHECK(same_bits(edge_outline_depth_term(10.0f, d_tap, t, -1.0f, true), grazing));
    // A grazing floor with a modest jump draws nothing, a facing surface with the same jump does.
    const f32 small_jump = 10.0f * (1.0f + 1.2f * t);
    CHECK(edge_outline_depth_term(10.0f, small_jump, t, 0.2f, true) == 0.0f);
    CHECK(edge_outline_depth_term(10.0f, small_jump, t, 1.0f, true) > 0.0f);
}

TEST_CASE("edge outline: normal term") {
    const math::vec3 up{0.0f, 1.0f, 0.0f};
    const math::vec3 side{1.0f, 0.0f, 0.0f};
    const math::vec3 sky{0.0f, 0.0f, 0.0f};
    CHECK(edge_outline_normal_term(up, up, 0.4f) == 0.0f);
    CHECK(edge_outline_normal_term(up, side, 0.4f) == 1.0f);  // 1 - dot = 1 >= 1.5 * 0.4
    CHECK(edge_outline_normal_term(up, math::vec3{0.0f, -1.0f, 0.0f}, 0.4f) == 1.0f);
    // Edges of the smoothstep: 1 - dot = threshold and 1.5 * threshold.
    const f32 t = 0.4f;
    const math::vec3 at_t{std::sqrt(1.0f - (1.0f - t) * (1.0f - t)), 1.0f - t, 0.0f};
    CHECK(edge_outline_normal_term(up, at_t, t) == doctest::Approx(0.0f).epsilon(1e-3));
    const math::vec3 at_15{std::sqrt(1.0f - (1.0f - 1.5f * t) * (1.0f - 1.5f * t)), 1.0f - 1.5f * t, 0.0f};
    CHECK(edge_outline_normal_term(up, at_15, t) == doctest::Approx(1.0f).epsilon(1e-3));
    const math::vec3 mid{std::sqrt(1.0f - (1.0f - 1.25f * t) * (1.0f - 1.25f * t)), 1.0f - 1.25f * t, 0.0f};
    CHECK(edge_outline_normal_term(up, mid, t) == doctest::Approx(0.5f).epsilon(1e-3));
    // Sky on either side draws nothing.
    CHECK(edge_outline_normal_term(sky, up, 0.4f) == 0.0f);
    CHECK(edge_outline_normal_term(up, sky, 0.4f) == 0.0f);
    CHECK(edge_outline_normal_term(sky, sky, 0.4f) == 0.0f);
    CHECK(edge_outline_normal_term(math::vec3{kNaN, 0.0f, 0.0f}, up, 0.4f) == 0.0f);
}

TEST_CASE("edge outline: fade") {
    CHECK(edge_outline_fade(0.0f, 30.0f, 80.0f, 0.0f) == 1.0f);
    CHECK(edge_outline_fade(30.0f, 30.0f, 80.0f, 0.0f) == 1.0f);
    CHECK(edge_outline_fade(80.0f, 30.0f, 80.0f, 0.0f) == 0.0f);
    CHECK(edge_outline_fade(500.0f, 30.0f, 80.0f, 0.0f) == 0.0f);
    CHECK(edge_outline_fade(55.0f, 30.0f, 80.0f, 0.0f) == doctest::Approx(0.5f));
    f32 prev = 1.0f;
    for (f32 d = 30.0f; d <= 80.0f; d += 2.5f) {
        const f32 v = edge_outline_fade(d, 30.0f, 80.0f, 0.0f);
        CHECK(v <= prev);
        CHECK(v >= 0.0f);
        prev = v;
    }
    // The exp^2 fog factor: exp(-(density * d)^2), before the fade starts.
    CHECK(edge_outline_fade(10.0f, 30.0f, 80.0f, 0.05f) == doctest::Approx(std::exp(-0.25f)));
    CHECK(edge_outline_fade(20.0f, 30.0f, 80.0f, 0.05f) < edge_outline_fade(10.0f, 30.0f, 80.0f, 0.05f));
    CHECK(edge_outline_fade(55.0f, 30.0f, 80.0f, 0.02f) ==
          doctest::Approx(0.5f * std::exp(-(0.02f * 55.0f) * (0.02f * 55.0f))));
    CHECK(edge_outline_fade(kNaN, 30.0f, 80.0f, 0.0f) == 0.0f);
    CHECK(edge_outline_fade(10.0f, 30.0f, 80.0f, kNaN) == 1.0f);  // a NaN fog density counts as 0
    CHECK(edge_outline_fade(kInf, 30.0f, 80.0f, 0.0f) == 0.0f);
}

TEST_CASE("edge outline: the default setting gives a frame with the listed numbers") {
    EdgeOutline o = on();
    o.thickness = 2.0f;
    const EdgeOutlineFrame f = edge_outline_frame(o, 200, 100, 0.01f, true);
    CHECK(f.strength == 1.0f);
    CHECK(f.color_opacity.x == 0.02f);
    CHECK(f.color_opacity.y == 0.02f);
    CHECK(f.color_opacity.z == 0.025f);
    CHECK(f.color_opacity.w == 1.0f);
    CHECK(f.tap_uv.x == doctest::Approx(2.0f / 200.0f));
    CHECK(f.tap_uv.y == doctest::Approx(2.0f / 100.0f));
    CHECK(f.depth_threshold == 0.05f);
    CHECK(f.normal_threshold == 0.4f);
    CHECK(f.depth_weight == 1.0f);
    CHECK(f.normal_weight == 1.0f);
    CHECK(f.fade_start == 30.0f);
    CHECK(f.fade_end == 80.0f);
    CHECK(f.fog_density == 0.01f);
    CHECK(f.taps == 8u);
    CHECK(f.sky_silhouette);
    CHECK(edge_outline_frame(o, 200, 100, 0.0f, false).taps == 4u);
    o.sky_silhouette = false;
    CHECK_FALSE(edge_outline_frame(o, 200, 100, 0.0f, false).sky_silhouette);
    o.opacity = 0.5f;
    const EdgeOutlineFrame half = edge_outline_frame(o, 200, 100, 0.0f, false);
    CHECK(half.strength == 0.5f);
    CHECK(half.color_opacity.w == 0.5f);
}

TEST_CASE("edge outline: strength is exactly 0 for every off case") {
    CHECK(is_zero_frame(edge_outline_frame(EdgeOutline{}, 64, 64, 0.0f, true)));  // disabled by default
    EdgeOutline o = on();
    CHECK_FALSE(is_zero_frame(edge_outline_frame(o, 64, 64, 0.0f, true)));
    CHECK(is_zero_frame(edge_outline_frame(o, 0, 64, 0.0f, true)));
    CHECK(is_zero_frame(edge_outline_frame(o, 64, 0, 0.0f, true)));

    EdgeOutline z = on();
    z.opacity = 0.0f;
    CHECK(is_zero_frame(edge_outline_frame(z, 64, 64, 0.0f, true)));
    z = on();
    z.depth_weight = 0.0f;
    CHECK_FALSE(is_zero_frame(edge_outline_frame(z, 64, 64, 0.0f, true)));  // creases still draw
    z.normal_weight = 0.0f;
    CHECK(is_zero_frame(edge_outline_frame(z, 64, 64, 0.0f, true)));
    z = on();
    z.fade_start = 0.0f;
    z.fade_end = 0.0f;
    CHECK(is_zero_frame(edge_outline_frame(z, 64, 64, 0.0f, true)));
    z = on();
    z.fade_end = -5.0f;  // clamps to 0, then orders to fade_start (30): still draws
    CHECK(edge_outline_frame(z, 64, 64, 0.0f, true).fade_end == 30.0f);
    z.fade_start = -1.0f;  // both clamp to 0
    CHECK(is_zero_frame(edge_outline_frame(z, 64, 64, 0.0f, true)));

    // NaN and infinity in any field.
    const f32 bad[] = {kNaN, kInf, -kInf};
    for (const f32 b : bad) {
        EdgeOutline n = on();
        n.color.x = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.color.y = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.color.z = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.opacity = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.thickness = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.depth_threshold = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.normal_threshold = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.depth_weight = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.normal_weight = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.fade_start = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        n = on();
        n.fade_end = b;
        CHECK(is_zero_frame(edge_outline_frame(n, 64, 64, 0.0f, true)));
        CHECK(is_zero_frame(edge_outline_frame(on(), 64, 64, b, true)));
    }
}

TEST_CASE("edge outline: the frame clamps its ranges and orders the fade") {
    EdgeOutline o = on();
    o.color = math::vec3{100.0f, -1.0f, 16.0f};
    o.opacity = 7.0f;
    o.thickness = 100.0f;
    o.depth_threshold = 0.0f;
    o.normal_threshold = 100.0f;
    o.depth_weight = 3.0f;
    o.normal_weight = 2.0f;
    o.fade_start = 20000.0f;
    o.fade_end = 10.0f;
    EdgeOutlineFrame f = edge_outline_frame(o, 100, 50, -1.0f, true);
    CHECK(f.color_opacity.x == 16.0f);
    CHECK(f.color_opacity.y == 0.0f);
    CHECK(f.color_opacity.z == 16.0f);
    CHECK(f.strength == 1.0f);
    CHECK(f.tap_uv.x == doctest::Approx(4.0f / 100.0f));
    CHECK(f.tap_uv.y == doctest::Approx(4.0f / 50.0f));
    CHECK(f.depth_threshold == 0.001f);
    CHECK(f.normal_threshold == 2.0f);
    CHECK(f.depth_weight == 1.0f);
    CHECK(f.normal_weight == 1.0f);
    CHECK(f.fade_start == 10000.0f);
    CHECK(f.fade_end == 10000.0f);  // fade_end >= fade_start
    CHECK(f.fog_density == 0.0f);

    o = on();
    o.thickness = 0.0f;
    o.depth_threshold = 100.0f;
    o.normal_threshold = 0.0f;
    o.fade_start = 50.0f;
    o.fade_end = 20.0f;
    f = edge_outline_frame(o, 100, 50, 0.0f, false);
    CHECK(f.tap_uv.x == doctest::Approx(0.5f / 100.0f));
    CHECK(f.tap_uv.y == doctest::Approx(0.5f / 50.0f));
    CHECK(f.depth_threshold == 1.0f);
    CHECK(f.normal_threshold == 0.01f);
    CHECK(f.fade_start == 50.0f);
    CHECK(f.fade_end == 50.0f);
}

TEST_CASE("edge outline: the frame is deterministic") {
    const EdgeOutlineFrame a = edge_outline_frame(on(), 1920, 1080, 0.02f, true);
    const EdgeOutlineFrame b = edge_outline_frame(on(), 1920, 1080, 0.02f, true);
    CHECK(same_bits(a.strength, b.strength));
    CHECK(same_bits(a.tap_uv.x, b.tap_uv.x));
    CHECK(same_bits(a.tap_uv.y, b.tap_uv.y));
    CHECK(same_bits(a.fog_density, b.fog_density));
    CHECK(a.taps == b.taps);
}

TEST_CASE("edge outline: the quality tiers keep it on, Low with the 4-tap cross") {
    const QualitySettings low = quality_settings(QualityTier::Low);
    const QualitySettings med = quality_settings(QualityTier::Medium);
    const QualitySettings high = quality_settings(QualityTier::High);
    CHECK(low.edge_outline);
    CHECK_FALSE(low.edge_outline_wide);
    CHECK(med.edge_outline);
    CHECK(med.edge_outline_wide);
    CHECK(high.edge_outline);
    CHECK(high.edge_outline_wide);
}

TEST_CASE("edge outline: apply_quality honors the tier flag and r.edge_outline") {
    ResetOutlineCvar reset;
    const EdgeOutline o = on();
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(applied(tier, o).outline.enabled);
        CHECK_FALSE(applied(tier, EdgeOutline{}).outline.enabled);  // a disabled setting stays disabled
    }
    QualitySettings off = quality_settings(QualityTier::High);
    off.edge_outline = false;
    PostFx post;
    post.outline = o;
    ShadowSettings shadows;
    apply_quality(off, shadows, post);
    CHECK_FALSE(post.outline.enabled);

    outline_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK_FALSE(applied(tier, o).outline.enabled);
    }
    outline_cvar().set_string("1");
    CHECK(applied(QualityTier::High, o).outline.enabled);
}

TEST_CASE("edge outline: apply_quality clamps the ranges and sanitizes NaN") {
    ResetOutlineCvar reset;
    EdgeOutline o = on();
    o.color = math::vec3{100.0f, -1.0f, 100.0f};
    o.opacity = 5.0f;
    o.thickness = 100.0f;
    o.depth_threshold = 0.0f;
    o.normal_threshold = 100.0f;
    o.depth_weight = 5.0f;
    o.normal_weight = -1.0f;
    o.fade_start = -3.0f;
    o.fade_end = 99999.0f;
    EdgeOutline c = applied(QualityTier::High, o).outline;
    CHECK(c.enabled);
    CHECK(c.color.x == 16.0f);
    CHECK(c.color.y == 0.0f);
    CHECK(c.color.z == 16.0f);
    CHECK(c.opacity == 1.0f);
    CHECK(c.thickness == 4.0f);
    CHECK(c.depth_threshold == 0.001f);
    CHECK(c.normal_threshold == 2.0f);
    CHECK(c.depth_weight == 1.0f);
    CHECK(c.normal_weight == 0.0f);
    CHECK(c.fade_start == 0.0f);
    CHECK(c.fade_end == 10000.0f);

    o = on();
    o.opacity = -1.0f;
    o.thickness = 0.0f;
    o.depth_threshold = 5.0f;
    o.normal_threshold = 0.0f;
    o.depth_weight = -1.0f;
    o.normal_weight = 5.0f;
    o.fade_start = 99999.0f;
    o.fade_end = 10.0f;  // below fade_start: reordered
    c = applied(QualityTier::High, o).outline;
    CHECK(c.opacity == 0.0f);
    CHECK(c.thickness == 0.5f);
    CHECK(c.depth_threshold == 1.0f);
    CHECK(c.normal_threshold == 0.01f);
    CHECK(c.depth_weight == 0.0f);
    CHECK(c.normal_weight == 1.0f);
    CHECK(c.fade_start == 10000.0f);
    CHECK(c.fade_end == 10000.0f);

    o = on();
    o.fade_start = 60.0f;
    o.fade_end = 40.0f;
    c = applied(QualityTier::High, o).outline;
    CHECK(c.fade_start == 60.0f);
    CHECK(c.fade_end == 60.0f);

    // NaN falls back to the default.
    o = on();
    o.color = math::vec3{kNaN, kNaN, kNaN};
    o.opacity = o.thickness = o.depth_threshold = o.normal_threshold = kNaN;
    o.depth_weight = o.normal_weight = o.fade_start = o.fade_end = kNaN;
    c = applied(QualityTier::High, o).outline;
    const EdgeOutline d;
    CHECK(c.color.x == d.color.x);
    CHECK(c.color.y == d.color.y);
    CHECK(c.color.z == d.color.z);
    CHECK(c.opacity == d.opacity);
    CHECK(c.thickness == d.thickness);
    CHECK(c.depth_threshold == d.depth_threshold);
    CHECK(c.normal_threshold == d.normal_threshold);
    CHECK(c.depth_weight == d.depth_weight);
    CHECK(c.normal_weight == d.normal_weight);
    CHECK(c.fade_start == d.fade_start);
    CHECK(c.fade_end == d.fade_end);
}

TEST_CASE("edge outline: in-range values and the default pass through apply_quality unchanged") {
    ResetOutlineCvar reset;
    const EdgeOutline d;
    const EdgeOutline c = applied(QualityTier::High, d).outline;
    CHECK_FALSE(c.enabled);
    CHECK(c.color.x == d.color.x);
    CHECK(c.color.y == d.color.y);
    CHECK(c.color.z == d.color.z);
    CHECK(c.opacity == d.opacity);
    CHECK(c.thickness == d.thickness);
    CHECK(c.depth_threshold == d.depth_threshold);
    CHECK(c.normal_threshold == d.normal_threshold);
    CHECK(c.depth_weight == d.depth_weight);
    CHECK(c.normal_weight == d.normal_weight);
    CHECK(c.fade_start == d.fade_start);
    CHECK(c.fade_end == d.fade_end);
    CHECK(c.sky_silhouette == d.sky_silhouette);
    CHECK(c.fog_follow == d.fog_follow);
}
