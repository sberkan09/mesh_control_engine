// M-OUTLINE 39c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the edge outline pass
// after the MSAA resolve and before TAA (Forward3D::record_outline, kOutlineFS) over the half-resolution opaque
// prepass. A disabled outline, opacity 0, both weights 0, Forward3DDesc::edge_outline false or a box beyond fade_end
// draw the frame without the pass bit for bit; the fixture's box on its slab gets lines on its silhouettes (against the
// slab and, with sky_silhouette, the slab's far edge against the sky) and its creases, none inside its flat faces, on
// the open slab or in the sky; depth_weight 0 keeps the creases only, normal_weight 0 the silhouettes only; a small box
// in the upper left pins the uv convention (its mirror across v = 0.5 stays untouched); exp^2 fog weakens the lines;
// the tier picks 4 or 8 taps (r.quality, read by the renderer); TAA keeps the lines on the edges (with a bounded
// flicker from the half-resolution source), and a frame with the outline disabled under TAA is the plain TAA frame bit
// for bit; a renderer without SSAO, SSR and contact shadows still draws it and keeps its water as before;
// -r.edge_outline 0 at init creates no outline (or, alone, prepass) pipeline; the lines follow the exp^2 fog whenever
// the volume is not drawn; two views of different sizes in one command buffer each match the view drawn alone. One 160
// x 120 frame per render (the TAA cases: 8).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/edge_outline.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

f64 luma(const Array<u8>& px, usize p) { return 0.2126 * px[p * 4] + 0.7152 * px[p * 4 + 1] + 0.0722 * px[p * 4 + 2]; }

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

u32 changed_count(const Array<u8>& a, const Array<u8>& b) {
    u32 n = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) n += same_px(a, b, p) ? 0u : 1u;
    return n;
}

struct Px {
    i32 x, y;  // read_pixels order: row 0 is the top
};

// The pixel showing world point p.
Px project(const View3D& view, vec3 p) {
    const math::vec4 c = view.view_proj * math::vec4(p, 1.0f);
    const f32 x = (c.x / c.w * 0.5f + 0.5f) * f32(kW), y = (0.5f - c.y / c.w * 0.5f) * f32(kH);
    REQUIRE(x >= 0.0f);
    REQUIRE(y >= 0.0f);
    REQUIRE(x < f32(kW));
    REQUIRE(y < f32(kH));
    return {i32(x), i32(y)};
}

// The largest luma drop (plain - lined) over the (2r + 1)^2 pixels around `c`.
f64 darkest_drop(const Array<u8>& lined, const Array<u8>& plain, Px c, i32 r) {
    f64 best = 0.0;
    for (i32 y = std::max(0, c.y - r); y <= std::min(i32(kH) - 1, c.y + r); ++y)
        for (i32 x = std::max(0, c.x - r); x <= std::min(i32(kW) - 1, c.x + r); ++x) {
            const usize p = usize(y) * kW + usize(x);
            best = std::max(best, luma(plain, p) - luma(lined, p));
        }
    return best;
}

// Pixels around `c` (radius r) that differ.
u32 changed_near(const Array<u8>& a, const Array<u8>& b, Px c, i32 r) {
    u32 n = 0;
    for (i32 y = std::max(0, c.y - r); y <= std::min(i32(kH) - 1, c.y + r); ++y)
        for (i32 x = std::max(0, c.x - r); x <= std::min(i32(kW) - 1, c.x + r); ++x)
            n += same_px(a, b, usize(y) * kW + usize(x)) ? 0u : 1u;
    return n;
}

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
};

// One frame of the outline scene (outline_scene) with `outline` (null: off).
Shot draw(Fixture3D& f, const EdgeOutline* outline, f32 fog = 0.0f, bool taa = false,
          const std::function<void(Scene3D&)>& more = {}) {
    f.render(fog, true, [&](Scene3D& s) {
        outline_scene(s, outline, taa);
        if (more) more(s);
    });
    CHECK(f.device->validation_error_count() == 0);
    return {f.pixels, f.renderer.forward_3d().last_stats()};
}

// The probe points of the fixture's frame (yaw 0, 35 degrees down: the camera looks along -z at the box's front face).
struct Probes {
    Px front_face, top_face, slab_open, slab_open2, sky;
    Px back_top_edge;     // the top face's far edge against the slab behind: a silhouette (both normals are +y)
    Px front_top_edge;    // the top face meets the front face: a crease (no depth jump)
    Px right_edge;        // the front face's right edge against the slab: a silhouette (and a normal change)
    Px slab_far_edge;     // the slab's far edge against the sky: a sky silhouette only
    Px small_box;         // the small box's front face centre, upper left
    Px small_box_mirror;  // its mirror across v = 0.5, on the open slab
};

Probes probes(const View3D& v) {
    Probes p;
    p.front_face = project(v, {0.0f, 0.5f, 0.5f});
    p.top_face = project(v, {0.0f, 1.0f, 0.0f});
    p.slab_open = project(v, {2.5f, 0.0f, 1.5f});
    p.slab_open2 = project(v, {-2.0f, 0.0f, -2.5f});
    p.back_top_edge = project(v, {0.0f, 1.0f, -0.5f});
    p.front_top_edge = project(v, {0.0f, 1.0f, 0.5f});
    p.right_edge = project(v, {0.5f, 0.5f, 0.5f});
    p.slab_far_edge = project(v, {1.0f, 0.0f, -6.0f});
    p.sky = {i32(kW) / 2 + 20, 3};
    p.small_box = project(v, kOutlineBoxAt + vec3{0.0f, 0.3f, 0.3f});
    p.small_box_mirror = {p.small_box.x, i32(kH) - 1 - p.small_box.y};
    return p;
}

struct CvarScope {  // sets a convar for the scope, resets it after
    ConVar* cv;
    CvarScope(const char* name, const char* value) : cv(ConVar::find(StringId(name))) {
        REQUIRE(cv != nullptr);
        REQUIRE(cv->set_string(value));
    }
    ~CvarScope() { cv->reset(); }
    CvarScope(const CvarScope&) = delete;
    CvarScope& operator=(const CvarScope&) = delete;
};

}  // namespace

TEST_CASE("edge outline: off, opacity 0 or both weights 0 draw the frame without the pass bit for bit") {
    Fixture3D f;
    upload_outline_box(f);
    const Shot plain = draw(f, nullptr);
    CHECK_FALSE(plain.stats.edge_outline);
    CHECK(plain.stats.edge_outline_taps == 0);
    CHECK(plain.stats.ssao_draws == 0);  // nothing else on the frame draws the prepass
    EdgeOutline off = outline_setting();
    off.enabled = false;
    EdgeOutline clear = outline_setting();
    clear.opacity = 0.0f;
    EdgeOutline weightless = outline_setting();
    weightless.depth_weight = 0.0f;
    weightless.normal_weight = 0.0f;
    for (const EdgeOutline* o : {&off, &clear, &weightless}) {
        const Shot s = draw(f, o);
        CHECK_FALSE(s.stats.edge_outline);
        CHECK(s.stats.edge_outline_taps == 0);
        CHECK(s.stats.ssao_draws == plain.stats.ssao_draws);  // no prepass on its account
        CHECK(same(s.px, plain.px));
    }
    // r.edge_outline 0 turns it off through apply_quality (the kit extract's path).
    {
        CvarScope cv("r.edge_outline", "0");
        PostFx post;
        post.outline = outline_setting();
        ShadowSettings shadows;
        apply_quality(quality_settings(QualityTier::High), shadows, post);
        CHECK_FALSE(post.outline.enabled);
        const Shot s = draw(f, &post.outline);
        CHECK_FALSE(s.stats.edge_outline);
        CHECK(same(s.px, plain.px));
    }
}

TEST_CASE("edge outline: Forward3DDesc::edge_outline false records nothing") {
    Forward3DDesc d;
    d.msaa = 4;
    d.edge_outline = false;
    Fixture3D f(4, true, true, 1.0f, &d);
    upload_outline_box(f);
    const Shot plain = draw(f, nullptr);
    const EdgeOutline o = outline_setting();
    const Shot s = draw(f, &o);
    CHECK_FALSE(s.stats.edge_outline);
    CHECK(s.stats.ssao_draws == 0);
    CHECK(same(s.px, plain.px));
}

TEST_CASE(
    "edge outline: lines on the silhouettes and creases, none inside flat faces, on the open slab or in the sky") {
    Fixture3D f;
    upload_outline_box(f);
    const Shot plain = draw(f, nullptr);
    const EdgeOutline o = outline_setting();
    const Shot lined = draw(f, &o);
    REQUIRE(lined.stats.edge_outline);
    CHECK(lined.stats.edge_outline_taps == edge_outline_tap_count(quality_settings(quality_tier()).edge_outline_wide));
    CHECK(lined.stats.ssao_draws > 0);  // the outline drew the prepass it reads
    const Probes p = probes(f.view);
    MESSAGE(changed_count(lined.px, plain.px) << " pixels changed");
    // Inside flat faces, on the open slab, in the sky: the frame without the pass exactly (3 px around each).
    for (const Px& c : {p.front_face, p.top_face, p.slab_open, p.slab_open2, p.sky, p.small_box_mirror}) {
        INFO("flat probe at " << c.x << ", " << c.y);
        CHECK(changed_near(lined.px, plain.px, c, 3) == 0);
    }
    // On the edges (within 2 px of the projected edge): darker by a clear step.
    for (const Px& c : {p.back_top_edge, p.front_top_edge, p.right_edge, p.slab_far_edge}) {
        const f64 drop = darkest_drop(lined.px, plain.px, c, 2);
        INFO("edge probe at " << c.x << ", " << c.y << ": luma drop " << drop);
        CHECK(drop > 20.0);
    }
    // The small box's own centre is inside its face, its outline around it (it is 0.6 m: a few pixels each way).
    CHECK(changed_near(lined.px, plain.px, p.small_box, 6) > 4);
    // The uv convention: the small box sits in the upper left; its mirror across v = 0.5 is open slab, untouched.
    CHECK(p.small_box.x < i32(kW) / 2);
    CHECK(p.small_box.y < i32(kH) / 2);
    CHECK(changed_near(lined.px, plain.px, p.small_box_mirror, 8) == 0);
    // Lines never brighten (dark colour, mix toward it).
    u32 brighter = 0;
    for (usize i = 0; i < usize(kW) * kH; ++i) brighter += luma(lined.px, i) > luma(plain.px, i) + 0.5 ? 1u : 0u;
    CHECK(brighter == 0);
}

TEST_CASE("edge outline: depth_weight 0 keeps the creases only, normal_weight 0 the silhouettes only") {
    Fixture3D f;
    upload_outline_box(f);
    const Shot plain = draw(f, nullptr);
    EdgeOutline creases = outline_setting();
    creases.depth_weight = 0.0f;
    EdgeOutline silhouettes = outline_setting();
    silhouettes.normal_weight = 0.0f;
    const Shot c = draw(f, &creases);
    const Shot s = draw(f, &silhouettes);
    REQUIRE(c.stats.edge_outline);
    REQUIRE(s.stats.edge_outline);
    const Probes p = probes(f.view);
    const f64 c_crease = darkest_drop(c.px, plain.px, p.front_top_edge, 2),
              c_sil = darkest_drop(c.px, plain.px, p.back_top_edge, 2);
    const f64 s_crease = darkest_drop(s.px, plain.px, p.front_top_edge, 2),
              s_sil = darkest_drop(s.px, plain.px, p.back_top_edge, 2);
    MESSAGE("creases only: crease " << c_crease << ", silhouette " << c_sil << "; silhouettes only: crease " << s_crease
                                    << ", silhouette " << s_sil);
    CHECK(c_crease > 20.0);
    CHECK(c_sil == 0.0);  // the top face and the slab behind share the +y normal
    CHECK(s_sil > 20.0);
    CHECK(s_crease == 0.0);
    // The slab's far edge against the sky is a silhouette (the sky has no normal).
    CHECK(changed_near(c.px, plain.px, p.slab_far_edge, 2) == 0);
    CHECK(darkest_drop(s.px, plain.px, p.slab_far_edge, 2) > 20.0);
}

TEST_CASE("edge outline: sky_silhouette off leaves geometry against the sky without a line") {
    Fixture3D f;
    upload_outline_box(f);
    const Shot plain = draw(f, nullptr);
    EdgeOutline on = outline_setting();
    EdgeOutline off = outline_setting();
    off.sky_silhouette = false;
    const Shot a = draw(f, &on);
    const Shot b = draw(f, &off);
    const Probes p = probes(f.view);
    CHECK(darkest_drop(a.px, plain.px, p.slab_far_edge, 2) > 20.0);
    CHECK(changed_near(b.px, plain.px, p.slab_far_edge, 2) == 0);
    CHECK(darkest_drop(b.px, plain.px, p.front_top_edge, 2) > 20.0);  // the rest of the lines stay
    // The sky itself never gets a line: the rows above the slab's far edge are untouched in both.
    const i32 top = project(f.view, {0.0f, 0.0f, -6.0f}).y - 3;
    REQUIRE(top > 4);
    u32 sky_changed = 0;
    for (i32 y = 0; y < top; ++y)
        for (i32 x = 0; x < i32(kW); ++x) {
            const usize i = usize(y) * kW + usize(x);
            sky_changed += (same_px(a.px, plain.px, i) && same_px(b.px, plain.px, i)) ? 0u : 1u;
        }
    CHECK(sky_changed == 0);
}

TEST_CASE("edge outline: a box beyond fade_end gets no line, exp2 fog weakens the lines") {
    Fixture3D f;
    upload_outline_box(f);
    const Shot plain = draw(f, nullptr);
    EdgeOutline near_fade = outline_setting();
    near_fade.fade_start = 1.0f;
    near_fade.fade_end = 2.0f;  // the camera is about 7 m from the box: everything is past the fade
    const Shot faded = draw(f, &near_fade);
    CHECK(faded.stats.edge_outline);  // recorded: the CPU cannot know the depths
    CHECK(same(faded.px, plain.px));

    const EdgeOutline o = outline_setting();
    const Shot clear_lines = draw(f, &o);
    const Shot fog_plain = draw(f, nullptr, 0.08f);
    const Shot fog_lines = draw(f, &o, 0.08f);
    EdgeOutline no_follow = outline_setting();
    no_follow.fog_follow = false;
    const Shot fog_unfollowed = draw(f, &no_follow, 0.08f);
    const Probes p = probes(f.view);
    const f64 clear_drop = darkest_drop(clear_lines.px, plain.px, p.front_top_edge, 2);
    const f64 fog_drop = darkest_drop(fog_lines.px, fog_plain.px, p.front_top_edge, 2);
    const f64 unfollowed_drop = darkest_drop(fog_unfollowed.px, fog_plain.px, p.front_top_edge, 2);
    MESSAGE("crease luma drop: clear " << clear_drop << ", fog " << fog_drop << ", fog without fog_follow "
                                       << unfollowed_drop);
    CHECK(fog_drop > 2.0);
    CHECK(fog_drop < 0.8 * unfollowed_drop);
}

TEST_CASE("edge outline: the tier picks the taps (Low 4, Medium and High 8), read by the renderer through r.quality") {
    Fixture3D f;
    upload_outline_box(f);
    const EdgeOutline o = outline_setting();
    Shot low, medium, high;
    {
        CvarScope q("r.quality", "low");
        low = draw(f, &o);
    }
    {
        CvarScope q("r.quality", "medium");
        medium = draw(f, &o);
    }
    {
        CvarScope q("r.quality", "high");
        high = draw(f, &o);
    }
    REQUIRE(low.stats.edge_outline);
    REQUIRE(medium.stats.edge_outline);
    REQUIRE(high.stats.edge_outline);
    // The Medium gate: the renderer's choice is the tier's explicit wide flag.
    CHECK(quality_settings(QualityTier::Medium).edge_outline_wide);
    CHECK_FALSE(quality_settings(QualityTier::Low).edge_outline_wide);
    CHECK(medium.stats.edge_outline_taps ==
          edge_outline_tap_count(quality_settings(QualityTier::Medium).edge_outline_wide));
    CHECK(medium.stats.edge_outline_taps == 8);
    CHECK(low.stats.edge_outline_taps == edge_outline_tap_count(quality_settings(QualityTier::Low).edge_outline_wide));
    CHECK(low.stats.edge_outline_taps == 4);
    CHECK(high.stats.edge_outline_taps == 8);
    CHECK(same(medium.px, high.px));  // same kernel, same frame
    const Shot plain = draw(f, nullptr);
    const u32 n4 = changed_count(low.px, plain.px), n8 = changed_count(high.px, plain.px);
    MESSAGE("lined pixels: 4 taps " << n4 << ", 8 taps " << n8);
    CHECK(n4 > 50);
    CHECK(n8 > n4);  // the diagonals catch what the cross misses
    CHECK_FALSE(same(low.px, high.px));
}

TEST_CASE(
    "edge outline: with TAA the lines stay on the edges, flicker bounded; disabled under TAA is the plain TAA frame") {
    const EdgeOutline o = outline_setting();
    EdgeOutline off = outline_setting();
    off.enabled = false;
    // 8 TAA frames of a fresh fixture each (one window at a time), with `outline` (null: off).
    auto sequence = [](const EdgeOutline* outline) {
        Fixture3D f;
        upload_outline_box(f);
        Array<Shot> shots;
        for (u32 i = 0; i < 8; ++i) shots.push_back(draw(f, outline, 0.0f, true));
        return shots;
    };
    const Array<Shot> plain = sequence(nullptr);
    const Array<Shot> disabled = sequence(&off);
    const Array<Shot> lined = sequence(&o);
    Array<u8> flat, flat_lined;  // one frame without TAA, plain and lined
    View3D view;
    {
        Fixture3D f;
        upload_outline_box(f);
        flat = draw(f, nullptr).px;
        flat_lined = draw(f, &o).px;
        view = f.view;  // the unjittered view the probes project with (TAA moves pixels by under one)
    }
    const Probes p = probes(view);
    int plain_step = 0, lined_step = 0;
    u32 plain_jumps = 0, lined_jumps = 0;  // pixels stepping by more than 32 from one frame to the next
    for (u32 i = 0; i < 8; ++i) {
        INFO("frame " << i);
        CHECK(plain[i].stats.taa);
        CHECK(lined[i].stats.taa);
        CHECK(lined[i].stats.edge_outline);
        CHECK(lined[i].stats.taa_history == (i > 0));
        CHECK_FALSE(disabled[i].stats.edge_outline);
        CHECK(same(disabled[i].px, plain[i].px));  // record_taa reads post_source_ = hdr_ when the outline did not draw
        for (const Px& c : {p.back_top_edge, p.front_top_edge, p.right_edge})
            CHECK(darkest_drop(lined[i].px, plain[i].px, c, 2) > 15.0);
        for (const Px& c : {p.front_face, p.slab_open, p.sky})
            CHECK(darkest_drop(lined[i].px, plain[i].px, c, 1) < 4.0);
        if (i >= 4) {  // the history has settled: frame-to-frame steps
            plain_step = std::max(plain_step, max_diff(plain[i].px, plain[i - 1].px));
            lined_step = std::max(lined_step, max_diff(lined[i].px, lined[i - 1].px));
            for (usize k = 0; k < usize(kW) * kH; ++k) {
                plain_jumps += std::fabs(luma(plain[i].px, k) - luma(plain[i - 1].px, k)) > 32.0 ? 1u : 0u;
                lined_jumps += std::fabs(luma(lined[i].px, k) - luma(lined[i - 1].px, k)) > 32.0 ? 1u : 0u;
            }
        }
    }
    // TAA off / on: the TAA frame darkens where the frame without TAA has its line cores (darker by more than 40).
    u32 cores = 0, kept = 0;
    for (usize k = 0; k < usize(kW) * kH; ++k) {
        if (luma(flat, k) - luma(flat_lined, k) <= 40.0) continue;
        ++cores;
        kept += luma(plain[7].px, k) - luma(lined[7].px, k) > 5.0 ? 1u : 0u;
    }
    MESSAGE("line cores without TAA " << cores << ", darkened under TAA " << kept
                                      << "; frame-to-frame max step, frames 4..7: plain " << plain_step << ", lined "
                                      << lined_step << "; pixels stepping by over 32, frames 4..7: plain "
                                      << plain_jumps << ", lined " << lined_jumps);
    CHECK(cores > 30);
    CHECK(kept > cores * 8 / 10);
    // Bounded flicker: the source is the half-resolution prepass, drawn with the jittered projection, so under the
    // jitter an edge moves by one prepass texel (two pixels) in some frames and TAA's neighbourhood clamp follows the
    // current frame there (llvmpipe, 2026-11-04: 156 pixel steps over 32 in the 4 settled frame pairs against 476 line
    // pixels, about 8 % a frame; the plain frame has none). Recorded in the milestone's risks and on the GPU list
    // (A12); this pins it from growing: under a quarter of the line pixels a frame.
    CHECK(plain_jumps == 0);
    CHECK(lined_jumps < cores);
}

TEST_CASE("edge outline: without SSAO, SSR and contact shadows it still draws, and the water is unchanged") {
    Forward3DDesc d;
    d.msaa = 4;
    d.ssao = false;
    d.ssr = false;
    d.contact_shadows = false;
    {
        Fixture3D f(4, true, true, 1.0f, &d);
        upload_outline_box(f);
        const Shot plain = draw(f, nullptr);
        const EdgeOutline o = outline_setting();
        const Shot lined = draw(f, &o);
        CHECK(lined.stats.edge_outline);
        CHECK(changed_count(lined.px, plain.px) > 50);
    }

    // The water's depth read follows SSAO, SSR and contact shadows only: a renderer that has the prepass for the
    // outline alone draws its water exactly as one without the outline pipelines.
    Forward3DDesc d_none = d;
    d_none.edge_outline = false;
    Array<u8> water_with, water_without;
    for (int k = 0; k < 2; ++k) {
        Fixture3D w(4, true, true, 1.0f, k == 0 ? &d : &d_none);
        water_camera(w);
        w.render(0.0f, true, [](Scene3D& s) { water_day_scene(s, water_carrier(0.25f)); });
        CHECK(w.device->validation_error_count() == 0);
        CHECK(w.renderer.forward_3d().last_stats().ssao_draws == 0);  // no depth read: the prepass is not drawn
        (k == 0 ? water_with : water_without) = w.pixels;
    }
    MESSAGE("water frame, max channel difference " << max_diff(water_with, water_without));
    CHECK(same(water_with, water_without));
}

TEST_CASE("edge outline: two views of different sizes in one command buffer each match the view drawn alone") {
    Fixture3D f;
    upload_outline_box(f);
    constexpr u32 kSmallW = 96, kSmallH = 72;
    rhi::TextureDesc td;
    td.size = {kSmallW, kSmallH};
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.small";
    const rhi::TextureHandle small = f.device->create_texture(td);
    const EdgeOutline o = outline_setting();
    auto record = [&](rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D size, u32 slot, bool first) {
        const Camera3D cam = orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(35.0f), 7.0f);
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(cam, size);
        DirectionalLight sun;
        sun.direction = {-0.5f, -1.0f, -0.3f};
        scene.set_light(sun);
        ShadowSettings ss;
        ss.resolution = 1024;
        scene.set_shadows(ss);
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        MeshInstance box;
        box.mesh = 1;
        scene.add(box);
        outline_scene(scene, &o);
        scene.build();
        Forward3D::ViewOutput out;
        out.slot = slot;
        out.first = first;
        f.renderer.record_3d(cmd, scene, target, out);
        CHECK(f.renderer.forward_3d().last_stats().edge_outline);
    };
    Array<u8> big_alone, small_alone, big, small_px;
    big_alone.resize(usize(kW) * kH * 4);
    big.resize(usize(kW) * kH * 4);
    small_alone.resize(usize(kSmallW) * kSmallH * 4);
    small_px.resize(usize(kSmallW) * kSmallH * 4);
    // Each alone, in a fresh slot, its own frame.
    for (int k = 0; k < 2; ++k) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        if (k == 0)
            record(cmd, f.target, {kW, kH}, 1, true);
        else
            record(cmd, small, {kSmallW, kSmallH}, 2, true);
        f.device->submit(cmd);
        if (k == 0)
            REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, big_alone.span()).has_value());
        else
            REQUIRE(f.device->read_pixels(small, rhi::Rect2D{0, 0, kSmallW, kSmallH}, small_alone.span()).has_value());
        f.device->end_frame();
    }
    // Both in one command buffer, in two more fresh slots, frame after frame, the big view first and then the small one
    // first: each frame's pair matches the views alone (the shared outline target is never freed under an earlier
    // view).
    for (int frame = 0; frame < 3; ++frame) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        const bool big_first = frame != 1;
        if (big_first) {
            record(cmd, f.target, {kW, kH}, 3, true);
            record(cmd, small, {kSmallW, kSmallH}, 4, false);
        } else {
            record(cmd, small, {kSmallW, kSmallH}, 4, true);
            record(cmd, f.target, {kW, kH}, 3, false);
        }
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, big.span()).has_value());
        REQUIRE(f.device->read_pixels(small, rhi::Rect2D{0, 0, kSmallW, kSmallH}, small_px.span()).has_value());
        f.device->end_frame();
        INFO("frame " << frame << (big_first ? ", big view first" : ", small view first"));
        CHECK(f.device->validation_error_count() == 0);
        CHECK(same(big, big_alone));
        CHECK(same(small_px, small_alone));
    }
    f.device->destroy_texture(small);
}

TEST_CASE(
    "edge outline: Volumetrics asked for but not drawn (no volumetric pipelines): the lines fade in the exp2 fog") {
    // The main pass swaps the exp^2 Fog for the volume only when the volume is recorded (stats().volumetrics); the
    // outline follows the same decision, not Volumetrics::enabled alone.
    Forward3DDesc d;
    d.msaa = 4;
    d.volumetrics = false;
    Fixture3D f(4, true, true, 1.0f, &d);
    upload_outline_box(f);
    const EdgeOutline o = outline_setting();
    const auto volume = [](Scene3D& s) {
        Volumetrics v;
        v.enabled = true;
        s.set_volumetrics(v);
    };
    const Shot fog_lines = draw(f, &o, 0.08f);
    const Shot vol_lines = draw(f, &o, 0.08f, false, volume);
    const Shot vol_plain = draw(f, nullptr, 0.08f, false, volume);
    CHECK_FALSE(vol_lines.stats.volumetrics);
    REQUIRE(vol_lines.stats.edge_outline);
    MESSAGE("max channel difference to the lines in the fog without Volumetrics "
            << max_diff(vol_lines.px, fog_lines.px));
    CHECK(same(vol_lines.px, fog_lines.px));
    EdgeOutline no_follow = outline_setting();
    no_follow.fog_follow = false;
    const Shot vol_unfollowed = draw(f, &no_follow, 0.08f, false, volume);
    const Probes p = probes(f.view);
    CHECK(darkest_drop(vol_lines.px, vol_plain.px, p.front_top_edge, 2) <
          0.8 * darkest_drop(vol_unfollowed.px, vol_plain.px, p.front_top_edge, 2));
}

TEST_CASE("edge outline: -r.edge_outline 0 at init creates no outline pipeline (and, alone, no prepass or flare)") {
    CvarScope cv("r.edge_outline", "0");
    Forward3DDesc d;
    d.msaa = 4;
    d.ssao = false;
    d.ssr = false;
    d.contact_shadows = false;
    Fixture3D f(4, true, true, 1.0f, &d);
    upload_outline_box(f);
    const Shot plain = draw(f, nullptr);
    const EdgeOutline o = outline_setting();  // set on the scene directly: only the init-time read can stop it
    const Shot s = draw(f, &o);
    CHECK_FALSE(s.stats.edge_outline);
    CHECK(s.stats.ssao_draws == 0);
    CHECK(same(s.px, plain.px));
    // Without SSAO, SSR, contact shadows and the outline there is no prepass, so no lens flare either (as before 39c).
    LensFlare fl;
    fl.enabled = true;
    const Shot flare = draw(f, &o, 0.0f, false, [&](Scene3D& sc) {
        PostFx post = sc.post();
        post.flare = fl;
        sc.set_post(post);
    });
    CHECK_FALSE(flare.stats.lens_flare);
    CHECK(flare.stats.ssao_draws == 0);
}
