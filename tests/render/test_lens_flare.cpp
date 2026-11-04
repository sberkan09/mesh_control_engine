// M-LENSFLARE 38b: the pure lens flare helpers (projection, fades, ghost layout, the frame) and how the quality
// tiers and `r.lens_flare` map onto PostFx::flare. No GPU.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "mce/convar.h"
#include "mce/render/lens_flare.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

// Camera at the origin looking down -z, y up (GL clip space), 90 degree vertical field of view, aspect 1: a direction
// (x, y, -1) lands at ndc (x, y).
math::mat4 camera() {
    return math::perspective_rh_no(1.5707963f, 1.0f, 0.1f, 100.0f) *
           math::look_at_rh(math::vec3{0.0f, 0.0f, 0.0f}, math::vec3{0.0f, 0.0f, -1.0f}, math::vec3{0.0f, 1.0f, 0.0f});
}

math::vec3 norm(math::vec3 v) {
    const f32 l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return math::vec3{v.x / l, v.y / l, v.z / l};
}

LensFlare on() {
    LensFlare f;
    f.enabled = true;
    return f;
}

bool same_bits(const LensFlareFrame& a, const LensFlareFrame& b) {
    static_assert(sizeof(LensFlareFrame) % sizeof(u32) == 0);
    constexpr usize kWords = sizeof(LensFlareFrame) / sizeof(u32);
    u32 wa[kWords];
    u32 wb[kWords];
    std::memcpy(wa, &a, sizeof a);
    std::memcpy(wb, &b, sizeof b);
    for (usize i = 0; i < kWords; ++i)
        if (wa[i] != wb[i]) return false;
    return true;
}

ConVar& flare_cvar() {
    ConVar* cv = ConVar::find(StringId("r.lens_flare"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetFlareCvar {
    ~ResetFlareCvar() { flare_cvar().reset(); }
};

PostFx applied(QualityTier tier, const LensFlare& flare) {
    PostFx post;
    post.flare = flare;
    ShadowSettings shadows;
    apply_quality(quality_settings(tier), shadows, post);
    return post;
}

}  // namespace

TEST_CASE("lens flare: projection of a known direction") {
    math::vec2 uv{-1.0f, -1.0f};
    CHECK(lens_flare_project(camera(), math::vec3{0.0f, 0.0f, -1.0f}, uv));
    CHECK(uv.x == doctest::Approx(0.5f));
    CHECK(uv.y == doctest::Approx(0.5f));

    // Off-centre and asymmetric: up (+y) and to the left (-x). Row 0 is v = 0 (uv = ndc * 0.5 + 0.5), so up is v > 0.5.
    CHECK(lens_flare_project(camera(), math::vec3{-0.5f, 0.25f, -1.0f}, uv));
    CHECK(uv.x == doctest::Approx(0.25f));
    CHECK(uv.y == doctest::Approx(0.625f));
    CHECK(lens_flare_project(camera(), math::vec3{0.5f, -0.25f, -1.0f}, uv));
    CHECK(uv.x == doctest::Approx(0.75f));
    CHECK(uv.y == doctest::Approx(0.375f));
}

TEST_CASE("lens flare: behind the camera and non-finite inputs do not project") {
    math::vec2 uv{0.125f, 0.875f};
    CHECK_FALSE(lens_flare_project(camera(), math::vec3{0.0f, 0.0f, 1.0f}, uv));
    CHECK_FALSE(lens_flare_project(camera(), math::vec3{0.3f, 0.2f, 1.0f}, uv));
    CHECK_FALSE(lens_flare_project(camera(), math::vec3{kNaN, 0.0f, -1.0f}, uv));
    CHECK_FALSE(lens_flare_project(camera(), math::vec3{0.0f, kInf, -1.0f}, uv));
    math::mat4 bad = camera();
    bad.c[0][0] = kNaN;
    CHECK_FALSE(lens_flare_project(bad, math::vec3{0.0f, 0.0f, -1.0f}, uv));
    CHECK(uv.x == 0.125f);  // untouched on false
    CHECK(uv.y == 0.875f);
}

TEST_CASE("lens flare: horizon fade") {
    CHECK(lens_flare_horizon_fade(math::vec3{0.0f, -1.0f, 0.0f}) == 0.0f);
    CHECK(lens_flare_horizon_fade(math::vec3{1.0f, -0.02f, 0.0f}) == 0.0f);
    CHECK(lens_flare_horizon_fade(math::vec3{0.0f, 0.04f, 1.0f}) == 1.0f);
    CHECK(lens_flare_horizon_fade(math::vec3{0.0f, 1.0f, 0.0f}) == 1.0f);
    CHECK(lens_flare_horizon_fade(math::vec3{0.0f, 0.01f, 1.0f}) == doctest::Approx(0.5f));  // the midpoint of a smoothstep
    f32 prev = 0.0f;
    for (f32 y = -0.02f; y <= 0.04f; y += 0.002f) {
        const f32 f = lens_flare_horizon_fade(math::vec3{0.0f, y, 1.0f});
        CHECK(f >= prev);
        CHECK(f > 0.0f - 1e-6f);
        CHECK(f <= 1.0f);
        prev = f;
    }
    const f32 inside = lens_flare_horizon_fade(math::vec3{0.0f, 0.02f, 1.0f});
    CHECK(inside > 0.0f);
    CHECK(inside < 1.0f);
    CHECK(lens_flare_horizon_fade(math::vec3{0.0f, kNaN, 0.0f}) == 0.0f);
}

TEST_CASE("lens flare: edge fade") {
    CHECK(lens_flare_edge_fade(math::vec2{0.0f, 0.5f}, 0.1f) == 0.0f);
    CHECK(lens_flare_edge_fade(math::vec2{1.0f, 0.5f}, 0.1f) == 0.0f);
    CHECK(lens_flare_edge_fade(math::vec2{0.5f, 0.0f}, 0.1f) == 0.0f);
    CHECK(lens_flare_edge_fade(math::vec2{0.5f, 1.0f}, 0.1f) == 0.0f);
    CHECK(lens_flare_edge_fade(math::vec2{-0.3f, 0.5f}, 0.1f) == 0.0f);
    CHECK(lens_flare_edge_fade(math::vec2{0.5f, 1.7f}, 0.1f) == 0.0f);
    CHECK(lens_flare_edge_fade(math::vec2{0.5f, 0.5f}, 0.1f) == 1.0f);
    CHECK(lens_flare_edge_fade(math::vec2{0.1f, 0.9f}, 0.1f) == 1.0f);
    CHECK(lens_flare_edge_fade(math::vec2{0.05f, 0.5f}, 0.1f) == doctest::Approx(0.5f));
    const f32 mid = lens_flare_edge_fade(math::vec2{0.03f, 0.5f}, 0.1f);
    CHECK(mid > 0.0f);
    CHECK(mid < 0.5f);
    CHECK(lens_flare_edge_fade(math::vec2{kNaN, 0.5f}, 0.1f) == 0.0f);
    CHECK(lens_flare_edge_fade(math::vec2{0.5f, 0.5f}, kNaN) == 1.0f);  // NaN width -> the default
    CHECK(lens_flare_edge_fade(math::vec2{0.5f, 0.0f}, 0.0f) == 0.0f);  // a zero width is clamped, never divides by 0
}

TEST_CASE("lens flare: ghost tables and layout") {
    LensFlare f = on();
    f.ghost_count = 6;
    f.ghost_chroma = 1.0f;
    const math::vec2 sun{0.8f, 0.7f};
    LensFlareGhost g[6];
    REQUIRE(lens_flare_ghost_layout(f, sun, 1.5f, g) == 6u);

    // The fixed tables (axis positions, radii, intensities, tints).
    const f32 t[6] = {-0.35f, -0.7f, -1.1f, 0.45f, -1.5f, 1.25f};
    const f32 radius[6] = {0.05f, 0.09f, 0.04f, 0.07f, 0.12f, 0.06f};
    const f32 inten[6] = {0.30f, 0.20f, 0.25f, 0.15f, 0.10f, 0.12f};
    const math::vec3 tint[6] = {{1.00f, 0.55f, 0.35f}, {0.40f, 0.80f, 1.00f}, {0.60f, 1.00f, 0.50f},
                                {1.00f, 0.40f, 0.70f}, {0.50f, 0.60f, 1.00f}, {1.00f, 0.85f, 0.40f}};
    for (u32 k = 0; k < 6; ++k) {
        INFO("ghost " << k);
        CHECK(g[k].center.x == doctest::Approx(0.5f + (sun.x - 0.5f) * t[k]));
        CHECK(g[k].center.y == doctest::Approx(0.5f + (sun.y - 0.5f) * t[k]));
        CHECK(g[k].radius == radius[k]);
        CHECK(g[k].intensity == doctest::Approx(inten[k]));
        CHECK(g[k].tint.x == doctest::Approx(tint[k].x));
        CHECK(g[k].tint.y == doctest::Approx(tint[k].y));
        CHECK(g[k].tint.z == doctest::Approx(tint[k].z));
        CHECK(g[k].pad_ == 0.0f);
    }
}

TEST_CASE("lens flare: ghost count, spacing, gain and chroma") {
    LensFlare f = on();
    const math::vec2 sun{0.25f, 0.75f};
    LensFlareGhost g[6];

    f.ghost_count = 0;
    CHECK(lens_flare_ghost_layout(f, sun, 1.0f, g) == 0u);
    f.ghost_count = 3;
    CHECK(lens_flare_ghost_layout(f, sun, 1.0f, g) == 3u);
    f.ghost_count = 99;
    CHECK(lens_flare_ghost_layout(f, sun, 1.0f, g) == 6u);
    CHECK(lens_flare_ghost_layout(f, sun, 1.0f, nullptr) == 6u);

    f.ghost_count = 6;
    f.ghost_spacing = 2.0f;
    f.ghost_intensity = 2.0f;
    f.ghost_chroma = 0.0f;
    REQUIRE(lens_flare_ghost_layout(f, sun, 1.0f, g) == 6u);
    CHECK(g[0].center.x == doctest::Approx(0.5f + (sun.x - 0.5f) * -0.35f * 2.0f));
    CHECK(g[0].center.y == doctest::Approx(0.5f + (sun.y - 0.5f) * -0.35f * 2.0f));
    CHECK(g[0].intensity == doctest::Approx(0.60f));
    for (u32 k = 0; k < 6; ++k) {  // chroma 0: white tints
        CHECK(g[k].tint.x == 1.0f);
        CHECK(g[k].tint.y == 1.0f);
        CHECK(g[k].tint.z == 1.0f);
    }

    f.ghost_spacing = 1.0f;
    f.ghost_intensity = 1.0f;
    f.ghost_chroma = 0.5f;  // half way from white to the table
    REQUIRE(lens_flare_ghost_layout(f, sun, 1.0f, g) == 6u);
    CHECK(g[1].tint.x == doctest::Approx(0.7f));
    CHECK(g[1].tint.y == doctest::Approx(0.9f));
    CHECK(g[1].tint.z == doctest::Approx(1.0f));

    // Every ghost lies on the line through the screen centre and the sun.
    for (u32 k = 0; k < 6; ++k) {
        const f32 cross = (g[k].center.x - 0.5f) * (sun.y - 0.5f) - (g[k].center.y - 0.5f) * (sun.x - 0.5f);
        CHECK(std::fabs(cross) < 1e-6f);
    }
    // Sun at the centre: all ghosts at the centre.
    REQUIRE(lens_flare_ghost_layout(f, math::vec2{0.5f, 0.5f}, 1.0f, g) == 6u);
    for (u32 k = 0; k < 6; ++k) {
        CHECK(g[k].center.x == 0.5f);
        CHECK(g[k].center.y == 0.5f);
    }
}

TEST_CASE("lens flare: the frame of a sun straight ahead") {
    const LensFlare f = on();
    const LensFlareFrame fr = lens_flare_frame(f, camera(), norm(math::vec3{0.0f, 0.3f, -1.0f}), 1.5f, true);
    // Sun at ndc (0, 0.3): uv (0.5, 0.65), well inside every fade, above the horizon.
    CHECK(fr.sun_uv.x == doctest::Approx(0.5f));
    CHECK(fr.sun_uv.y == doctest::Approx(0.65f));
    CHECK(fr.strength == 1.0f);
    CHECK(fr.aspect == 1.5f);
    CHECK(fr.halo == 1.0f);
    CHECK(fr.halo_radius == 0.25f);
    CHECK(fr.starburst == 0.0f);  // the setting's own starburst default is 0
    CHECK(fr.rays == 6u);
    CHECK(fr.occlusion_radius == 0.015f);
    CHECK(fr.ghost_count == 4u);
    for (u32 k = 0; k < fr.ghost_count; ++k) CHECK(fr.ghosts[k].radius > 0.0f);
}

TEST_CASE("lens flare: strength is intensity * visibility * horizon * edge") {
    LensFlare f = on();
    f.intensity = 2.0f;
    f.visibility = 0.5f;
    f.edge_fade = 0.2f;
    const math::vec3 dir = norm(math::vec3{0.0f, 0.6f, -1.0f});  // ndc y = 0.6 -> v = 0.8, 0.2 from the top edge
    const f32 horizon = lens_flare_horizon_fade(dir);
    math::vec2 uv;
    REQUIRE(lens_flare_project(camera(), dir, uv));
    const f32 edge = lens_flare_edge_fade(uv, 0.2f);
    CHECK(edge == 1.0f);
    const LensFlareFrame full = lens_flare_frame(f, camera(), dir, 1.0f, false);
    CHECK(full.strength == 2.0f * 0.5f * horizon * edge);

    // Partway into the edge ramp: ndc y = 0.9 -> v = 0.95, 0.05 from the border, a quarter of the 0.2 width.
    const math::vec3 near_edge = norm(math::vec3{0.0f, 0.9f, -1.0f});
    REQUIRE(lens_flare_project(camera(), near_edge, uv));
    const f32 e2 = lens_flare_edge_fade(uv, 0.2f);
    CHECK(e2 > 0.0f);
    CHECK(e2 < 1.0f);
    const LensFlareFrame part = lens_flare_frame(f, camera(), near_edge, 1.0f, false);
    CHECK(part.strength == 2.0f * 0.5f * lens_flare_horizon_fade(near_edge) * e2);
    CHECK(part.strength < full.strength);
    CHECK(part.strength > 0.0f);
}

TEST_CASE("lens flare: strength is exactly 0 for every off case") {
    const math::vec3 up = norm(math::vec3{0.0f, 0.3f, -1.0f});
    REQUIRE(lens_flare_frame(on(), camera(), up, 1.0f, true).strength > 0.0f);

    auto strength = [&](const LensFlare& f, math::vec3 d, const math::mat4& vp) {
        return lens_flare_frame(f, vp, d, 1.0f, true).strength;
    };

    LensFlare off = on();
    off.enabled = false;
    CHECK(strength(off, up, camera()) == 0.0f);

    LensFlare f = on();
    f.intensity = 0.0f;
    CHECK(strength(f, up, camera()) == 0.0f);
    f = on();
    f.visibility = 0.0f;
    CHECK(strength(f, up, camera()) == 0.0f);

    CHECK(strength(on(), norm(math::vec3{0.0f, 0.3f, 1.0f}), camera()) == 0.0f);   // behind the camera
    CHECK(strength(on(), norm(math::vec3{0.0f, -0.5f, -1.0f}), camera()) == 0.0f); // below the horizon
    CHECK(strength(on(), norm(math::vec3{3.0f, 0.3f, -1.0f}), camera()) == 0.0f);  // off screen

    // NaN and infinite inputs.
    CHECK(strength(on(), math::vec3{kNaN, 0.3f, -1.0f}, camera()) == 0.0f);
    CHECK(strength(on(), math::vec3{0.0f, kInf, -1.0f}, camera()) == 0.0f);
    CHECK(strength(on(), math::vec3{0.0f, 0.0f, 0.0f}, camera()) == 0.0f);
    math::mat4 bad = camera();
    bad.c[1][1] = kNaN;
    CHECK(strength(on(), up, bad) == 0.0f);
    f = on();
    f.intensity = kNaN;
    CHECK(strength(f, up, camera()) == 0.0f);
    f = on();
    f.visibility = kNaN;
    CHECK(strength(f, up, camera()) == 0.0f);
}

TEST_CASE("lens flare: the frame is deterministic") {
    LensFlare f = on();
    f.starburst = 1.5f;
    f.ghost_count = 6;
    const math::vec3 d = norm(math::vec3{-0.3f, 0.4f, -1.0f});
    const LensFlareFrame a = lens_flare_frame(f, camera(), d, 1.77f, true);
    const LensFlareFrame b = lens_flare_frame(f, camera(), d, 1.77f, true);
    CHECK(a.strength > 0.0f);
    CHECK(same_bits(a, b));
    // Off frames are deterministic too.
    f.enabled = false;
    CHECK(same_bits(lens_flare_frame(f, camera(), d, 1.77f, true), lens_flare_frame(f, camera(), d, 1.77f, true)));
}

TEST_CASE("lens flare: starburst is 0 when not allowed; the other fields are clamped") {
    LensFlare f = on();
    f.starburst = 2.0f;
    const math::vec3 d = norm(math::vec3{0.0f, 0.3f, -1.0f});
    CHECK(lens_flare_frame(f, camera(), d, 1.0f, true).starburst == 2.0f);
    CHECK(lens_flare_frame(f, camera(), d, 1.0f, false).starburst == 0.0f);

    f = on();
    f.halo = 99.0f;
    f.halo_radius = 0.0f;
    f.starburst = 99.0f;
    f.starburst_rays = 99;
    f.occlusion_radius = 5.0f;
    f.ghost_count = 99;
    const LensFlareFrame hi = lens_flare_frame(f, camera(), d, 1.0f, true);
    CHECK(hi.halo == 4.0f);
    CHECK(hi.halo_radius == 0.02f);
    CHECK(hi.starburst == 4.0f);
    CHECK(hi.rays == 16u);
    CHECK(hi.occlusion_radius == 0.1f);
    CHECK(hi.ghost_count == 6u);
    f.starburst_rays = 0;
    f.halo = kNaN;
    f.halo_radius = kNaN;
    f.occlusion_radius = kNaN;
    const LensFlareFrame lo = lens_flare_frame(f, camera(), d, 1.0f, true);
    CHECK(lo.rays == 2u);
    CHECK(lo.halo == 1.0f);
    CHECK(lo.halo_radius == 0.25f);
    CHECK(lo.occlusion_radius == 0.015f);
    // A bad aspect falls back to 1.
    CHECK(lens_flare_frame(on(), camera(), d, kNaN, true).aspect == 1.0f);
    CHECK(lens_flare_frame(on(), camera(), d, 0.0f, true).aspect == 1.0f);
}

TEST_CASE("lens flare: the quality tiers") {
    const QualitySettings low = quality_settings(QualityTier::Low);
    const QualitySettings med = quality_settings(QualityTier::Medium);
    const QualitySettings high = quality_settings(QualityTier::High);
    CHECK_FALSE(low.lens_flare);
    CHECK_FALSE(low.lens_flare_starburst);
    CHECK(med.lens_flare);
    CHECK_FALSE(med.lens_flare_starburst);
    CHECK(high.lens_flare);
    CHECK(high.lens_flare_starburst);
}

TEST_CASE("lens flare: apply_quality honors the tier and r.lens_flare") {
    ResetFlareCvar reset;
    LensFlare f = on();
    f.starburst = 2.0f;
    CHECK_FALSE(applied(QualityTier::Low, f).flare.enabled);
    CHECK(applied(QualityTier::Medium, f).flare.enabled);
    CHECK(applied(QualityTier::High, f).flare.enabled);
    CHECK(applied(QualityTier::Medium, f).flare.starburst == 0.0f);
    CHECK(applied(QualityTier::Low, f).flare.starburst == 0.0f);
    CHECK(applied(QualityTier::High, f).flare.starburst == 2.0f);
    // A disabled flare stays disabled.
    CHECK_FALSE(applied(QualityTier::High, LensFlare{}).flare.enabled);

    flare_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK_FALSE(applied(tier, f).flare.enabled);
    }
    flare_cvar().set_string("1");
    CHECK(applied(QualityTier::High, f).flare.enabled);
}

TEST_CASE("lens flare: apply_quality clamps the ranges and sanitizes NaN") {
    ResetFlareCvar reset;
    LensFlare f = on();
    f.intensity = 100.0f;
    f.visibility = 5.0f;
    f.halo = -1.0f;
    f.halo_radius = 0.0f;
    f.ghost_count = 99;
    f.ghost_intensity = 100.0f;
    f.ghost_spacing = 0.0f;
    f.ghost_chroma = 3.0f;
    f.starburst = 100.0f;
    f.starburst_rays = 1;
    f.occlusion_radius = 0.0f;
    f.edge_fade = 5.0f;
    LensFlare c = applied(QualityTier::High, f).flare;
    CHECK(c.enabled);
    CHECK(c.intensity == 4.0f);
    CHECK(c.visibility == 1.0f);
    CHECK(c.halo == 0.0f);
    CHECK(c.halo_radius == 0.02f);
    CHECK(c.ghost_count == kLensFlareMaxGhosts);
    CHECK(c.ghost_intensity == 4.0f);
    CHECK(c.ghost_spacing == 0.25f);
    CHECK(c.ghost_chroma == 1.0f);
    CHECK(c.starburst == 4.0f);
    CHECK(c.starburst_rays == 2u);
    CHECK(c.occlusion_radius == 0.002f);
    CHECK(c.edge_fade == 0.5f);

    f.intensity = -1.0f;
    f.visibility = -1.0f;
    f.halo = 100.0f;
    f.halo_radius = 100.0f;
    f.ghost_intensity = -1.0f;
    f.ghost_spacing = 100.0f;
    f.ghost_chroma = -1.0f;
    f.starburst = -1.0f;
    f.starburst_rays = 100;
    f.occlusion_radius = 100.0f;
    f.edge_fade = 0.0f;
    c = applied(QualityTier::High, f).flare;
    CHECK(c.intensity == 0.0f);
    CHECK(c.visibility == 0.0f);
    CHECK(c.halo == 4.0f);
    CHECK(c.halo_radius == 0.6f);
    CHECK(c.ghost_intensity == 0.0f);
    CHECK(c.ghost_spacing == 2.0f);
    CHECK(c.ghost_chroma == 0.0f);
    CHECK(c.starburst == 0.0f);
    CHECK(c.starburst_rays == 16u);
    CHECK(c.occlusion_radius == 0.1f);
    CHECK(c.edge_fade == 0.01f);

    // NaN falls back to the default.
    f = on();
    f.intensity = f.visibility = f.halo = f.halo_radius = kNaN;
    f.ghost_intensity = f.ghost_spacing = f.ghost_chroma = kNaN;
    f.starburst = f.occlusion_radius = f.edge_fade = kNaN;
    c = applied(QualityTier::High, f).flare;
    const LensFlare d;
    CHECK(c.intensity == d.intensity);
    CHECK(c.visibility == d.visibility);
    CHECK(c.halo == d.halo);
    CHECK(c.halo_radius == d.halo_radius);
    CHECK(c.ghost_intensity == d.ghost_intensity);
    CHECK(c.ghost_spacing == d.ghost_spacing);
    CHECK(c.ghost_chroma == d.ghost_chroma);
    CHECK(c.starburst == d.starburst);
    CHECK(c.occlusion_radius == d.occlusion_radius);
    CHECK(c.edge_fade == d.edge_fade);
}

TEST_CASE("lens flare: in-range values and the default pass through apply_quality unchanged") {
    ResetFlareCvar reset;
    const PostFx def = applied(QualityTier::High, LensFlare{});
    const LensFlare d;
    CHECK(def.flare.intensity == d.intensity);
    CHECK(def.flare.halo_radius == d.halo_radius);
    CHECK(def.flare.ghost_count == d.ghost_count);
    CHECK(def.flare.starburst_rays == d.starburst_rays);
    CHECK(def.flare.edge_fade == d.edge_fade);
}
