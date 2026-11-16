// M-HEATHAZE 43c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: heat haze, one
// fullscreen pass from the HDR frame and the half-resolution opaque prepass into the post-fx ping-pong target, after
// the lens rain and before bloom (Forward3D::record_heat_haze, kHazeFS). No volume, strength 0, a volume behind the
// camera or off screen, the Low tier, r.heat_haze 0 and a render-to-texture view draw the frame without the pass bit
// for bit (the last three record nothing); a volume on screen changes pixels only inside the screen disc
// heat_haze_project gives (plus a pixel), and many of them; the same time gives the same frame, another time another,
// and the rising field's 256-cell wrap does not pop; a pole in front of the haze stays sharp (its pixels unchanged)
// while the checker behind it moves, and no displaced lookup pulls the pole's colour into the background (the
// nearer-pixel fallback); the pass runs after the lens rain (its drops are bent: pixels outside every drop change)
// and before bloom (with bloom, pixels outside the disc change too); with TAA and auto exposure the next frame is the
// same whether the frame before had haze or not (no haze in the history or the exposure); the tier caps the volumes
// (Medium 4, High 8); and the GLSL port of heat_haze_hash equals the C++ hash bit for bit, run alone on the GPU. One
// 160 x 120 frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/heat_haze.h"
#include "mce/render/quality.h"
#include "shaders3d.h"  // render's 3D shaders: the hash port and the fullscreen vertex stage

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

constexpr u32 kWall = 70, kPole = 71;
constexpr f32 kAspect = f32(kW) / f32(kH);
constexpr f32 kPx = 1.0f / f32(kH);  // one pixel, in view heights

bool same(const Array<u8>& a, const Array<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize p) {
    return a[p * 4] == b[p * 4] && a[p * 4 + 1] == b[p * 4 + 1] && a[p * 4 + 2] == b[p * 4 + 2];
}

int px_diff(const Array<u8>& a, const Array<u8>& b, usize p) {
    int m = 0;
    for (usize c = 0; c < 3; ++c) m = std::max(m, std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c])));
    return m;
}

u32 changed_px(const Array<u8>& a, const Array<u8>& b) {
    u32 n = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) n += same_px(a, b, p) ? 0u : 1u;
    return n;
}

u32 moved_px(const Array<u8>& a, const Array<u8>& b, int step) {
    u32 n = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) n += px_diff(a, b, p) > step ? 1u : 0u;
    return n;
}

// The pixel's centre in view heights from the bottom left (kHazeFS's p); read_pixels is top row first.
math::vec2 pos_of(usize p) {
    const usize x = p % kW, y = p / kW;
    return {(f32(x) + 0.5f) / f32(kW) * kAspect, 1.0f - (f32(y) + 0.5f) / f32(kH)};
}

bool green(const Array<u8>& px, usize p) {
    return int(px[p * 4 + 1]) > int(px[p * 4]) + 40 && int(px[p * 4 + 1]) > int(px[p * 4 + 2]) + 40;
}

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
    View3D view;
};

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

struct HazeFixture {
    Fixture3D f;
    HazeFixture() {
        upload_haze_wall(f, kWall);
        upload_haze_pole(f, kPole);
        haze_camera(f);
    }
    // One frame of the wall scene with `haze` (null: none) at `time`; `pole` adds the pole in front of the haze.
    Shot draw(const HeatHaze* haze, f32 time = 2.0f, bool pole = false,
              const std::function<void(Scene3D&)>& more = {}) {
        f.render(0.0f, true, [&](Scene3D& s) {
            heat_haze_scene(s, haze, time, kWall, pole ? kPole : ~0u, {0.3f, 0.0f, 3.5f});
            if (more) more(s);
        });
        CHECK(f.device->validation_error_count() == 0);
        return {f.pixels, f.renderer.forward_3d().last_stats(), f.view};
    }
};

// The screen disc of volume `v` (heat_haze_project with the frame's own matrices), in view heights.
struct Disc {
    math::vec2 c;
    f32 r = 0.0f;
    f32 near = 0.0f;
};

Disc disc_of(const View3D& view, const HeatHazeVolume& v) {
    HazeScreen s;
    REQUIRE(heat_haze_project(view.view_proj, view.view, v.center, v.radius, s));
    return {{s.center.x * kAspect, s.center.y}, s.radius, s.near_depth};
}

f32 dist(math::vec2 a, math::vec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

}  // namespace

TEST_CASE("heat haze: no volume, strength 0, a volume behind the camera or off screen draw the frame without the pass") {
    HazeFixture h;
    const Shot plain = h.draw(nullptr);
    CHECK(plain.stats.heat_haze_volumes == 0);
    // count 0 with volumes filled in: nothing is read.
    HeatHaze none = haze_setting();
    none.count = 0;
    HeatHaze behind = haze_setting({0.0f, 1.5f, 12.0f}, 1.0f);   // the camera sits at z = 7, looking down -z
    HeatHaze aside = haze_setting({40.0f, 1.5f, 0.0f}, 1.0f);    // far off the left/right border
    for (const HeatHaze* hz : {&none, &behind, &aside}) {
        const Shot s = h.draw(hz);
        CHECK(s.stats.heat_haze_volumes == 0);
        CHECK(same(s.px, plain.px));
    }
    // Strength 0 records the pass (the volume is on screen) but moves nothing: the copy is exact.
    HeatHaze still = haze_setting();
    still.volumes[0].strength = 0.0f;
    const Shot s = h.draw(&still);
    CHECK(s.stats.heat_haze_volumes == 1);
    CHECK(same(s.px, plain.px));
}

TEST_CASE("heat haze: a volume changes pixels only inside its screen disc, and many of them") {
    HazeFixture h;
    const Shot plain = h.draw(nullptr);
    // Off the middle in both directions (up and to the right), so a backend that flipped the uv would miss the disc.
    const HeatHaze hz = haze_setting({1.2f, 2.4f, 0.0f}, 1.0f);
    const Shot hot = h.draw(&hz);
    REQUIRE(hot.stats.heat_haze_volumes == 1);
    const Disc d = disc_of(hot.view, hz.volumes[0]);
    CHECK(d.c.x > 0.5f * kAspect + 0.05f);
    CHECK(d.c.y > 0.55f);
    MESSAGE("disc centre " << d.c.x << ", " << d.c.y << ", radius " << d.r << " view heights, near " << d.near << " m");
    CHECK(d.r > 0.1f);
    CHECK(d.r < 0.3f);
    u32 changed = 0, outside = 0, inside = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        const bool in = dist(pos_of(p), d.c) < d.r;
        inside += in ? 1u : 0u;
        if (same_px(hot.px, plain.px, p)) continue;
        ++changed;
        outside += dist(pos_of(p), d.c) < d.r + kPx ? 0u : 1u;
    }
    MESSAGE(changed << " of " << inside << " pixels in the disc changed, " << outside << " outside it");
    CHECK(outside == 0);
    CHECK(changed > inside / 5);
}

TEST_CASE("heat haze: the same time gives the same frame, another time another; the rise wrap does not pop") {
    HazeFixture h;
    const HeatHaze hz = haze_setting();
    const Shot a = h.draw(&hz, 3.0f);
    const Shot b = h.draw(&hz, 3.0f);
    CHECK(same(a.px, b.px));
    const Shot c = h.draw(&hz, 5.0f);
    MESSAGE("3 s vs 5 s: " << changed_px(a.px, c.px) << " pixels differ");
    CHECK(changed_px(a.px, c.px) > 200);
    // The rise phase (rise * kHazeCells * scale cells per second) wraps at 256 cells: 256 / 1.8 s with the defaults.
    // 10 ms across the wrap moves about as much as 10 ms anywhere else (a sub-pixel move flips a level along the
    // checker's edges; count the pixels that moved by more than 8 levels).
    const f32 wrap = shaders3d::kHazePeriodCells / (0.6f * shaders3d::kHazeCells);
    const Shot w0 = h.draw(&hz, wrap - 0.005f);
    const Shot w1 = h.draw(&hz, wrap + 0.005f);
    const Shot t0 = h.draw(&hz, 30.0f);
    const Shot t1 = h.draw(&hz, 30.01f);
    const u32 across = moved_px(w0.px, w1.px, 8), step = moved_px(t0.px, t1.px, 8), far = moved_px(t0.px, c.px, 8);
    MESSAGE("moved by more than 8 levels: across the wrap " << across << ", 10 ms " << step << ", far " << far);
    CHECK(across < 4 * step + 60);
    CHECK(far > 4 * (across > step ? across : step) + 100);
}

TEST_CASE("heat haze: a pole in front of the haze stays sharp while the checker behind it moves") {
    HazeFixture h;
    const HeatHaze hz = haze_setting();
    const Shot plain = h.draw(nullptr, 2.0f, true);
    const Shot hot = h.draw(&hz, 2.0f, true);
    REQUIRE(hot.stats.heat_haze_volumes == 1);
    const Disc d = disc_of(hot.view, hz.volumes[0]);
    auto pole_at = [&](i32 x, i32 y) {
        if (x < 0 || y < 0 || x >= i32(kW) || y >= i32(kH)) return false;
        return green(plain.px, usize(y) * kW + usize(x));
    };
    u32 pole = 0, pole_core = 0, core_changed = 0, background_moved = 0, pulled = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        const i32 x = i32(p % kW), y = i32(p / kW);
        const bool in_disc = dist(pos_of(p), d.c) < d.r;
        if (!in_disc) continue;
        // Within 2 pixels of the pole (the half-resolution prepass decides its edge to a texel: one full pixel).
        bool near_pole = false, core = true;
        for (i32 dy = -2; dy <= 2; ++dy)
            for (i32 dx = -2; dx <= 2; ++dx) {
                near_pole = near_pole || pole_at(x + dx, y + dy);
                core = core && pole_at(x + dx, y + dy);
            }
        if (pole_at(x, y)) ++pole;
        if (core) {
            ++pole_core;
            core_changed += same_px(hot.px, plain.px, p) ? 0u : 1u;
        }
        if (!near_pole) background_moved += same_px(hot.px, plain.px, p) ? 0u : 1u;
        // The pole's colour where the frame without haze has none, a pixel or more off the pole.
        bool touches = false;
        for (i32 dy = -1; dy <= 1; ++dy)
            for (i32 dx = -1; dx <= 1; ++dx) touches = touches || pole_at(x + dx, y + dy);
        if (!touches) pulled += green(hot.px, p) ? 1u : 0u;
    }
    MESSAGE("pole pixels in the disc " << pole << " (core " << pole_core << ", changed " << core_changed
                                       << "), background moved " << background_moved << ", pole colour pulled out "
                                       << pulled);
    CHECK(pole > 100);
    CHECK(pole_core > 30);
    CHECK(core_changed == 0);
    CHECK(background_moved > 300);
    CHECK(pulled == 0);
}

TEST_CASE("heat haze: the pass runs after the lens rain") {
    // After the lens rain: with drops that change only their own discs (no blur, no trails, still drops), the haze
    // pulls drop pixels into their surroundings, so pixels outside every drop differ between rain + haze and haze
    // alone. Were the haze before the rain, the rain would leave those pixels as the haze drew them.
    HazeFixture h;
    HeatHaze hz = haze_setting();
    hz.volumes[0].strength = 2.0f;
    LensRain rain = lens_rain_setting();
    const auto with_rain = [&](Scene3D& s) {
        PostFx post = s.post();
        post.rain = rain;
        s.set_post(post);
    };
    const Shot plain = h.draw(nullptr);
    const Shot r = h.draw(nullptr, 2.0f, false, with_rain);
    const Shot hh = h.draw(&hz);
    const Shot rh = h.draw(&hz, 2.0f, false, with_rain);
    REQUIRE(r.stats.lens_rain);
    REQUIRE(rh.stats.lens_rain);
    REQUIRE(rh.stats.heat_haze_volumes == 1);
    const Disc d = disc_of(rh.view, hz.volumes[0]);
    auto drop_at = [&](i32 x, i32 y) {
        if (x < 0 || y < 0 || x >= i32(kW) || y >= i32(kH)) return false;
        return !same_px(r.px, plain.px, usize(y) * kW + usize(x));
    };
    u32 outside_drops = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        if (!(dist(pos_of(p), d.c) < d.r)) continue;
        const i32 x = i32(p % kW), y = i32(p / kW);
        bool near_drop = false;
        for (i32 dy = -1; dy <= 1; ++dy)
            for (i32 dx = -1; dx <= 1; ++dx) near_drop = near_drop || drop_at(x + dx, y + dy);
        if (!near_drop && !same_px(rh.px, hh.px, p)) ++outside_drops;
    }
    MESSAGE(outside_drops << " pixels outside every drop differ between rain + haze and haze alone");
    CHECK(outside_drops > 20);
}

TEST_CASE("heat haze: the pass runs before bloom") {
    // Before bloom: the bloom of the bent frame spreads the change past the disc. Were the haze after bloom, every
    // pixel outside the disc would be the frame without haze exactly. A bright sky over a dark ground: the haze bends
    // the horizon, bloom carries the bent light outward.
    const auto bright = [&](Scene3D& s) {
        no_sun(s);
        Sky sky;
        sky.zenith = sky.horizon = {30.0f, 30.0f, 30.0f};
        sky.ground = {0.0f, 0.0f, 0.0f};
        sky.ibl = false;
        s.set_sky(sky);
        PostFx post = s.post();
        post.bloom = true;
        post.bloom_intensity = 0.5f;
        s.set_post(post);
    };
    HazeFixture g;
    g.f.look_at = {0.0f, 50.0f, 0.0f};  // the horizon across the middle, nothing but sky and ground in view
    HeatHaze over = haze_setting({0.0f, 50.0f, -10.0f}, 4.0f, 2.0f);
    const Shot b0 = g.draw(nullptr, 2.0f, false, [&](Scene3D& s) {
        bright(s);
    });
    const Shot b1 = g.draw(&over, 2.0f, false, [&](Scene3D& s) {
        bright(s);
    });
    REQUIRE(b1.stats.heat_haze_volumes == 1);
    const Disc bd = disc_of(b1.view, over.volumes[0]);
    u32 inside = 0, beyond = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        if (same_px(b0.px, b1.px, p)) continue;
        if (dist(pos_of(p), bd.c) < bd.r + kPx) ++inside;
        else ++beyond;
    }
    MESSAGE("with bloom: " << inside << " changed pixels in the disc, " << beyond << " beyond it");
    CHECK(inside > 50);
    CHECK(beyond > 0);
}

TEST_CASE("heat haze: with TAA and auto exposure the next frame holds no haze") {
    const HeatHaze hz = haze_setting();
    auto two_frames = [&](bool haze_first) {
        HazeFixture h;
        const auto setup = [](Scene3D& s) {
            PostFx post = s.post();
            post.taa = true;
            post.auto_exposure.enabled = true;
            s.set_post(post);
        };
        const Shot first = h.draw(haze_first ? &hz : nullptr, 2.0f, false, setup);
        CHECK(first.stats.taa);
        CHECK(first.stats.heat_haze_volumes == (haze_first ? 1u : 0u));
        const Shot second = h.draw(nullptr, 2.0f, false, setup);
        CHECK(second.stats.taa_history);
        CHECK(second.stats.heat_haze_volumes == 0);
        return second.px;
    };
    const Array<u8> after_haze = two_frames(true);
    const Array<u8> after_plain = two_frames(false);
    CHECK(same(after_haze, after_plain));
}

TEST_CASE("heat haze: the Low tier and r.heat_haze 0 record nothing; Medium draws up to 4 volumes, High up to 8") {
    HeatHaze six;
    six.count = 6;
    for (u32 i = 0; i < 6; ++i) {
        six.volumes[i].center = {-2.5f + f32(i), 1.5f, 0.0f};
        six.volumes[i].radius = 0.4f;
        six.volumes[i].strength = 1.5f;
    }
    // apply_quality trims the count: Low 0, Medium 4, High as set.
    ShadowSettings shadows;
    PostFx low, medium, high;
    low.haze = medium.haze = high.haze = six;
    apply_quality(quality_settings(QualityTier::Low), shadows, low);
    apply_quality(quality_settings(QualityTier::Medium), shadows, medium);
    apply_quality(quality_settings(QualityTier::High), shadows, high);
    CHECK(low.haze.count == 0);
    CHECK(medium.haze.count == 4);
    CHECK(high.haze.count == 6);

    // The renderer reads the tier and the cvar itself too: a scene that skipped apply_quality gets the same caps.
    HazeFixture h;
    for (const auto& [name, value] : {std::pair{"r.quality", "low"}, std::pair{"r.heat_haze", "0"}}) {
        CvarScope scope(name, value);
        const Shot plain = h.draw(nullptr);
        const Shot hot = h.draw(&six);
        CHECK(hot.stats.heat_haze_volumes == 0);
        CHECK(same(hot.px, plain.px));
    }
    {
        CvarScope scope("r.quality", "medium");
        const Shot plain = h.draw(nullptr);
        const Shot hot = h.draw(&six);
        CHECK(hot.stats.heat_haze_volumes == 4);
        // The first four volumes in the setting's order: nothing changes inside the last two discs.
        u32 in_dropped = 0, in_kept = 0;
        for (usize p = 0; p < usize(kW) * kH; ++p) {
            if (same_px(hot.px, plain.px, p)) continue;
            bool kept = false, dropped = false;
            for (u32 i = 0; i < 6; ++i) {
                const Disc d = disc_of(hot.view, six.volumes[i]);
                if (dist(pos_of(p), d.c) < d.r + kPx) (i < 4 ? kept : dropped) = true;
            }
            in_kept += kept ? 1u : 0u;
            in_dropped += dropped && !kept ? 1u : 0u;
        }
        MESSAGE("Medium: " << in_kept << " changed pixels in the four kept discs, " << in_dropped << " in the others");
        CHECK(in_kept > 50);
        CHECK(in_dropped == 0);
    }
    const Shot all = h.draw(&six);
    CHECK(all.stats.heat_haze_volumes == 6);
}

TEST_CASE("heat haze: a render-to-texture view records no haze") {
    HazeFixture h;
    const HeatHaze hz = haze_setting();
    for (const bool linear : {false, true}) {
        h.f.device->begin_frame(h.f.frame++);
        rhi::CommandBuffer cmd;
        Scene3D scene(h.f.renderer.meshes(), mem::heap());
        scene.begin(orbit_camera({0.0f, 1.5f, 0.0f}, 0.0f, 0.0f, 7.0f), {kW, kH});
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        heat_haze_scene(scene, &hz, 2.0f, kWall);
        scene.build();
        Forward3D::ViewOutput out;
        out.slot = 1;
        out.linear_output = linear;
        h.f.renderer.record_3d(cmd, scene, h.f.target, out);
        CHECK(h.f.renderer.forward_3d().last_stats().heat_haze_volumes == (linear ? 0u : 1u));
        h.f.device->submit(cmd);
        h.f.device->end_frame();
        CHECK(h.f.device->validation_error_count() == 0);
    }
}

TEST_CASE("heat haze: the shader's hash is heat_haze_hash bit for bit") {
    // kHazeFS's hh_hash (MCE_GLSL_HEAT_HAZE_HASH) run alone: one row of 64 pixels, each hashing inputs made from its
    // column (column 0 the all-ones case of the CPU goldens), written as four bytes and read back. One row, so the
    // backends' row order does not matter.
    Fixture3D f;
    constexpr const char* kFS = "#version 450 core\n" MCE_GLSL_HEAT_HAZE_HASH R"(
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    uint x = uint(gl_FragCoord.x);
    uint h = x == 0u ? hh_hash(0xffffffffu, 0xffffffffu, 0xffffffffu)
                     : hh_hash(x * 0x9e3779b9u, (x ^ 0x5bd1e995u) * 0x01000193u, x * 0x27d4eb2fu + 7u);
    o_color = unpackUnorm4x8(h);
}
)";
    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = {shaders3d::kFullscreenVS, "fullscreen.vert"};
    pd.fragment_shader = {kFS, "haze_hash_test.frag"};
    pd.state.depth = {};
    pd.debug_name = "test.haze_hash";
    auto pipe = f.device->create_pipeline(pd);
    REQUIRE(pipe.has_value());
    constexpr u32 kN = 64;
    rhi::TextureDesc td;
    td.size = {kN, 1};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.haze_hash";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    f.device->begin_frame(f.frame++);
    rhi::CommandBuffer cmd;
    rhi::PassAttachment o;
    o.texture = rt;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&o, 1);
    p.debug_name = "test.haze_hash";
    cmd.begin_pass(p);
    cmd.bind_pipeline(pipe.value());
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    f.device->submit(cmd);
    Array<u8> px;
    px.resize(usize(kN) * 4);
    REQUIRE(f.device->read_pixels(rt, rhi::Rect2D{0, 0, kN, 1}, px.span()).has_value());
    f.device->end_frame();
    CHECK(f.device->validation_error_count() == 0);
    u32 match = 0;
    for (u32 x = 0; x < kN; ++x) {
        const u32 want = x == 0 ? heat_haze_hash(0xffffffffu, 0xffffffffu, 0xffffffffu)
                                : heat_haze_hash(x * 0x9e3779b9u, (x ^ 0x5bd1e995u) * 0x01000193u, x * 0x27d4eb2fu + 7u);
        const u32 got = u32(px[x * 4]) | (u32(px[x * 4 + 1]) << 8) | (u32(px[x * 4 + 2]) << 16) |
                        (u32(px[x * 4 + 3]) << 24);
        INFO("column " << x);
        CHECK(got == want);
        match += got == want ? 1u : 0u;
    }
    CHECK(match == kN);
    CHECK(heat_haze_hash(0xffffffffu, 0xffffffffu, 0xffffffffu) == 0x828050a8u);  // the CPU golden it rests on
    f.device->destroy_texture(rt);
    f.device->destroy_pipeline(pipe.value());
}
