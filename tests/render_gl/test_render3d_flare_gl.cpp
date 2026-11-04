// M-LENSFLARE 38c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the sun flare's 1x1
// occlusion pass over the half-resolution prepass depth and its composite into the post-fx ping-pong target before
// bloom (Forward3D::record_flare, kFlareOccFS / kFlareFS). A disabled flare, intensity 0, a sun behind the camera,
// below the horizon or beyond the frame's border draw the frame without the pass bit for bit; an unoccluded sun
// brightens the halo and the ghost discs lens_flare_frame lays out (an asymmetric sun pins the uv convention: a
// flipped v would put the ghosts on the mirrored side) and nothing else; an opaque box over the sun gives the frame
// without the flare exactly, half a box an intermediate flare; bloom carries the flare beyond its halo; with TAA the
// next frame's history holds no flare; the Medium tier draws no starburst. The scene is the flare sky (fixture3d.h);
// one 160 x 120 frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "fixture3d.h"
#include "mce/render/lens_flare.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

enum class Occluder { None, Full, Half };

f64 luma(const Array<u8>& px, usize i) { return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]; }

bool same(const Array<u8>& a, const Array<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize p) {
    return a[p * 4] == b[p * 4] && a[p * 4 + 1] == b[p * 4 + 1] && a[p * 4 + 2] == b[p * 4 + 2];
}

int max_diff(const Array<u8>& a, const Array<u8>& b) {
    int m = 0;
    for (usize i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

// The pixel's centre in the fullscreen-pass uv (read_pixels is top row first; v = 0 is the bottom row).
math::vec2 uv_of(usize p) {
    const usize x = p % kW, y = p / kW;
    return {(f32(x) + 0.5f) / f32(kW), 1.0f - (f32(y) + 0.5f) / f32(kH)};
}

usize pixel_at(math::vec2 uv) {
    const usize x = usize(std::clamp(i32(uv.x * f32(kW)), 0, i32(kW) - 1));
    const usize y = usize(std::clamp(i32((1.0f - uv.y) * f32(kH)), 0, i32(kH) - 1));
    return y * kW + x;
}

// Distance in units of the view height (the flare's radii), as the shaders measure it.
f32 dist(math::vec2 a, math::vec2 b) {
    const f32 dx = (a.x - b.x) * (f32(kW) / f32(kH)), dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
};

// One frame of the flare sky with the sun toward `toward` (null: at kFlareSunU, kFlareSunV), the flare (null: off)
// and the occluder as asked.
Shot draw(Fixture3D& f, const LensFlare* flare, Occluder occ = Occluder::None, bool bloom = false,
          const vec3* toward = nullptr, const std::function<void(Scene3D&)>& more = {}) {
    cloud_sky_camera(f);
    f.render(0.0f, true, [&](Scene3D& s) {
        const vec3 d = toward ? *toward : lens_flare_toward(s.view(), kFlareSunU, kFlareSunV);
        lens_flare_scene(s, d, flare, bloom);
        if (occ == Occluder::Full) lens_flare_occluder(s, d, 0.0f);
        if (occ == Occluder::Half) lens_flare_occluder(s, d, -0.5f);
        if (more) more(s);
    });
    CHECK(f.device->validation_error_count() == 0);
    return {f.pixels, f.renderer.forward_3d().last_stats()};
}

// The CPU frame the pass drew from (no TAA: the projection is unjittered).
LensFlareFrame cpu_frame(const Fixture3D& f, const LensFlare& fl) {
    return lens_flare_frame(fl, f.view.view_proj, lens_flare_toward(f.view, kFlareSunU, kFlareSunV),
                            f32(kW) / f32(kH), true);
}

// Summed 8-bit luma the flare adds over the pixels of ghost k's disc.
f64 ghost_gain(const Array<u8>& px, const Array<u8>& plain, const LensFlareGhost& g) {
    f64 sum = 0.0;
    for (usize p = 0; p < usize(kW) * kH; ++p)
        if (dist(uv_of(p), g.center) < g.radius * 0.7f) sum += luma(px, p * 4) - luma(plain, p * 4);
    return sum;
}

}  // namespace

TEST_CASE("lens flare: off, intensity 0 or visibility 0 draw the frame without the pass bit for bit") {
    Fixture3D f;
    const Shot plain = draw(f, nullptr);
    CHECK_FALSE(plain.stats.lens_flare);
    CHECK(plain.stats.lens_flare_strength == 0.0f);
    LensFlare off = lens_flare_setting();
    off.enabled = false;
    off.starburst = 2.0f;
    const Shot a = draw(f, &off);
    CHECK_FALSE(a.stats.lens_flare);
    CHECK(same(a.px, plain.px));
    LensFlare dark = lens_flare_setting();
    dark.intensity = 0.0f;
    const Shot b = draw(f, &dark);
    CHECK_FALSE(b.stats.lens_flare);
    CHECK(same(b.px, plain.px));
    LensFlare hidden = lens_flare_setting();
    hidden.visibility = 0.0f;
    const Shot c = draw(f, &hidden);
    CHECK_FALSE(c.stats.lens_flare);
    CHECK(same(c.px, plain.px));
    // The prepass the flare would read is not drawn either.
    CHECK(c.stats.ssao_draws == plain.stats.ssao_draws);
}

TEST_CASE("lens flare: an unoccluded sun brightens its halo and the CPU layout's ghost discs, and nothing else") {
    Fixture3D f;
    const Shot plain = draw(f, nullptr);
    const LensFlare fl = lens_flare_setting();
    const Shot lit = draw(f, &fl);
    REQUIRE(lit.stats.lens_flare);
    const LensFlareFrame fr = cpu_frame(f, fl);
    REQUIRE(fr.strength > 0.0f);
    CHECK(lit.stats.lens_flare_strength == doctest::Approx(fr.strength));
    CHECK(lit.stats.ssao_draws > 0);  // the flare drew the prepass it reads
    REQUIRE(fr.ghost_count == 4);
    CHECK(fr.sun_uv.x == doctest::Approx(kFlareSunU).epsilon(1e-3));
    CHECK(fr.sun_uv.y == doctest::Approx(kFlareSunV).epsilon(1e-3));

    // The halo: just outside the sky's sun disk, toward the frame's centre.
    const usize halo_px = pixel_at({fr.sun_uv.x + 0.06f * f32(kH) / f32(kW), fr.sun_uv.y});
    MESSAGE("halo pixel luma " << luma(plain.px, halo_px * 4) << " -> " << luma(lit.px, halo_px * 4));
    CHECK(luma(lit.px, halo_px * 4) > luma(plain.px, halo_px * 4) + 8.0);
    // Each ghost centre, where lens_flare_ghost_layout put it.
    for (u32 k = 0; k < fr.ghost_count; ++k) {
        const usize p = pixel_at(fr.ghosts[k].center);
        MESSAGE("ghost " << k << " at uv (" << fr.ghosts[k].center.x << ", " << fr.ghosts[k].center.y << "): luma "
                         << luma(plain.px, p * 4) << " -> " << luma(lit.px, p * 4));
        CHECK(luma(lit.px, p * 4) > luma(plain.px, p * 4) + 4.0);
    }
    // The uv convention: ghost 1's mirror across v = 0.5 lies outside the halo and every disc, and is untouched (a
    // flipped v in any pass would put the ghost there instead).
    const math::vec2 mirror{fr.ghosts[1].center.x, 1.0f - fr.ghosts[1].center.y};
    REQUIRE(dist(mirror, fr.sun_uv) > 3.0f * fr.halo_radius + 0.02f);
    for (u32 k = 0; k < fr.ghost_count; ++k) REQUIRE(dist(mirror, fr.ghosts[k].center) > fr.ghosts[k].radius + 0.02f);
    CHECK(same_px(lit.px, plain.px, pixel_at(mirror)));
    // Beyond the halo's cut (3 radii) and outside every ghost disc (plus a pixel), the frame is the plain one.
    const f32 margin = 1.5f / f32(kH);
    u32 outside = 0, changed = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        const math::vec2 uv = uv_of(p);
        bool close = dist(uv, fr.sun_uv) <= 3.0f * fr.halo_radius + margin;
        for (u32 k = 0; k < fr.ghost_count; ++k) close = close || dist(uv, fr.ghosts[k].center) <= fr.ghosts[k].radius + margin;
        if (close) continue;
        ++outside;
        changed += same_px(lit.px, plain.px, p) ? 0u : 1u;
    }
    MESSAGE(outside << " pixels outside the halo and the ghosts, " << changed << " changed");
    CHECK(outside > usize(kW) * kH / 2);
    CHECK(changed == 0);
}

TEST_CASE("lens flare: a box over the sun hides it exactly, half a box gives an intermediate flare") {
    Fixture3D f;
    const LensFlare fl = lens_flare_setting();
    const Shot plain = draw(f, nullptr);
    const Shot lit = draw(f, &fl);
    const LensFlareFrame fr = cpu_frame(f, fl);
    const Shot plain_full = draw(f, nullptr, Occluder::Full);
    const Shot full = draw(f, &fl, Occluder::Full);
    REQUIRE(full.stats.lens_flare);  // recorded: the CPU cannot see the occlusion
    MESSAGE("box over the sun: max channel difference to the frame without the flare " << max_diff(full.px, plain_full.px));
    CHECK(same(full.px, plain_full.px));
    const Shot plain_half = draw(f, nullptr, Occluder::Half);
    const Shot half = draw(f, &fl, Occluder::Half);
    // Ghost 1 sits on the far side of the centre, clear of the box: its light is the occlusion's share.
    const f64 g_open = ghost_gain(lit.px, plain.px, fr.ghosts[1]);
    const f64 g_half = ghost_gain(half.px, plain_half.px, fr.ghosts[1]);
    const f64 g_full = ghost_gain(full.px, plain_full.px, fr.ghosts[1]);
    MESSAGE("ghost 1 gain: open " << g_open << ", half covered " << g_half << ", covered " << g_full);
    CHECK(g_open > 50.0);
    CHECK(g_full == 0.0);
    CHECK(g_half > 0.2 * g_open);
    CHECK(g_half < 0.8 * g_open);
}

TEST_CASE("lens flare: a sun behind the camera, below the horizon or beyond the border draws the plain frame") {
    Fixture3D f;
    const LensFlare fl = lens_flare_setting();
    draw(f, nullptr);  // the view
    const View3D view = f.view;
    const vec3 behind = math::normalize(vec3{-view.forward.x, 0.4f, -view.forward.z});
    const vec3 below = math::normalize(vec3{view.forward.x, -0.1f, view.forward.z});
    const vec3 border = lens_flare_toward(view, -0.03f, 0.7f);  // just past the left edge
    for (const vec3* d : {&behind, &below, &border}) {
        const Shot plain = draw(f, nullptr, Occluder::None, false, d);
        const Shot lit = draw(f, &fl, Occluder::None, false, d);
        CHECK_FALSE(lit.stats.lens_flare);
        CHECK(lit.stats.lens_flare_strength == 0.0f);
        CHECK(same(lit.px, plain.px));
    }
}

TEST_CASE("lens flare: bloom spreads the flare beyond its halo (the flare is added before bloom)") {
    Fixture3D f;
    LensFlare fl = lens_flare_setting();
    fl.ghost_count = 0;
    fl.halo = 4.0f;
    fl.halo_radius = 0.05f;
    fl.intensity = 4.0f;
    const Shot plain = draw(f, nullptr, Occluder::None, true);
    const Shot lit = draw(f, &fl, Occluder::None, true);
    const Shot plain_sharp = draw(f, nullptr);
    const Shot lit_sharp = draw(f, &fl);
    REQUIRE(lit.stats.lens_flare);
    const LensFlareFrame fr = cpu_frame(f, fl);
    // A ring beyond the halo's cut: the flare reaches it only through bloom.
    const f32 r0 = 3.0f * fr.halo_radius + 2.0f / f32(kH), r1 = r0 + 0.2f;
    f64 sum_plain = 0.0, sum_lit = 0.0;
    u32 ring = 0, sharp_changed = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        const f32 r = dist(uv_of(p), fr.sun_uv);
        if (r < r0 || r > r1) continue;
        ++ring;
        sum_plain += luma(plain.px, p * 4);
        sum_lit += luma(lit.px, p * 4);
        sharp_changed += same_px(lit_sharp.px, plain_sharp.px, p) ? 0u : 1u;
    }
    MESSAGE(ring << " ring pixels: bloomed luma " << sum_plain << " -> " << sum_lit << ", " << sharp_changed
                 << " changed without bloom");
    CHECK(ring > 100);
    CHECK(sharp_changed == 0);
    CHECK(sum_lit > sum_plain + f64(ring));  // at least one 8-bit step a pixel on average
}

TEST_CASE("lens flare: with TAA the next frame's history holds no flare") {
    const LensFlare fl = lens_flare_setting();
    auto two_frames = [&](bool flare_first) {
        Fixture3D f;
        const auto taa = [](Scene3D& s) {
            PostFx post = s.post();
            post.taa = true;
            s.set_post(post);
        };
        const Shot first = draw(f, flare_first ? &fl : nullptr, Occluder::None, false, nullptr, taa);
        CHECK(first.stats.taa);
        CHECK(first.stats.lens_flare == flare_first);
        const Shot second = draw(f, &fl, Occluder::Full, false, nullptr, taa);
        CHECK(second.stats.taa_history);
        CHECK(second.stats.lens_flare);
        return second.px;
    };
    const Array<u8> after_flare = two_frames(true);
    const Array<u8> after_plain = two_frames(false);
    MESSAGE("second frame, max channel difference " << max_diff(after_flare, after_plain));
    CHECK(same(after_flare, after_plain));
}

TEST_CASE("lens flare: the Medium tier draws no starburst, High does") {
    Fixture3D f;
    LensFlare fl = lens_flare_setting();
    fl.starburst = 2.0f;
    fl.starburst_rays = 6;
    PostFx medium;
    medium.flare = fl;
    ShadowSettings shadows;
    apply_quality(quality_settings(QualityTier::Medium), shadows, medium);
    CHECK(medium.flare.enabled);
    CHECK(medium.flare.starburst == 0.0f);
    PostFx high;
    high.flare = fl;
    apply_quality(quality_settings(QualityTier::High), shadows, high);
    CHECK(high.flare.starburst == 2.0f);
    LensFlare none = fl;
    none.starburst = 0.0f;
    const Shot rays = draw(f, &high.flare);
    const Shot mid = draw(f, &medium.flare);
    const Shot bare = draw(f, &none);
    CHECK(mid.stats.lens_flare);
    CHECK(same(mid.px, bare.px));
    u32 changed = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) changed += same_px(rays.px, bare.px, p) ? 0u : 1u;
    MESSAGE("starburst pixels on High: " << changed);
    CHECK(changed > 50);
}
