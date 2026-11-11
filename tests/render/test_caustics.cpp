// M-CAUSTICS 52b: the pure caustics helpers (mce/render/caustics.h), the CPU twin of kCausticsFS: the integer-wrapped clock,
// the world anchor folded into the period, the water square's centre, the web, the fades, the absorption, the plane point,
// the per-pixel term and the validity gate. No GPU.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/render/caustics.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

CausticsParams params() {
    CausticsParams c;
    c.toward_sun = math::normalize(math::vec3{0.3f, 0.9f, -0.2f});
    c.plane_h = -3.0f;  // the camera 3 m above the water
    c.half_extent = 500.0f;
    c.inv_scale = 0.5f;
    c.phase = 0.25f;
    c.octaves = 3;
    return c;
}

// The term's red channel (the cases below set no water extinction, so the three channels agree but for the Fresnel).
f32 term_r(math::vec3 p, const CausticsParams& c, f32 vol_point = 1.0f, f32 vol_surface = 1.0f) {
    return caustics_term(p, c, vol_point, vol_surface).x;
}

CausticsGate open_gate() {
    CausticsGate g;
    g.enabled = true;
    g.tier = true;
    g.octaves = 3;
    g.cvar = true;
    g.water = true;
    g.render_texture = false;
    g.post_source = true;
    g.perspective = true;
    g.toward_sun = {0.2f, 0.9f, 0.1f};
    g.sun_color = {1.0f, 0.96f, 0.9f};
    g.intensity = 2.0f;
    g.strength = 1.0f;
    g.sun_share = 0.8f;
    return g;
}

}  // namespace

TEST_CASE("caustics: the clock is whole ticks wrapped to the period; speed scales it, 0 holds it") {
    CHECK(caustics_phase(0.0f, 1.0f) == 0.0f);
    CHECK(caustics_phase(1.0f, 1.0f) == 0.25f);  // 1000 of 4000 ticks
    CHECK(caustics_phase(1.5f, 1.0f) == caustics_phase(5.5f, 1.0f));  // one period (4 s) later: the same float
    CHECK(caustics_phase(1.5f, 1.0f) == caustics_phase(4001.5f, 1.0f));
    CHECK(caustics_phase(1.0f, 2.0f) == 0.5f);
    CHECK(caustics_phase(123.456f, 0.0f) == 0.0f);
    CHECK(caustics_phase(kNaN, 1.0f) == 0.0f);
    CHECK(caustics_phase(kInf, 1.0f) == 0.0f);
    CHECK(caustics_phase(1.0f, kNaN) == 0.25f);   // NaN speed: 1
    CHECK(caustics_phase(1.0f, 100.0f) == 0.0f);  // speed clamps to 4: 4000 ticks = one period
    CHECK(caustics_phase(-1.0f, 1.0f) == 0.75f);  // a negative time wraps too
    // A long session: the phase stays a whole tick over the period.
    for (const f32 t : {100000.25f, 3000001.5f, 1.6e7f}) {
        const f32 ph = caustics_phase(t, 1.0f);
        CHECK(ph >= 0.0f);
        CHECK(ph < 1.0f);
        const f32 ticks = ph * f32(kCausticsPeriodTicks);
        CHECK(ticks == std::floor(ticks));
    }
}

TEST_CASE("caustics: the world anchor folds the camera's xz into the 64-cell period, in f64") {
    const math::vec2 a = caustics_offset(10.0, -6.0, 2.0f);
    CHECK(a.x == 5.0f);
    CHECK(a.y == 61.0f);
    // 128 m on a 2 m cell is one period: the same offset.
    const math::vec2 b = caustics_offset(138.0, 122.0, 2.0f);
    CHECK(b.x == a.x);
    CHECK(b.y == a.y);
    // Far from the origin (a long journey on a floating origin): still in range and exact for whole metres.
    const math::vec2 far = caustics_offset(1.0e9 + 10.0, -2.5e8 - 6.0, 2.0f);
    CHECK(far.x == 5.0f);
    CHECK(far.y == 61.0f);
    const math::vec2 n = caustics_offset(std::nan(""), 4.0, kNaN);  // NaN scale: 2 m
    CHECK(n.x == 0.0f);
    CHECK(n.y == 2.0f);
    for (const f64 x : {-1.0e-12, 1.0e-20, 127.99999999999})
        CHECK(caustics_offset(x, 0.0, 2.0f).x < f32(kCausticsCells));
}

TEST_CASE("caustics: the water square's centre snaps to whole cells around the camera as kWaterVS's") {
    // 500 m half size: 62.5 m cells.
    const math::vec2 c = caustics_water_centre(math::vec2{10.0f, -40.0f}, 500.0f);
    CHECK(c.x == -10.0f);
    CHECK(c.y == doctest::Approx(-22.5f));
    const math::vec2 n = caustics_water_centre(math::vec2{kNaN, 1.0f}, 500.0f);
    CHECK(n.x == 0.0f);
    CHECK(n.y == 0.0f);
}

TEST_CASE("caustics: the web is 0..1, a thin bright line network, periodic, moving with the phase") {
    f32 lo = 1.0f, hi = 0.0f, sum = 0.0f;
    u32 bright = 0, n = 0;
    for (int i = 0; i < 64; ++i)
        for (int j = 0; j < 64; ++j) {
            const math::vec2 q{f32(i) * 0.137f + 0.01f, f32(j) * 0.131f + 0.02f};
            const f32 v = caustics_pattern(q, 0.3f, 3);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            sum += v;
            bright += v > 0.5f ? 1 : 0;
            ++n;
            // The period: 64 cells, exactly the same value.
            CHECK(caustics_pattern(math::vec2{q.x + 64.0f, q.y - 128.0f}, 0.3f, 3) ==
                  doctest::Approx(v).epsilon(1e-3));
        }
    MESSAGE("web over 4096 points: min " << lo << ", max " << hi << ", mean " << sum / f32(n) << ", bright share "
                                         << f32(bright) / f32(n));
    CHECK(lo >= 0.0f);
    CHECK(hi <= 1.0f);
    CHECK(hi > 0.8f);
    CHECK(lo < 0.05f);
    CHECK(sum / f32(n) > 0.05f);
    CHECK(sum / f32(n) < 0.5f);
    CHECK(f32(bright) / f32(n) < 0.35f);  // lines, not a wash
    // The phase moves it; a whole period of phase (0 and 1 are the same loop position) does not.
    u32 moved = 0;
    for (int i = 0; i < 200; ++i) {
        const math::vec2 q{f32(i) * 0.31f, f32(i) * 0.17f};
        moved += caustics_pattern(q, 0.0f, 3) != caustics_pattern(q, 0.1f, 3) ? 1 : 0;
        CHECK(caustics_pattern(q, 0.0f, 2) == doctest::Approx(caustics_pattern(q, 1.0f, 2)).epsilon(1e-3));
    }
    CHECK(moved > 100);
    // The octaves differ; each stays 0..1 (the reduced sums renormalized); 0 octaves and non-finite input give 0.
    u32 differ = 0;
    for (int i = 0; i < 200; ++i) {
        const math::vec2 q{f32(i) * 0.29f, 3.0f + f32(i) * 0.07f};
        const f32 v1 = caustics_pattern(q, 0.5f, 1), v2 = caustics_pattern(q, 0.5f, 2), v3 = caustics_pattern(q, 0.5f, 3);
        CHECK(v1 >= 0.0f);
        CHECK(v1 <= 1.0f);
        CHECK(v2 >= 0.0f);
        CHECK(v2 <= 1.0f);
        differ += v2 != v3 ? 1 : 0;
    }
    CHECK(differ > 100);
    CHECK(caustics_pattern(math::vec2{1.0f, 2.0f}, 0.5f, 0) == 0.0f);
    CHECK(caustics_pattern(math::vec2{kNaN, 2.0f}, 0.5f, 3) == 0.0f);
    CHECK(caustics_pattern(math::vec2{kInf, 2.0f}, 0.5f, 3) == 0.0f);
    CHECK(caustics_pattern(math::vec2{1.0f, 2.0f}, kNaN, 3) == 0.0f);
}

TEST_CASE("caustics: the sun fade, the depth falloff, the view path and the absorption") {
    CHECK(caustics_sun_fade(0.0f) == 0.0f);
    CHECK(caustics_sun_fade(kCausticsMinSunY) == 0.0f);
    CHECK(caustics_sun_fade(0.1f) > 0.0f);
    CHECK(caustics_sun_fade(0.1f) < 1.0f);
    CHECK(caustics_sun_fade(kCausticsFullSunY) == 1.0f);
    CHECK(caustics_sun_fade(1.0f) == 1.0f);
    CHECK(caustics_sun_fade(kNaN) == 0.0f);

    CHECK(caustics_depth_fade(0.0f, 30.0f) == 0.0f);
    CHECK(caustics_depth_fade(-1.0f, 30.0f) == 0.0f);
    CHECK(caustics_depth_fade(0.1f, 30.0f) > 0.0f);
    CHECK(caustics_depth_fade(0.1f, 30.0f) < 1.0f);
    CHECK(caustics_depth_fade(1.0f, 30.0f) == 1.0f);
    CHECK(caustics_depth_fade(24.0f, 30.0f) == 1.0f);
    CHECK(caustics_depth_fade(27.0f, 30.0f) > 0.0f);
    CHECK(caustics_depth_fade(27.0f, 30.0f) < 1.0f);
    CHECK(caustics_depth_fade(30.0f, 30.0f) == 0.0f);
    CHECK(caustics_depth_fade(31.0f, 30.0f) == 0.0f);
    CHECK(caustics_depth_fade(kNaN, 30.0f) == 0.0f);
    CHECK(caustics_depth_fade(1.0f, kNaN) == 0.0f);

    // The camera 3 m above the plane, a point 4 m below it straight down: 4 m of the 7 m ray is in water.
    CHECK(caustics_view_path(math::vec3{0.0f, -7.0f, 0.0f}, -3.0f) == doctest::Approx(4.0f));
    // A slanted ray: |p| * depth / -p.y.
    CHECK(caustics_view_path(math::vec3{7.0f, -7.0f, 0.0f}, -3.0f) == doctest::Approx(4.0f * std::sqrt(2.0f)));
    CHECK(caustics_view_path(math::vec3{0.0f, -7.0f, 0.0f}, 2.0f) == 0.0f);   // the camera under water
    CHECK(caustics_view_path(math::vec3{0.0f, -2.0f, 0.0f}, -3.0f) == 0.0f);  // the point above the plane
    CHECK(caustics_view_path(math::vec3{kNaN, -7.0f, 0.0f}, -3.0f) == 0.0f);

    CHECK(caustics_absorption(0.15f, 0.0f, 0.0f) == 1.0f);
    CHECK(caustics_absorption(0.0f, 50.0f, 20.0f) == 1.0f);
    CHECK(caustics_absorption(0.5f, 2.0f, 2.0f) == doctest::Approx(std::exp(-2.0f)));
    CHECK(caustics_absorption(kNaN, 2.0f, 2.0f) == doctest::Approx(std::exp(-0.6f)));  // NaN: 0.15
    CHECK(caustics_absorption(0.15f, kInf, 0.0f) == 0.0f);
    CHECK(caustics_absorption(0.15f, -1.0f, 0.0f) == 0.0f);
}

TEST_CASE("caustics: the plane point follows the sun to the plane inside the water square") {
    CausticsParams c = params();
    c.toward_sun = math::normalize(math::vec3{0.6f, 0.8f, 0.0f});
    math::vec2 xz;
    f32 depth = 0.0f;
    REQUIRE(caustics_plane_point(math::vec3{1.0f, -7.0f, 2.0f}, c, xz, depth));
    CHECK(depth == doctest::Approx(4.0f));
    CHECK(xz.x == doctest::Approx(1.0f + 0.75f * 4.0f));  // 4 m up at a 0.6 / 0.8 slope
    CHECK(xz.y == doctest::Approx(2.0f));
    // At or above the plane: no point.
    CHECK_FALSE(caustics_plane_point(math::vec3{0.0f, -3.0f, 0.0f}, c, xz, depth));
    CHECK_FALSE(caustics_plane_point(math::vec3{0.0f, 1.0f, 0.0f}, c, xz, depth));
    // Outside the water square.
    c.half_extent = 10.0f;
    CHECK(caustics_plane_point(math::vec3{9.0f, -3.5f, 0.0f}, c, xz, depth));
    CHECK_FALSE(caustics_plane_point(math::vec3{11.0f, -3.5f, 0.0f}, c, xz, depth));
    c.centre = {5.0f, 0.0f};
    CHECK(caustics_plane_point(math::vec3{11.0f, -3.5f, 0.0f}, c, xz, depth));
    // A sun under the horizon band, a NaN.
    CausticsParams low = params();
    low.toward_sun = math::vec3{1.0f, 0.01f, 0.0f};
    CHECK_FALSE(caustics_plane_point(math::vec3{0.0f, -7.0f, 0.0f}, low, xz, depth));
    CHECK_FALSE(caustics_plane_point(math::vec3{kNaN, -7.0f, 0.0f}, params(), xz, depth));
    CausticsParams bad = params();
    bad.plane_h = kNaN;
    CHECK_FALSE(caustics_plane_point(math::vec3{0.0f, -7.0f, 0.0f}, bad, xz, depth));
}

TEST_CASE("caustics: the term is 0 above the plane and below max_depth, and the light falls off with depth") {
    const CausticsParams c = params();
    CHECK(term_r(math::vec3{0.0f, -2.0f, -5.0f}, c) == 0.0f);  // above the plane
    CHECK(term_r(math::vec3{0.0f, -40.0f, -5.0f}, c) == 0.0f);  // 37 m under it: past max_depth
    CHECK(term_r(math::vec3{kNaN, -6.0f, -5.0f}, c) == 0.0f);
    CHECK(term_r(math::vec3{0.0f, -kInf, -5.0f}, c) == 0.0f);
    // The mean over a patch of floor: deeper floors get less.
    f32 last = 2.0f;
    for (const f32 depth : {1.0f, 4.0f, 10.0f, 20.0f}) {
        f32 sum = 0.0f;
        for (int i = 0; i < 40; ++i)
            for (int j = 0; j < 40; ++j)
                sum += term_r(math::vec3{f32(i) * 0.23f - 4.0f, c.plane_h - depth, f32(j) * 0.21f - 9.0f}, c);
        const f32 mean = sum / 1600.0f;
        MESSAGE("depth " << depth << " m: mean term " << mean);
        CHECK(mean < last);
        CHECK(mean > 0.0f);
        last = mean;
    }
    // Every term is 0..1; the fog takes its share; more absorption takes more.
    CausticsParams fog = c, murky = c;
    fog.fog_density = 0.05f;
    murky.absorption = 1.0f;
    for (int i = 0; i < 100; ++i) {
        const math::vec3 p{f32(i) * 0.17f - 8.0f, -6.0f, f32(i) * -0.13f - 2.0f};
        const f32 t = term_r(p, c);
        CHECK(t >= 0.0f);
        CHECK(t <= 1.0f);
        CHECK(term_r(p, fog) <= t);
        CHECK(term_r(p, murky) <= t);
    }
    // The camera under water: no view path, so the same floor point gets at least as much.
    CausticsParams under = c;
    under.plane_h = 2.0f;
    const math::vec3 p{1.3f, -1.5f, -4.1f};  // 3.5 m below the plane in both frames' terms
    CausticsParams above = c;
    above.plane_h = -3.0f;
    const math::vec3 pa{1.3f, -6.5f, -4.1f};
    CHECK(term_r(p, under) >= term_r(pa, above));
}

TEST_CASE("caustics: the gate opens only with everything in place") {
    math::vec3 sun, color;
    REQUIRE(caustics_gate(open_gate(), sun, color));
    CHECK(math::length(sun) == doctest::Approx(1.0f));
    CHECK(color.x == doctest::Approx(1.0f * 2.0f * 1.0f * 0.8f));  // colour * intensity * strength * share * fade 1
    CHECK(color.z == doctest::Approx(0.9f * 2.0f * 1.0f * 0.8f));
    const auto closed = [&](void (*edit)(CausticsGate&)) {
        CausticsGate g = open_gate();
        edit(g);
        math::vec3 s{7.0f, 7.0f, 7.0f}, c{7.0f, 7.0f, 7.0f};
        const bool open = caustics_gate(g, s, c);
        CHECK(s.x == 7.0f);  // nothing written when closed
        return !open;
    };
    CHECK(closed([](CausticsGate& g) { g.enabled = false; }));
    CHECK(closed([](CausticsGate& g) { g.tier = false; }));            // Low
    CHECK(closed([](CausticsGate& g) { g.octaves = 0; }));
    CHECK(closed([](CausticsGate& g) { g.cvar = false; }));            // r.caustics 0
    CHECK(closed([](CausticsGate& g) { g.water = false; }));           // no WaterSurface
    CHECK(closed([](CausticsGate& g) { g.see_through = false; }));     // opaque water seen from above
    CHECK(closed([](CausticsGate& g) { g.render_texture = true; }));
    CHECK(closed([](CausticsGate& g) { g.post_source = false; }));
    CHECK(closed([](CausticsGate& g) { g.perspective = false; }));     // ortho
    CHECK(closed([](CausticsGate& g) { g.toward_sun = {0.0f, -0.5f, 1.0f}; }));  // under the horizon
    CHECK(closed([](CausticsGate& g) { g.toward_sun = {1.0f, 0.015f, 0.0f}; }));  // under the fade band
    CHECK(closed([](CausticsGate& g) { g.toward_sun = {0.0f, 0.0f, 0.0f}; }));
    CHECK(closed([](CausticsGate& g) { g.toward_sun = {kNaN, 1.0f, 0.0f}; }));
    CHECK(closed([](CausticsGate& g) { g.toward_sun = {kInf, 1.0f, 0.0f}; }));
    CHECK(closed([](CausticsGate& g) { g.strength = 0.0f; }));
    CHECK(closed([](CausticsGate& g) { g.sun_share = 0.0f; }));
    CHECK(closed([](CausticsGate& g) { g.intensity = 0.0f; }));
    CHECK(closed([](CausticsGate& g) { g.intensity = kNaN; }));
    CHECK(closed([](CausticsGate& g) { g.sun_color = {0.0f, 0.0f, 0.0f}; }));
    CHECK(closed([](CausticsGate& g) { g.sun_color = {kNaN, 1.0f, 1.0f}; }));
    CHECK(closed([](CausticsGate& g) { g.intensity = 3.0e38f; g.strength = 4.0f; }));  // overflow
    // A low sun inside the fade band draws, dimmer.
    CausticsGate low = open_gate();
    low.toward_sun = {1.0f, 0.08f, 0.0f};
    math::vec3 s2, c2;
    REQUIRE(caustics_gate(low, s2, c2));
    CHECK(c2.x < color.x);
    CHECK(c2.x > 0.0f);
}

TEST_CASE("caustics: seen from above, the light passes the water as kWaterFS lets the floor through") {
    // The water's extinction: -log(clamp(shallow, 0.02, 1)) / max(depth_scale, 0.5).
    const math::vec3 sg = caustics_water_sigma(math::vec3{0.10f, 0.38f, 1.0f}, 8.0f);
    CHECK(sg.x == doctest::Approx(-std::log(0.10f) / 8.0f));
    CHECK(sg.y == doctest::Approx(-std::log(0.38f) / 8.0f));
    CHECK(sg.z == 0.0f);
    const math::vec3 sn = caustics_water_sigma(math::vec3{kNaN, 0.0f, 2.0f}, 0.1f);  // NaN, 0 -> 0.02; 2 -> 1; scale 0.5
    CHECK(sn.x == doctest::Approx(-std::log(0.02f) / 0.5f));
    CHECK(sn.y == doctest::Approx(-std::log(0.02f) / 0.5f));
    CHECK(sn.z == 0.0f);
    CHECK(caustics_water_sigma(math::vec3{0.5f, 0.5f, 0.5f}, kNaN).x == doctest::Approx(-std::log(0.5f) / 8.0f));

    CausticsParams c = params();  // the camera 3 m above the water
    c.water_sigma = math::vec3{0.3f, 0.1f, 0.05f};
    c.reflectivity = 1.0f;
    f32 fog_d = 0.0f;
    // Straight down onto a floor 4 m under the surface: F = 0.02, 4 m of water, fogged at the surface 3 m away.
    const math::vec3 down = caustics_transmission(math::vec3{0.0f, -7.0f, 0.0f}, c, fog_d);
    CHECK(down.x == doctest::Approx(0.98f * std::exp(-1.2f)));
    CHECK(down.y == doctest::Approx(0.98f * std::exp(-0.4f)));
    CHECK(down.z == doctest::Approx(0.98f * std::exp(-0.2f)));
    CHECK(fog_d == doctest::Approx(3.0f));
    // A grazing view: the Fresnel takes most of it.
    const math::vec3 graze = caustics_transmission(math::vec3{0.0f, -3.5f, -60.0f}, c, fog_d);
    CHECK(graze.y < 0.5f * down.y);
    // The camera under the water, or a view crossing outside the water square: nothing in the way.
    CausticsParams under = c;
    under.plane_h = 2.0f;
    const math::vec3 u = caustics_transmission(math::vec3{0.0f, -7.0f, 0.0f}, under, fog_d);
    CHECK(u.x == 1.0f);
    CHECK(fog_d == doctest::Approx(7.0f));
    CausticsParams small = c;
    small.half_extent = 1.0f;
    CHECK(caustics_transmission(math::vec3{30.0f, -7.0f, 0.0f}, small, fog_d).x == 1.0f);  // crosses at x 12.9
    CHECK(caustics_transmission(math::vec3{2.0f, -7.0f, 0.0f}, small, fog_d).x < 1.0f);   // crosses at x 0.86
    // The term carries it per channel: the red, most absorbed, least.
    u32 compared = 0;
    for (int i = 0; i < 200; ++i) {
        const math::vec3 p{f32(i) * 0.09f - 9.0f, -6.5f, -2.0f - f32(i) * 0.03f};
        const math::vec3 t = caustics_term(p, c);
        if (t.z <= 0.0f) continue;
        CHECK(t.x < t.y);
        CHECK(t.y < t.z);
        ++compared;
    }
    CHECK(compared > 20);
    // A reflectivity of 0: no Fresnel loss.
    CausticsParams matte = c;
    matte.reflectivity = 0.0f;
    CHECK(caustics_transmission(math::vec3{0.0f, -7.0f, 0.0f}, matte, fog_d).z == doctest::Approx(std::exp(-0.2f)));
}

TEST_CASE("caustics: through the water the fog is counted twice, the volume's transmittance too") {
    // kMeshFS fogs the floor at its own distance into the copy kWaterFS shows; kWaterFS fogs again at the surface's.
    CausticsParams c = params();  // the camera 3 m above the water
    c.water_sigma = math::vec3{0.1f, 0.1f, 0.1f};
    CausticsParams fog = c;
    fog.fog_density = 0.01f;
    f32 share = 0.0f;
    u32 compared = 0;
    for (int i = 0; i < 300; ++i) {
        // Floors 100 m out, about 2 m under the water: the surface crossing about 60 m out.
        const math::vec3 p{f32(i) * 0.05f - 7.5f, -5.0f, -100.0f - f32(i) * 0.01f};
        REQUIRE(caustics_through_water(p, c, share));
        CHECK(share == doctest::Approx(0.6f));
        const f32 bare = term_r(p, c);
        if (bare <= 0.0f) continue;
        const f32 len = math::length(p);
        f32 surface = 0.0f;
        caustics_transmission(p, c, surface);
        CHECK(surface == doctest::Approx(len * share).epsilon(1e-4));
        const f32 fs = 0.01f * surface, fl = 0.01f * len;
        const f32 want = bare * std::exp(-fs * fs) * std::exp(-fl * fl);
        CHECK(term_r(p, fog) == doctest::Approx(want).epsilon(1e-4));
        CHECK(term_r(p, fog) < 0.5f * bare * std::exp(-fs * fs));  // the surface's fog alone would leave twice as much
        // The volumetric fog: the point's transmittance, and through the water the surface's too.
        CHECK(term_r(p, c, 0.5f, 0.4f) == doctest::Approx(bare * 0.2f).epsilon(1e-4));
        ++compared;
    }
    CHECK(compared > 20);
    // The camera under the water: one fog (the main pass's at the point), one volume read.
    CausticsParams under = fog;
    under.plane_h = 2.0f;
    CausticsParams under_clear = under;
    under_clear.fog_density = 0.0f;
    math::vec3 p{0.0f, -1.5f, -40.0f};
    for (int i = 0; i < 400 && !(term_r(p, under_clear) > 0.05f); ++i) p.x = f32(i) * 0.037f;  // a point on the web
    REQUIRE_FALSE(caustics_through_water(p, under, share));
    CHECK(share == 1.0f);
    const f32 bare = term_r(p, under_clear);
    REQUIRE(bare > 0.05f);
    const f32 fl = 0.01f * math::length(p);
    CHECK(term_r(p, under) == doctest::Approx(bare * std::exp(-fl * fl)).epsilon(1e-4));
    CHECK(term_r(p, under_clear, 0.5f, 0.1f) == doctest::Approx(bare * 0.5f).epsilon(1e-4));
}
