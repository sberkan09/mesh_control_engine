// M-UNDERWATER 44c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the camera under water,
// one fullscreen pass from the HDR frame (and, at detail 1 and 2, the half-resolution opaque prepass) into the post-fx
// ping-pong target, after the flare and before the lens rain, the heat haze and bloom (Forward3D::record_underwater,
// kUnderwaterFS). No water, a disabled carrier, PostFx::underwater.enabled false, strength 0, r.underwater 0 and the camera
// above the crossing band draw the frame without the pass bit for bit and record nothing (the same passes and draws);
// below the band the whole frame changes; three equal grey pillars at 3, 8 and 20 m lose red faster than blue and more
// with distance, and the far ones near the colour of the far, empty water (the scatter colour); an up-going pixel whose
// ray meets the surface first is the lid, whatever is drawn above the water (or the sky's brightness), while a pillar
// below the surface met first by an up-going ray stays visible; the Snell window straight up is brighter than the lid at
// 30 degrees; the crossing sweep from 1.5 m above to 1.5 m below the level in 5 cm steps adds no step of its own to the
// plain sweep's (no pop), its top frame is the plain frame and its bottom frame the full look; the wobble is a function
// of the scene time (the same time the same frame, another time another, the 64 s whole-cycle wrap without a pop and
// exact); with TAA and auto exposure the next frame holds no look; the pass runs after the lens rain's input (the drops
// refract the lid), before the heat haze (the haze bends the lid) and before bloom (bloom lights the lid); the tiers (Low:
// a flat, per-colour tint and no prepass; Medium one wobble octave, High two); without the prepass pipeline the pass
// falls back to the flat tint; a render-to-texture view records nothing; and the GLSL ports of underwater_window,
// _lid_distance and _transmittance (and the fog mix built on them) equal the C++ helpers, run alone on the GPU. One
// 160 x 120 frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/heat_haze.h"
#include "mce/render/quality.h"
#include "mce/render/underwater.h"
#include "shaders3d.h"  // render's 3D shaders: the helper ports and the fullscreen vertex stage

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

constexpr u32 kPillars = 76, kAbove = 77;

bool same(const Array<u8>& a, const Array<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize p) {
    return a[p * 4] == b[p * 4] && a[p * 4 + 1] == b[p * 4 + 1] && a[p * 4 + 2] == b[p * 4 + 2];
}

u32 changed_px(const Array<u8>& a, const Array<u8>& b) {
    u32 n = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) n += same_px(a, b, p) ? 0u : 1u;
    return n;
}

int px_diff(const Array<u8>& a, const Array<u8>& b, usize p) {
    int m = 0;
    for (usize c = 0; c < 3; ++c) m = std::max(m, std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c])));
    return m;
}

u32 moved_px(const Array<u8>& a, const Array<u8>& b, int step) {
    u32 n = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) n += px_diff(a, b, p) > step ? 1u : 0u;
    return n;
}

// The mean over the frame of the mean absolute channel difference, in 8-bit levels.
f64 mean_diff(const Array<u8>& a, const Array<u8>& b) {
    f64 sum = 0.0;
    for (usize p = 0; p < usize(kW) * kH; ++p)
        for (usize c = 0; c < 3; ++c) sum += std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c]));
    return sum / f64(usize(kW) * kH * 3);
}

Rgb rgb_at(const Array<u8>& px, usize x, usize y) {
    const usize i = (y * kW + x) * 4;
    return {px[i], px[i + 1], px[i + 2]};
}

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
    rhi::FrameStats gpu;
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

Underwater look(bool enabled = true) {
    Underwater u;
    u.enabled = enabled;
    return u;
}

// The camera 2 m under the surface looking level toward -z over the pillars.
constexpr vec3 kLevelCam{0.0f, 1.2f, 7.0f};
// The camera 2.8 m under the surface looking 40 degrees up: nothing but the lid in view (the window's edge across the
// middle), so every pixel of the full look is independent of the scene colour.
constexpr vec3 kUpCam{0.0f, 0.4f, 5.0f};

struct UwFixture {
    Fixture3D f;
    explicit UwFixture(const Forward3DDesc* desc = nullptr) : f(4, true, true, 1.0f, desc) {
        upload_underwater_pillars(f, kPillars);
        mesh_fixture::Builder b;  // a box floating above the water, in view of the level camera
        b.add_node(-1);
        mesh_fixture::Box box;
        box.center = {0.8f, 5.5f, -8.0f};
        box.half = {1.0f, 1.0f, 1.0f};
        box.color[0] = 0.9f;
        box.color[1] = 0.8f;
        box.color[2] = 0.1f;
        b.add_box(box);
        REQUIRE(f.renderer.meshes().upload(kAbove, 1, b.build().span()).has_value());
        underwater_place_camera(f, kLevelCam, 0.0f);
    }
    // One frame: the water `w` (null: none) with the look `uw` at `time`, the pillars unless `pillars` is false; `more`
    // runs last.
    Shot draw(const WaterSurface* w, const Underwater& uw = {}, f32 time = 2.0f, bool pillars = true,
              const std::function<void(Scene3D&)>& more = {}, bool post_default = false) {
        f.render(0.0f, false, [&](Scene3D& s) {
            underwater_scene(s, w, uw, time, pillars ? kPillars : ~0u, post_default);
            if (more) more(s);
        });
        CHECK(f.device->validation_error_count() == 0);
        return {f.pixels, f.renderer.forward_3d().last_stats(), f.gpu, f.view};
    }
};

}  // namespace

TEST_CASE("underwater: no water, the setting off, strength 0, r.underwater 0 and the camera above the band record nothing") {
    UwFixture u;
    const WaterSurface water = underwater_water();
    // No water (the camera would be 2 m under the level): the setting on or off gives the same frame.
    const Shot dry_off = u.draw(nullptr, look(false));
    const Shot dry_on = u.draw(nullptr, look(true));
    CHECK_FALSE(dry_on.stats.underwater);
    CHECK(same(dry_on.px, dry_off.px));
    WaterSurface disabled = water;
    disabled.enabled = false;
    const Shot carrier_off = u.draw(&disabled, look(true));
    CHECK_FALSE(carrier_off.stats.underwater);
    CHECK(same(carrier_off.px, dry_off.px));

    // Water above the camera: the frame without the look is PostFx::underwater.enabled false.
    const Shot plain = u.draw(&water, look(false));
    CHECK_FALSE(plain.stats.underwater);
    CHECK(plain.stats.underwater_submersion == 0.0f);
    Underwater none = look(true);
    none.strength = 0.0f;
    const Shot zero = u.draw(&water, none);
    CHECK_FALSE(zero.stats.underwater);
    CHECK(same(zero.px, plain.px));
    {
        CvarScope scope("r.underwater", "0");
        const Shot cv = u.draw(&water, look(true));
        CHECK_FALSE(cv.stats.underwater);
        CHECK(same(cv.px, plain.px));
        CHECK(cv.gpu.passes == plain.gpu.passes);
        CHECK(cv.gpu.draws == plain.gpu.draws);
    }
    // The camera just above the crossing band (calm water: band 0.15 m) and well above it.
    for (const f32 above : {0.16f, 3.0f}) {
        underwater_place_camera(u.f, {0.0f, kUnderwaterLevel + above, 7.0f}, 0.0f);
        const Shot p = u.draw(&water, look(false));
        const Shot on = u.draw(&water, look(true));
        CHECK_FALSE(on.stats.underwater);
        CHECK(same(on.px, p.px));
        CHECK(on.gpu.passes == p.gpu.passes);
        CHECK(on.gpu.draws == p.gpu.draws);
    }
    // And below it the pass runs: one more pass, the whole frame changed.
    underwater_place_camera(u.f, kLevelCam, 0.0f);
    const Shot under = u.draw(&water, look(true));
    REQUIRE(under.stats.underwater);
    CHECK(under.stats.underwater_submersion == 1.0f);
    CHECK(under.gpu.passes > plain.gpu.passes);
    const u32 changed = changed_px(under.px, plain.px);
    MESSAGE(changed << " of " << kW * kH << " pixels changed under water");
    CHECK(changed > kW * kH * 9 / 10);
}

TEST_CASE("underwater: absorption takes red before blue and more with distance; far things fade to the scatter colour") {
    UwFixture u;
    const WaterSurface water = underwater_water();
    const Shot plain = u.draw(&water, look(false));
    const Shot under = u.draw(&water, look(true));
    REQUIRE(under.stats.underwater);
    f32 ratio_r[3], ratio_b[3];
    Rgb seen[3];
    for (int i = 0; i < 3; ++i) {
        u.f.view = plain.view;
        u.f.pixels = plain.px;
        const Rgb p = u.f.at_world(kUnderwaterPillars[i]);
        u.f.pixels = under.px;
        seen[i] = u.f.at_world(kUnderwaterPillars[i]);
        MESSAGE("pillar " << i << ": plain " << p.r << " " << p.g << " " << p.b << ", under water " << seen[i].r << " "
                          << seen[i].g << " " << seen[i].b);
        REQUIRE(p.r > 40);
        REQUIRE(p.b > 40);
        ratio_r[i] = f32(seen[i].r) / f32(p.r);
        ratio_b[i] = f32(seen[i].b) / f32(p.b);
    }
    // The plain pillars are the same grey (the same face, no shadows, no fog), within a few levels.
    CHECK(ratio_r[0] > ratio_r[1] + 0.05f);
    CHECK(ratio_r[1] > ratio_r[2] + 0.05f);
    for (int i = 0; i < 3; ++i) CHECK(ratio_r[i] < ratio_b[i]);
    CHECK(ratio_b[2] - ratio_r[2] > ratio_b[0] - ratio_r[0]);
    // A ray just below the horizon at the right border meets nothing (the ground slab ends at 6 m): the far cap, the
    // scatter colour itself. The far pillar is nearer to it than the near one.
    const Rgb far = rgb_at(under.px, kW - 3, kH / 2 + 2);
    auto dist = [&](const Rgb& a) { return std::abs(a.r - far.r) + std::abs(a.g - far.g) + std::abs(a.b - far.b); };
    MESSAGE("far water " << far.r << " " << far.g << " " << far.b);
    CHECK(far.b > far.r);
    CHECK(dist(seen[2]) < dist(seen[1]));
    CHECK(dist(seen[1]) < dist(seen[0]));
}

TEST_CASE("underwater: an up-going ray meeting the surface first is the lid, whatever is above the water") {
    UwFixture u;
    const WaterSurface water = underwater_water();
    const auto above = [](Scene3D& s) {
        MeshInstance m;
        m.mesh = kAbove;
        s.add(m);
    };
    // The floating box is in view (without the water; the plane drawn from below hides it in the plain frame); with the
    // look the frame is the same with or without it.
    const Shot dry = u.draw(nullptr, look(false));
    const Shot dry_box = u.draw(nullptr, look(false), 2.0f, true, above);
    CHECK(changed_px(dry.px, dry_box.px) > 50);
    const Shot under = u.draw(&water, look(true));
    const Shot under_box = u.draw(&water, look(true), 2.0f, true, above);
    REQUIRE(under_box.stats.underwater);
    CHECK(same(under.px, under_box.px));
    // An up-going ray that meets a pillar before the surface shows the pillar: the near pillar's upper part (1 m above
    // the camera's height, 3 m away; the surface is 2 m up, so its ray meets it at about 6 m).
    const Shot no_pillars = u.draw(&water, look(true), 2.0f, false);
    u.f.view = under.view;
    const vec3 upper{kUnderwaterPillars[0].x, 2.2f, kUnderwaterPillars[0].z};
    u.f.pixels = under.px;
    const Rgb with = u.f.at_world(upper);
    u.f.pixels = no_pillars.px;
    const Rgb without = u.f.at_world(upper);
    MESSAGE("upper pillar " << with.r << " " << with.g << " " << with.b << ", lid there " << without.r << " "
                            << without.g << " " << without.b);
    CHECK(std::abs(with.r - without.r) + std::abs(with.g - without.g) + std::abs(with.b - without.b) > 30);

    // Looking up, everything in view is the lid: the sky's brightness (what the plain frame shows through the water)
    // does not reach a pixel.
    underwater_place_camera(u.f, kUpCam, -40.0f);
    const auto sky = [](f32 c) {
        return [c](Scene3D& s) { s.set_sky(constant_sky(c, false)); };
    };
    const Shot dim = u.draw(&water, look(true), 2.0f, false, sky(0.2f));
    const Shot bright = u.draw(&water, look(true), 2.0f, false, sky(3.0f));
    const Shot dim_plain = u.draw(&water, look(false), 2.0f, false, sky(0.2f));
    const Shot bright_plain = u.draw(&water, look(false), 2.0f, false, sky(3.0f));
    CHECK(changed_px(dim_plain.px, bright_plain.px) > kW * kH / 2);
    CHECK(same(dim.px, bright.px));
}

TEST_CASE("underwater: the Snell window straight up is brighter than the lid at 30 degrees") {
    UwFixture u;
    const WaterSurface water = underwater_water();
    const vec3 cam{0.0f, 0.5f, 3.0f};
    underwater_place_camera(u.f, cam, -85.0f);
    const Shot up = u.draw(&water, look(true), 2.0f, false);
    underwater_place_camera(u.f, cam, -30.0f);
    const Shot slant = u.draw(&water, look(true), 2.0f, false);
    REQUIRE(up.stats.underwater);
    const Rgb a = rgb_at(up.px, kW / 2, kH / 2), b = rgb_at(slant.px, kW / 2, kH / 2);
    MESSAGE("straight up " << a.r << " " << a.g << " " << a.b << ", 30 degrees up " << b.r << " " << b.g << " " << b.b);
    CHECK(a.luma() > b.luma() + 20);
}

TEST_CASE("underwater: the surface crossing has no pop; its ends are the plain frame and the full look") {
    // From 1.5 m above to 1.5 m below the level in 5 cm steps, with and without the look, over the default swell (0.4 m:
    // band 0.6 m). The plain sweep has a step of its own where the camera passes the drawn surface; the look may add to
    // each step only a small share of the whole look's difference, so a hard switch (a pop) fails.
    UwFixture u;
    WaterSurface water = underwater_water();
    water.amplitude = 0.4f;
    constexpr int kSteps = 61;
    std::vector<Array<u8>> plain, under;
    std::vector<f32> sub;
    for (int k = 0; k < kSteps; ++k) {
        const f32 y = kUnderwaterLevel + 1.5f - 0.05f * f32(k);
        underwater_place_camera(u.f, {0.0f, y, 7.0f}, 0.0f);
        plain.push_back(u.draw(&water, look(false)).px);
        const Shot s = u.draw(&water, look(true));
        under.push_back(s.px);
        sub.push_back(s.stats.underwater_submersion);
    }
    CHECK(sub.front() == 0.0f);
    CHECK(sub.back() == 1.0f);
    for (int k = 1; k < kSteps; ++k) CHECK(sub[usize(k)] >= sub[usize(k - 1)]);
    CHECK(same(under.front(), plain.front()));
    const f64 full = mean_diff(under.back(), plain.back());
    f64 worst_extra = 0.0, worst_plain = 0.0, worst_under = 0.0;
    for (int k = 1; k < kSteps; ++k) {
        const f64 p = mean_diff(plain[usize(k)], plain[usize(k - 1)]);
        const f64 w = mean_diff(under[usize(k)], under[usize(k - 1)]);
        worst_extra = std::max(worst_extra, w - p);
        worst_plain = std::max(worst_plain, p);
        worst_under = std::max(worst_under, w);
    }
    MESSAGE("whole look " << full << " levels; largest step: plain " << worst_plain << ", under water " << worst_under
                          << ", added by the look " << worst_extra);
    CHECK(full > 20.0);
    CHECK(worst_extra < 0.12 * full);
}

TEST_CASE("underwater: the wobble follows the scene time; the 64 s wrap is exact and does not pop") {
    UwFixture u;
    const WaterSurface water = underwater_water();
    Underwater uw = look(true);
    uw.wobble = 2.0f;  // about a pixel at 160 x 120
    const Shot a = u.draw(&water, uw, 3.0f);
    const Shot b = u.draw(&water, uw, 3.0f);
    CHECK(same(a.px, b.px));
    const Shot c = u.draw(&water, uw, 5.0f);
    MESSAGE("3 s vs 5 s: " << changed_px(a.px, c.px) << " pixels differ");
    CHECK(changed_px(a.px, c.px) > 100);
    // Whole cycles per 64 s: 67 s is 3 s again, bit for bit (and the frame without the look does not depend on the clock).
    const Shot wrapped = u.draw(&water, uw, 67.0f);
    CHECK(same(wrapped.px, a.px));
    CHECK(same(u.draw(&water, look(false), 3.0f).px, u.draw(&water, look(false), 67.0f).px));
    // 10 ms across the wrap moves about as much as 10 ms anywhere else.
    const Shot w0 = u.draw(&water, uw, 63.995f);
    const Shot w1 = u.draw(&water, uw, 64.005f);
    const Shot t0 = u.draw(&water, uw, 30.0f);
    const Shot t1 = u.draw(&water, uw, 30.01f);
    const u32 across = moved_px(w0.px, w1.px, 4), step = moved_px(t0.px, t1.px, 4), far = moved_px(t0.px, c.px, 4);
    MESSAGE("moved by more than 4 levels: across the wrap " << across << ", 10 ms " << step << ", far " << far);
    CHECK(across < 4 * step + 40);
    CHECK(far > 4 * std::max(across, step) + 40);
}

TEST_CASE("underwater: with TAA and auto exposure the next frame holds no look") {
    const WaterSurface water = underwater_water();
    auto two_frames = [&](bool look_first) {
        UwFixture u;
        const auto setup = [](Scene3D& s) {
            PostFx post = s.post();
            post.taa = true;
            post.auto_exposure.enabled = true;
            s.set_post(post);
        };
        const Shot first = u.draw(&water, look(look_first), 2.0f, true, setup);
        CHECK(first.stats.taa);
        CHECK(first.stats.underwater == look_first);
        const Shot second = u.draw(&water, look(false), 2.0f, true, setup);
        CHECK(second.stats.taa_history);
        CHECK_FALSE(second.stats.underwater);
        return second.px;
    };
    CHECK(same(two_frames(true), two_frames(false)));
}

TEST_CASE("underwater: the lens rain and the heat haze act on the underwater picture, and bloom after them") {
    // Looking up, every pixel of the full look is the lid, which discards the scene colour: an effect recorded before
    // the pass would leave no trace, one after it changes pixels.
    UwFixture u;
    const WaterSurface water = underwater_water();
    underwater_place_camera(u.f, kUpCam, -40.0f);
    const Shot alone = u.draw(&water, look(true), 2.0f, false);
    REQUIRE(alone.stats.underwater);
    const Shot rain = u.draw(&water, look(true), 2.0f, false, [](Scene3D& s) {
        PostFx post = s.post();
        post.rain = lens_rain_setting();
        s.set_post(post);
    });
    REQUIRE(rain.stats.lens_rain);
    MESSAGE("lens rain: " << changed_px(rain.px, alone.px) << " pixels changed");
    CHECK(changed_px(rain.px, alone.px) > 50);

    const vec3 fwd{0.0f, std::sin(math::radians(40.0f)), -std::cos(math::radians(40.0f))};
    const HeatHaze hz = [&] {
        HeatHaze h;
        h.count = 1;
        h.volumes[0].center = kUpCam + fwd * 6.0f;
        h.volumes[0].radius = 2.0f;
        h.volumes[0].strength = 2.0f;
        return h;
    }();
    const Shot haze = u.draw(&water, look(true), 2.0f, false, [&](Scene3D& s) {
        PostFx post = s.post();
        post.haze = hz;
        s.set_post(post);
    });
    REQUIRE(haze.stats.heat_haze_volumes == 1);
    MESSAGE("heat haze: " << changed_px(haze.px, alone.px) << " pixels changed");
    CHECK(changed_px(haze.px, alone.px) > 20);

    const Shot bloom = u.draw(&water, look(true), 2.0f, false, [](Scene3D& s) {
        PostFx post = s.post();
        post.bloom = true;
        post.bloom_threshold = 0.0f;
        post.bloom_intensity = 0.5f;
        s.set_post(post);
    });
    MESSAGE("bloom: " << changed_px(bloom.px, alone.px) << " pixels changed");
    CHECK(changed_px(bloom.px, alone.px) > kW * kH / 4);
}

TEST_CASE("underwater: Low is a flat tint without the prepass; Medium wobbles one octave, High two") {
    UwFixture u;
    const WaterSurface water = underwater_water();
    Underwater wobbly = look(true);
    wobbly.wobble = 2.0f;
    Underwater still = look(true);
    still.wobble = 0.0f;
    // A flat tint: the output is a function of the input colour alone (within the 8-bit rounding of the HDR input), with
    // no lid (looking level, the upper half would be the lid at Medium) and no depth.
    auto flat = [&](const Shot& plain, const Shot& tinted) {
        std::map<u32, Rgb> seen;
        u32 conflicts = 0;
        for (usize p = 0; p < usize(kW) * kH; ++p) {
            const u32 key = u32(plain.px[p * 4]) | (u32(plain.px[p * 4 + 1]) << 8) | (u32(plain.px[p * 4 + 2]) << 16);
            const Rgb t{tinted.px[p * 4], tinted.px[p * 4 + 1], tinted.px[p * 4 + 2]};
            auto [it, fresh] = seen.emplace(key, t);
            if (!fresh && (std::abs(it->second.r - t.r) > 2 || std::abs(it->second.g - t.g) > 2 ||
                           std::abs(it->second.b - t.b) > 2))
                ++conflicts;
        }
        return conflicts;
    };
    {
        CvarScope scope("r.quality", "low");
        const Shot plain = u.draw(&water, look(false));
        const Shot low = u.draw(&water, wobbly);
        REQUIRE(low.stats.underwater);
        CHECK(low.stats.ssao_draws == 0);  // no prepass (the calm water and the scene ask for none either)
        CHECK(changed_px(low.px, plain.px) > kW * kH / 2);
        CHECK(flat(plain, low) == 0);
    }
    Shot medium, medium_still;
    {
        CvarScope scope("r.quality", "medium");
        const Shot plain = u.draw(&water, look(false));
        medium = u.draw(&water, wobbly);
        medium_still = u.draw(&water, still);
        REQUIRE(medium.stats.underwater);
        CHECK(medium.stats.ssao_draws > 0);  // the pass drew the prepass for its distances
        CHECK(flat(plain, medium) > 100);    // the lid and the distances are not a flat tint
    }
    {
        CvarScope scope("r.quality", "high");
        const Shot high = u.draw(&water, wobbly);
        const Shot high_still = u.draw(&water, still);
        // The second octave: without wobble Medium and High are the same frame, with it they differ.
        CHECK(same(high_still.px, medium_still.px));
        MESSAGE("High vs Medium with wobble: " << changed_px(high.px, medium.px) << " pixels differ");
        CHECK(changed_px(high.px, medium.px) > 50);
        CHECK(changed_px(medium.px, medium_still.px) > 50);
    }
}

TEST_CASE("underwater: without the prepass pipeline the pass draws the flat tint") {
    Forward3DDesc d;
    d.msaa = 4;
    d.ssao = false;
    d.ssr = false;
    d.contact_shadows = false;
    d.edge_outline = false;
    UwFixture u(&d);
    const WaterSurface water = underwater_water();
    CvarScope scope("r.quality", "high");
    const Shot plain = u.draw(&water, look(false));
    const Shot under = u.draw(&water, look(true));
    REQUIRE(under.stats.underwater);
    CHECK(under.stats.ssao_draws == 0);
    CHECK(changed_px(under.px, plain.px) > kW * kH / 2);
    std::map<u32, Rgb> seen;
    u32 conflicts = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        const u32 key = u32(plain.px[p * 4]) | (u32(plain.px[p * 4 + 1]) << 8) | (u32(plain.px[p * 4 + 2]) << 16);
        const Rgb t{under.px[p * 4], under.px[p * 4 + 1], under.px[p * 4 + 2]};
        auto [it, fresh] = seen.emplace(key, t);
        if (!fresh && (std::abs(it->second.r - t.r) > 2 || std::abs(it->second.g - t.g) > 2 || std::abs(it->second.b - t.b) > 2))
            ++conflicts;
    }
    CHECK(conflicts == 0);
}

TEST_CASE("underwater: a render-to-texture view records no look") {
    UwFixture u;
    const WaterSurface water = underwater_water();
    for (const bool linear : {false, true}) {
        u.f.device->begin_frame(u.f.frame++);
        rhi::CommandBuffer cmd;
        Scene3D scene(u.f.renderer.meshes(), mem::heap());
        scene.begin(orbit_camera({0.0f, 1.2f, 0.0f}, 0.0f, 0.0f, 7.0f), {kW, kH});
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        underwater_scene(scene, &water, look(true), 2.0f, kPillars);
        scene.build();
        Forward3D::ViewOutput out;
        out.slot = 1;
        out.linear_output = linear;
        u.f.renderer.record_3d(cmd, scene, u.f.target, out);
        CHECK(u.f.renderer.forward_3d().last_stats().underwater == !linear);
        u.f.device->submit(cmd);
        u.f.device->end_frame();
        CHECK(u.f.device->validation_error_count() == 0);
    }
}

TEST_CASE("underwater: the shader's helper ports equal underwater_window, _lid_distance and _transmittance") {
    // kUnderwaterFS's functions (MCE_GLSL_UNDERWATER) run alone: one row of 8 x 64 pixels, block b = x / 64 evaluating one
    // function at input i = x % 64 (inputs exact in f32 on both sides), each result's f32 bits written as four bytes and
    // read back. The constants come from mce/render/underwater.h, printed to 9 digits (exact). One row, so the backends'
    // row order does not matter.
    Fixture3D f;
    const math::vec3 sigma = underwater_sigma(math::vec3{0.10f, 0.38f, 0.40f}, 8.0f, 1.0f);
    const math::vec3 scatter{0.05f, 0.2f, 0.3f}, colour{0.8f, 0.5f, 0.3f};
    char consts[512];
    std::snprintf(consts, sizeof consts,
                  "const float kCos = %.9g;\nconst float kSoft = %.9g;\nconst float kFar = %.9g;\n"
                  "const vec3 kSigma = vec3(%.9g, %.9g, %.9g);\nconst vec3 kScatter = vec3(%.9g, %.9g, %.9g);\n"
                  "const vec3 kColour = vec3(%.9g, %.9g, %.9g);\n",
                  double(kUnderwaterWindowCos), double(kUnderwaterWindowSoftness), double(kUnderwaterFarCap),
                  double(sigma.x), double(sigma.y), double(sigma.z), double(scatter.x), double(scatter.y),
                  double(scatter.z), double(colour.x), double(colour.y), double(colour.z));
    const std::string fs = std::string("#version 450 core\n") + MCE_GLSL_UNDERWATER + consts + R"(
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    int x = int(gl_FragCoord.x);
    int b = x / 64;
    float i = float(x - b * 64);
    float r = 0.0;
    if (b == 0) {
        r = uw_window(i / 64.0 * 0.25 + 0.5, kCos, kSoft);
    } else if (b == 1) {
        r = uw_lid_distance(i * 0.75 + 0.5, i / 64.0 - 0.25, kFar);
    } else if (b < 5) {
        vec3 t = uw_transmittance(kSigma, i * 0.5, kFar);
        r = b == 2 ? t.r : (b == 3 ? t.g : t.b);
    } else {
        vec3 c = uw_fog(kColour, kSigma, kScatter, i * 0.5, kFar);
        r = b == 5 ? c.r : (b == 6 ? c.g : c.b);
    }
    o_color = unpackUnorm4x8(floatBitsToUint(r));
}
)";
    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = {shaders3d::kFullscreenVS, "fullscreen.vert"};
    pd.fragment_shader = {fs.c_str(), "underwater_helpers_test.frag"};
    pd.state.depth = {};
    pd.debug_name = "test.underwater_helpers";
    auto pipe = f.device->create_pipeline(pd);
    REQUIRE(pipe.has_value());
    constexpr u32 kN = 8 * 64;
    rhi::TextureDesc td;
    td.size = {kN, 1};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.underwater_helpers";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    f.device->begin_frame(f.frame++);
    rhi::CommandBuffer cmd;
    rhi::PassAttachment o;
    o.texture = rt;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&o, 1);
    p.debug_name = "test.underwater_helpers";
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
    f32 worst[4] = {};
    for (u32 x = 0; x < kN; ++x) {
        const u32 b = x / 64;
        const f32 i = f32(x % 64);
        f32 want = 0.0f, tol = 0.0f;
        if (b == 0) {
            want = underwater_window(i / 64.0f * 0.25f + 0.5f, kUnderwaterWindowSoftness);
            tol = 2e-6f;
        } else if (b == 1) {
            want = underwater_lid_distance(i * 0.75f + 0.5f, i / 64.0f - 0.25f);
            tol = 4e-7f * std::max(1.0f, want);
        } else {
            const math::vec3 t = underwater_transmittance(sigma, i * 0.5f);
            if (b < 5) {
                want = b == 2 ? t.x : (b == 3 ? t.y : t.z);
            } else {
                const f32 fog = 1.0f - (t.x + t.y + t.z) * (1.0f / 3.0f);
                want = b == 5 ? colour.x * t.x + scatter.x * fog
                              : (b == 6 ? colour.y * t.y + scatter.y * fog : colour.z * t.z + scatter.z * fog);
            }
            tol = 2e-6f + 1e-5f * want;  // GLSL exp is not correctly rounded: a few ULP
        }
        const u32 bits = u32(px[x * 4]) | (u32(px[x * 4 + 1]) << 8) | (u32(px[x * 4 + 2]) << 16) | (u32(px[x * 4 + 3]) << 24);
        const f32 got = std::bit_cast<f32>(bits);
        const f32 err = std::fabs(got - want);
        const u32 k = b == 0 ? 0u : (b == 1 ? 1u : (b < 5 ? 2u : 3u));
        worst[k] = std::max(worst[k], err);
        INFO("block " << b << ", input " << i << ": got " << got << ", want " << want);
        CHECK(err <= tol);
        match += err <= tol ? 1u : 0u;
    }
    MESSAGE("largest error: window " << worst[0] << ", lid distance " << worst[1] << ", transmittance " << worst[2]
                                     << ", fog " << worst[3]);
    CHECK(match == kN);
    // The table covers the window's edge (0..1 inside the input range), the lid's far cap and the transmittance's tail.
    CHECK(underwater_window(0.5f, kUnderwaterWindowSoftness) == 0.0f);
    CHECK(underwater_window(0.75f - 0.25f / 64.0f, kUnderwaterWindowSoftness) == 1.0f);
    CHECK(underwater_lid_distance(16.0f * 0.75f + 0.5f, 0.0f) == kUnderwaterFarCap);
    f.device->destroy_texture(rt);
    f.device->destroy_pipeline(pipe.value());
}
