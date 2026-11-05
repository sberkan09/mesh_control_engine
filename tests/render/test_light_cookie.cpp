// M-LIGHTCOOKIE 40b: the pure light cookie helpers (the spot projection matrix, the edge fade, the sun uv, the mip, the layer
// plan and the GPU record) and how the quality tiers, `r.light_cookies` and the range clamps map onto PointLight and
// SunCookie. No GPU.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <limits>

#include "mce/convar.h"
#include "mce/render/clusters.h"
#include "mce/render/light_cookie.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr f32 kPi = 3.14159265359f;

ConVar& cookie_cvar() {
    ConVar* cv = ConVar::find(StringId("r.light_cookies"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCookieCvar {
    ~ResetCookieCvar() { cookie_cvar().reset(); }
};

// The cookie uv of a world point through a spot matrix; `inside` is the shader's test.
struct Projected {
    math::vec2 uv;
    f32 w = 0.0f;
    bool inside = false;
};

Projected project(const math::mat4& m, math::vec3 p) {
    const math::vec4 c = m * math::vec4(p, 1.0f);
    Projected r;
    r.w = c.w;
    r.uv = {c.x / c.w * 0.5f + 0.5f, c.y / c.w * 0.5f + 0.5f};
    r.inside = c.w > 0.0f && r.uv.x >= 0.0f && r.uv.x <= 1.0f && r.uv.y >= 0.0f && r.uv.y <= 1.0f;
    return r;
}

// A spot at (1, 2, 3) looking down -Z: the camera right is +X and up is +Y.
const math::vec3 kPos{1.0f, 2.0f, 3.0f};
const math::vec3 kForward{0.0f, 0.0f, -1.0f};
constexpr f32 kOuter = 0.5f;

PointLight spot(u32 key, f32 strength = 1.0f) {
    PointLight l;
    l.spot_outer = 0.6f;
    l.cookie = key;
    l.cookie_strength = strength;
    return l;
}

}  // namespace

TEST_CASE("light cookie: a point on the cone axis maps to the uv centre") {
    const math::mat4 m = spot_cookie_matrix(kPos, kForward, kOuter, 0.0f, 20.0f);
    for (f32 d : {0.5f, 5.0f, 19.0f}) {
        const Projected p = project(m, kPos + kForward * d);
        INFO("distance " << d);
        CHECK(p.inside);
        CHECK(p.uv.x == doctest::Approx(0.5f).epsilon(1e-5));
        CHECK(p.uv.y == doctest::Approx(0.5f).epsilon(1e-5));
    }
}

TEST_CASE("light cookie: the cone's circle is inscribed in the frustum") {
    const math::mat4 m = spot_cookie_matrix(kPos, kForward, kOuter, 0.0f, 20.0f);
    const f32 reach = 5.0f * std::tan(kOuter);  // the cone's radius 5 m along the axis
    const math::vec3 axis = kPos + kForward * 5.0f;
    const Projected right = project(m, axis + math::vec3{reach, 0.0f, 0.0f});
    CHECK(right.uv.x == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(right.uv.y == doctest::Approx(0.5f).epsilon(1e-5));
    const Projected left = project(m, axis + math::vec3{-reach, 0.0f, 0.0f});
    CHECK(left.uv.x == doctest::Approx(0.0f).epsilon(1e-5));
    const Projected up = project(m, axis + math::vec3{0.0f, reach, 0.0f});
    CHECK(up.uv.x == doctest::Approx(0.5f).epsilon(1e-5));
    CHECK(up.uv.y == doctest::Approx(1.0f).epsilon(1e-5));
    const Projected down = project(m, axis + math::vec3{0.0f, -reach, 0.0f});
    CHECK(down.uv.y == doctest::Approx(0.0f).epsilon(1e-5));
    // A point on the circle's diagonal is inside the square (the corner is outside the circle, so a lit pixel always has a texel).
    const Projected diag = project(m, axis + math::vec3{reach * 0.7f, reach * 0.7f, 0.0f});
    CHECK(diag.inside);
    // Past the border: outside.
    CHECK_FALSE(project(m, axis + math::vec3{reach * 1.2f, 0.0f, 0.0f}).inside);
}

TEST_CASE("light cookie: a point behind the light has w <= 0 and is outside") {
    const math::mat4 m = spot_cookie_matrix(kPos, kForward, kOuter, 0.0f, 20.0f);
    const Projected p = project(m, kPos - kForward * 3.0f);
    CHECK(p.w <= 0.0f);
    CHECK_FALSE(p.inside);
}

TEST_CASE("light cookie: the roll turns the projected pattern by exactly the angle") {
    const math::mat4 m0 = spot_cookie_matrix(kPos, kForward, kOuter, 0.0f, 20.0f);
    const math::vec3 world = kPos + kForward * 4.0f + math::vec3{0.6f, 0.2f, 0.0f};
    const Projected base = project(m0, world);
    const f32 a = base.uv.x - 0.5f, b = base.uv.y - 0.5f;
    for (f32 roll : {0.0f, 0.7f, kPi * 0.5f, -1.3f, kPi}) {
        INFO("roll " << roll);
        const Projected p = project(spot_cookie_matrix(kPos, kForward, kOuter, roll, 20.0f), world);
        // The pattern turns counter-clockwise, so a fixed world point reads the texture rotated by -roll.
        CHECK(p.uv.x - 0.5f == doctest::Approx(std::cos(roll) * a + std::sin(roll) * b).epsilon(1e-4).scale(1.0));
        CHECK(p.uv.y - 0.5f == doctest::Approx(-std::sin(roll) * a + std::cos(roll) * b).epsilon(1e-4).scale(1.0));
    }
    // The cone axis stays at the centre whatever the roll.
    const Projected axis = project(spot_cookie_matrix(kPos, kForward, kOuter, 1.1f, 20.0f), kPos + kForward * 3.0f);
    CHECK(axis.uv.x == doctest::Approx(0.5f).epsilon(1e-5));
    CHECK(axis.uv.y == doctest::Approx(0.5f).epsilon(1e-5));
}

TEST_CASE("light cookie: a cone within a degree of vertical takes +Z as up and stays finite") {
    const math::vec3 down{0.0f, -1.0f, 0.0f};
    const math::mat4 m = spot_cookie_matrix(kPos, down, kOuter, 0.0f, 20.0f);
    const math::vec3 axis = kPos + down * 5.0f;
    const f32 reach = 5.0f * std::tan(kOuter);
    CHECK(project(m, axis).uv.x == doctest::Approx(0.5f).epsilon(1e-5));
    const Projected up = project(m, axis + math::vec3{0.0f, 0.0f, reach});
    CHECK(up.uv.x == doctest::Approx(0.5f).epsilon(1e-5));
    CHECK(up.uv.y == doctest::Approx(1.0f).epsilon(1e-5));
    const Projected right = project(m, axis + math::vec3{-reach, 0.0f, 0.0f});  // right = cross(dir, +Z) = -X
    CHECK(right.uv.x == doctest::Approx(1.0f).epsilon(1e-5));
    // A tilt of about 0.5 degrees is still the fallback; one of 5 degrees is not (up is +Y there).
    const math::vec3 slight = math::normalize(math::vec3{0.0087f, -1.0f, 0.0f});
    const math::vec3 tilted = math::normalize(math::vec3{0.0873f, -1.0f, 0.0f});
    const Projected a = project(spot_cookie_matrix(kPos, slight, kOuter, 0.0f, 20.0f), kPos + slight * 5.0f + math::vec3{0, 0, reach});
    CHECK(a.uv.y == doctest::Approx(1.0f).epsilon(1e-3));
    const Projected b = project(spot_cookie_matrix(kPos, tilted, kOuter, 0.0f, 20.0f), kPos + tilted * 5.0f + math::vec3{0, 0, reach});
    CHECK(b.uv.y == doctest::Approx(0.5f).epsilon(1e-3));  // +Z is sideways there
}

TEST_CASE("light cookie: the matrix is deterministic and sanitizes its inputs") {
    const math::mat4 a = spot_cookie_matrix(kPos, kForward, kOuter, 0.3f, 12.0f);
    const math::mat4 b = spot_cookie_matrix(kPos, kForward, kOuter, 0.3f, 12.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(a.c[c][r] == b.c[c][r]);
    // A zero or NaN direction looks straight down; a NaN roll counts as 0; a NaN or tiny radius and cone stay usable.
    const math::mat4 down = spot_cookie_matrix(kPos, {0.0f, -1.0f, 0.0f}, kOuter, 0.0f, 20.0f);
    const math::mat4 zero = spot_cookie_matrix(kPos, {0.0f, 0.0f, 0.0f}, kOuter, kNaN, 20.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(down.c[c][r] == zero.c[c][r]);
    const math::mat4 odd = spot_cookie_matrix(kPos, kForward, kNaN, kInf, kNaN);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(std::isfinite(odd.c[c][r]));
}

TEST_CASE("light cookie: spot_cookie_edge is 1 when off, 0 at the border and a smoothstep between") {
    CHECK(spot_cookie_edge({0.0f, 0.0f}, 0.0f) == 1.0f);
    CHECK(spot_cookie_edge({0.5f, 0.5f}, 0.0f) == 1.0f);
    CHECK(spot_cookie_edge({0.3f, 0.7f}, kNaN) == 1.0f);
    CHECK(spot_cookie_edge({0.0f, 0.5f}, 0.5f) == 0.0f);
    CHECK(spot_cookie_edge({0.5f, 1.0f}, 0.5f) == 0.0f);
    CHECK(spot_cookie_edge({-0.2f, 0.5f}, 0.5f) == 0.0f);
    CHECK(spot_cookie_edge({0.5f, 0.5f}, 0.5f) == 1.0f);
    CHECK(spot_cookie_edge({0.25f, 0.5f}, 0.5f) == 1.0f);  // the fade band is edge / 2 = 0.25 wide
    CHECK(spot_cookie_edge({0.125f, 0.5f}, 0.5f) == doctest::Approx(0.5f));
    CHECK(spot_cookie_edge({0.5f, 0.875f}, 0.5f) == doctest::Approx(0.5f));
    CHECK(spot_cookie_edge({0.0625f, 0.5f}, 0.5f) == doctest::Approx(0.15625f));  // t = 0.25: 3t^2 - 2t^3
    CHECK(spot_cookie_edge({0.5f, 0.5f}, 4.0f) == 1.0f);  // edge clamps to 1: the band is 0.5 wide
    CHECK(spot_cookie_edge({kNaN, 0.5f}, 0.5f) == 0.0f);
}

TEST_CASE("light cookie: sun_cookie_uv has the period size and shifts by offset / size") {
    SunCookie c;
    c.size = 20.0f;
    const math::vec3 down{0.0f, -1.0f, 0.0f};  // the ground point is (x, z)
    const math::vec3 p{3.0f, 7.0f, -4.0f};
    const math::vec2 uv = sun_cookie_uv(p, down, c);
    CHECK(uv.x == doctest::Approx(3.0f / 20.0f));
    CHECK(uv.y == doctest::Approx(-4.0f / 20.0f));
    const math::vec2 sx = sun_cookie_uv(p + math::vec3{20.0f, 0.0f, 0.0f}, down, c);
    CHECK(sx.x - uv.x == doctest::Approx(1.0f));
    CHECK(sx.y == doctest::Approx(uv.y));
    const math::vec2 sz = sun_cookie_uv(p + math::vec3{0.0f, 0.0f, 20.0f}, down, c);
    CHECK(sz.y - uv.y == doctest::Approx(1.0f));
    // The height along the sun's direction does not matter.
    const math::vec2 sy = sun_cookie_uv(p + math::vec3{0.0f, 50.0f, 0.0f}, down, c);
    CHECK(sy.x == doctest::Approx(uv.x));
    CHECK(sy.y == doctest::Approx(uv.y));
    // The offset shifts by offset / size; a shift by one full size changes nothing modulo 1.
    c.offset = {5.0f, -10.0f};
    const math::vec2 so = sun_cookie_uv(p, down, c);
    CHECK(so.x - uv.x == doctest::Approx(0.25f));
    CHECK(so.y - uv.y == doctest::Approx(-0.5f));
    c.offset = {20.0f, 40.0f};
    const math::vec2 sw = sun_cookie_uv(p, down, c);
    CHECK(sw.x - uv.x == doctest::Approx(1.0f));
    CHECK(sw.y - uv.y == doctest::Approx(2.0f));
    // The size scales the tiling.
    c.offset = {0.0f, 0.0f};
    c.size = 40.0f;
    CHECK(sun_cookie_uv(p, down, c).x == doctest::Approx(3.0f / 40.0f));
}

TEST_CASE("light cookie: the sun roll turns the pattern counter-clockwise in the plane") {
    SunCookie c;
    c.size = 20.0f;
    const math::vec3 down{0.0f, -1.0f, 0.0f};
    const math::vec3 p{10.0f, 0.0f, 0.0f};
    c.roll = kPi * 0.5f;
    const math::vec2 q = sun_cookie_uv(p, down, c);
    CHECK(q.x == doctest::Approx(0.0f).scale(1.0).epsilon(1e-5));
    CHECK(q.y == doctest::Approx(-0.5f));
    c.roll = 0.7f;
    const math::vec2 r = sun_cookie_uv(p, down, c);
    CHECK(r.x == doctest::Approx(std::cos(0.7f) * 0.5f));
    CHECK(r.y == doctest::Approx(-std::sin(0.7f) * 0.5f));
}

TEST_CASE("light cookie: the sun pattern is fixed on the ground, carried along the sun, and sanitizes its inputs") {
    SunCookie c;
    const math::vec3 world{12.0f, 3.0f, -8.0f};
    const math::vec3 d = math::normalize(math::vec3{0.3f, -0.9f, 0.1f});
    const math::vec3 d2 = math::normalize(math::vec3{0.3001f, -0.9f, 0.1f});
    const math::vec2 a = sun_cookie_uv(world, d, c), b = sun_cookie_uv(world, d2, c);
    CHECK(std::fabs(a.x - b.x) < 1e-3f);
    CHECK(std::fabs(a.y - b.y) < 1e-3f);
    // Near the zenith there is no basis switch: a tiny move across vertical does not snap.
    const math::vec2 v1 = sun_cookie_uv(world, {0.0f, -1.0f, 0.0f}, c), v2 = sun_cookie_uv(world, {0.001f, -1.0f, 0.0f}, c);
    const math::vec2 v3 = sun_cookie_uv(world, {0.0f, -1.0f, 0.02f}, c), v4 = sun_cookie_uv(world, {0.0f, -1.0f, 0.0175f}, c);
    CHECK(std::fabs(v1.x - v2.x) < 1e-3f);
    CHECK(std::fabs(v1.y - v2.y) < 1e-3f);
    CHECK(std::fabs(v3.x - v4.x) < 1e-3f);
    CHECK(std::fabs(v3.y - v4.y) < 1e-3f);
    // On the ground (y = 0) the pattern does not depend on the sun at all (no swim with the azimuth or the height).
    const math::vec3 ground{12.0f, 0.0f, -8.0f};
    const math::vec2 g0 = sun_cookie_uv(ground, {0.0f, -1.0f, 0.0f}, c);
    for (math::vec3 sd : {math::vec3{0.3f, -0.9f, 0.1f}, math::vec3{-0.7f, -0.4f, 0.6f}, math::vec3{0.0f, -0.2f, -1.0f}}) {
        const math::vec2 g = sun_cookie_uv(ground, sd, c);
        CHECK(g.x == doctest::Approx(g0.x));
        CHECK(g.y == doctest::Approx(g0.y));
    }
    // Above the ground a point takes the pattern of the ground point the sun's ray through it reaches.
    const math::vec3 hit = world + d * (world.y / -d.y);
    CHECK(std::fabs(hit.y) < 1e-4f);
    const math::vec2 h = sun_cookie_uv(hit, d, c);
    CHECK(a.x == doctest::Approx(h.x));
    CHECK(a.y == doctest::Approx(h.y));
    // A grazing (or horizontal) sun is floored at kSunCookieMinDy: finite, and every sun below the floor projects alike.
    const math::vec2 flat = sun_cookie_uv(world, {1.0f, 0.0f, 0.0f}, c);
    const math::vec2 low = sun_cookie_uv(world, {1.0f, -0.01f, 0.0f}, c);
    CHECK(std::isfinite(flat.x));
    CHECK(std::isfinite(flat.y));
    CHECK(flat.x == doctest::Approx((12.0f + 3.0f / kSunCookieMinDy) / 20.0f));
    CHECK(std::fabs(flat.x - low.x) < 1e-3f);
    CHECK(std::fabs(flat.y - low.y) < 1e-3f);
    CHECK(sun_cookie_dy(0.0f) == -kSunCookieMinDy);
    CHECK(sun_cookie_dy(-0.5f) == -0.5f);
    CHECK(sun_cookie_dy(0.01f) == kSunCookieMinDy);
    // A NaN size is the default 20, a zero or NaN direction looks straight down, non-finite roll and offset count as 0.
    SunCookie bad;
    bad.size = kNaN;
    bad.roll = kInf;
    bad.offset = {kNaN, kInf};
    const math::vec2 e = sun_cookie_uv(world, {0.0f, 0.0f, 0.0f}, bad);
    const math::vec2 f = sun_cookie_uv(world, {0.0f, -1.0f, 0.0f}, SunCookie{});
    CHECK(e.x == f.x);
    CHECK(e.y == f.y);
}

TEST_CASE("light cookie: cookie_lod is 0 below a texel per pixel, +1 per doubling, clamped to the chain") {
    const f32 outer = kPi * 0.25f;  // tan = 1
    const f32 texel = 2.0f * 10.0f * std::tan(outer) / 256.0f;
    CHECK(cookie_lod(texel * 0.25f, 10.0f, outer, 256) == 0.0f);
    CHECK(cookie_lod(texel, 10.0f, outer, 256) == doctest::Approx(0.0f).scale(1.0));
    CHECK(cookie_lod(texel * 2.0f, 10.0f, outer, 256) == doctest::Approx(1.0f));
    CHECK(cookie_lod(texel * 8.0f, 10.0f, outer, 256) == doctest::Approx(3.0f));
    CHECK(cookie_lod(texel * 1000.0f, 10.0f, outer, 256) == doctest::Approx(8.0f));  // log2(256)
    CHECK(cookie_lod(texel * 1000.0f, 10.0f, outer, 128) == doctest::Approx(7.0f));
    // A closer light has bigger texels: the same footprint reads a finer mip.
    CHECK(cookie_lod(texel * 4.0f, 5.0f, outer, 256) == doctest::Approx(3.0f));
    // Degenerate input gives 0.
    CHECK(cookie_lod(kNaN, 10.0f, outer, 256) == 0.0f);
    CHECK(cookie_lod(1.0f, 0.0f, outer, 256) == 0.0f);
    CHECK(cookie_lod(1.0f, kNaN, outer, 256) == 0.0f);
    CHECK(cookie_lod(-1.0f, 10.0f, outer, 256) == 0.0f);
    CHECK(cookie_lod(1.0f, 10.0f, outer, 0) == 0.0f);
}

TEST_CASE("light cookie: cookie_lod_sun follows the tiling") {
    const f32 texel = 20.0f / 256.0f;
    CHECK(cookie_lod_sun(texel * 0.5f, 20.0f, 256) == 0.0f);
    CHECK(cookie_lod_sun(texel * 4.0f, 20.0f, 256) == doctest::Approx(2.0f));
    CHECK(cookie_lod_sun(1000.0f, 20.0f, 256) == doctest::Approx(8.0f));
    CHECK(cookie_lod_sun(1.0f, 20.0f, 0) == 0.0f);
    CHECK(cookie_lod_sun(kInf, 20.0f, 256) == 0.0f);
    CHECK(cookie_lod_sun(1.0f, 0.0f, 256) == 0.0f);
}

TEST_CASE("light cookie plan: equal keys share a layer, in first-seen order") {
    const PointLight lights[] = {spot(7), spot(9), spot(7), spot(5)};
    u32 rec[4] = {};
    const CookiePlan p = plan_cookies(lights, DirectionalLight{}, 16, 16, rec);
    CHECK(p.count == 3);
    CHECK(p.lights == 4);
    CHECK(p.dropped == 0);
    CHECK(p.sun_layer == ~0u);
    CHECK(p.key[0] == 7);
    CHECK(p.key[1] == 9);
    CHECK(p.key[2] == 5);
    for (u32 i = 0; i < 4; ++i) CHECK(rec[i] == i);
    CHECK(p.layer_of[0] == 0);
    CHECK(p.layer_of[1] == 1);
    CHECK(p.layer_of[2] == 0);
    CHECK(p.layer_of[3] == 2);
}

TEST_CASE("light cookie plan: the layer order is stable across frames and does not depend on later lights") {
    const PointLight frame1[] = {spot(3), spot(4)};
    const PointLight frame2[] = {spot(3), spot(4), spot(8)};
    u32 r1[2] = {}, r2[3] = {};
    const CookiePlan a = plan_cookies(frame1, DirectionalLight{}, 16, 16, r1);
    const CookiePlan b = plan_cookies(frame2, DirectionalLight{}, 16, 16, r2);
    CHECK(a.key[0] == b.key[0]);
    CHECK(a.key[1] == b.key[1]);
    CHECK(b.key[2] == 8);
    const CookiePlan again = plan_cookies(frame2, DirectionalLight{}, 16, 16, r2);
    for (u32 i = 0; i < kMaxCookieLayers; ++i) CHECK(again.key[i] == b.key[i]);
    CHECK(again.count == b.count);
}

TEST_CASE("light cookie plan: the sun takes layer 0 first and is not counted against the light cap") {
    DirectionalLight sun;
    sun.cookie.texture = 42;
    const PointLight lights[] = {spot(7), spot(42), spot(9)};
    u32 rec[3] = {};
    const CookiePlan p = plan_cookies(lights, sun, 16, 2, rec);
    CHECK(p.sun_layer == 0);
    CHECK(p.key[0] == 42);
    CHECK(p.lights == 2);  // two cookied lights fit although the sun also has a cookie
    CHECK(p.dropped == 1);
    CHECK(rec[0] == 0);
    CHECK(rec[1] == 1);
    CHECK(rec[2] == ~0u);
    CHECK(p.layer_of[0] == 1);  // 7
    CHECK(p.layer_of[1] == 0);  // 42 shares the sun's layer
    CHECK(p.count == 2);
}

TEST_CASE("light cookie plan: the light and layer caps drop lights and count them") {
    PointLight lights[6];
    for (u32 i = 0; i < 6; ++i) lights[i] = spot(100 + i);
    u32 rec[6] = {};
    const CookiePlan by_lights = plan_cookies(lights, DirectionalLight{}, 16, 4, rec);
    CHECK(by_lights.lights == 4);
    CHECK(by_lights.dropped == 2);
    CHECK(rec[3] == 3);
    CHECK(rec[4] == ~0u);
    CHECK(rec[5] == ~0u);
    const CookiePlan by_layers = plan_cookies(lights, DirectionalLight{}, 3, 16, rec);
    CHECK(by_layers.count == 3);
    CHECK(by_layers.lights == 3);
    CHECK(by_layers.dropped == 3);
    CHECK(rec[2] == 2);
    CHECK(rec[3] == ~0u);
    // A light whose key is already in an atlas layer is not dropped by a full atlas.
    PointLight more[4] = {spot(1), spot(2), spot(3), spot(1)};
    u32 rec4[4] = {};
    const CookiePlan shared = plan_cookies(more, DirectionalLight{}, 3, 16, rec4);
    CHECK(shared.dropped == 0);
    CHECK(shared.lights == 4);
    // The caps clamp to the constants, and a zero cap drops everything.
    const CookiePlan huge = plan_cookies(lights, DirectionalLight{}, 1000, 1000, rec);
    CHECK(huge.lights == 6);
    const CookiePlan none = plan_cookies(lights, DirectionalLight{}, 0, 16, rec);
    CHECK(none.lights == 0);
    CHECK(none.dropped == 6);
    DirectionalLight sun;
    sun.cookie.texture = 1;
    CHECK(plan_cookies(lights, sun, 0, 16, rec).sun_layer == ~0u);
}

TEST_CASE("light cookie plan: more cookied lights than the constant drop beyond kMaxCookieLights") {
    PointLight lights[kMaxCookieLights + 3];
    for (u32 i = 0; i < kMaxCookieLights + 3; ++i) lights[i] = spot(i);
    u32 rec[kMaxCookieLights + 3] = {};
    const CookiePlan p = plan_cookies(lights, DirectionalLight{}, 1000, 1000, rec);
    CHECK(p.lights == kMaxCookieLights);
    CHECK(p.count == kMaxCookieLayers);
    CHECK(p.dropped == 3);
}

TEST_CASE("light cookie plan: only a spot with a key and a strength above 0 takes part") {
    PointLight point = spot(1);
    point.spot_outer = 0.0f;  // a point light ignores its cookie
    PointLight none = spot(2);
    none.cookie = ~0u;
    const PointLight zero = spot(3, 0.0f);  // strength 0 = the plain light: no layer, not a drop
    const PointLight nan = spot(4, kNaN);
    const PointLight lights[] = {point, none, zero, nan, spot(5)};
    u32 rec[5] = {};
    const CookiePlan p = plan_cookies(lights, DirectionalLight{}, 16, 16, rec);
    CHECK(p.lights == 1);
    CHECK(p.count == 1);
    CHECK(p.key[0] == 5);
    CHECK(p.dropped == 0);
    for (u32 i = 0; i < 4; ++i) CHECK(rec[i] == ~0u);
    CHECK(rec[4] == 0);
    // A sun with strength 0 takes no layer.
    DirectionalLight sun;
    sun.cookie.texture = 9;
    sun.cookie.strength = 0.0f;
    CHECK(plan_cookies(lights, sun, 16, 16, rec).sun_layer == ~0u);
    // No cookies at all: an empty plan.
    const CookiePlan empty = plan_cookies({}, DirectionalLight{}, 16, 16, {});
    CHECK(empty.count == 0);
    CHECK(empty.lights == 0);
    CHECK(empty.dropped == 0);
    CHECK(empty.sun_layer == ~0u);
}

TEST_CASE("light cookie: the GPU record layout and values") {
    static_assert(sizeof(GpuCookieRecord) == 80);
    CHECK(offsetof(GpuCookieRecord, view_proj) == 0);
    CHECK(offsetof(GpuCookieRecord, params) == 64);
    PointLight l = spot(3, 0.75f);
    l.position = kPos;
    l.direction = kForward;
    l.spot_outer = kOuter;
    l.radius = 20.0f;
    l.cookie_roll = 0.4f;
    l.cookie_edge = 0.3f;
    const GpuCookieRecord r = cookie_record(l, 5);
    const math::mat4 m = spot_cookie_matrix(kPos, kForward, kOuter, 0.4f, 20.0f);
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) CHECK(r.view_proj.c[c][row] == m.c[c][row]);
    CHECK(r.params.x == 5.0f);
    CHECK(r.params.y == 0.75f);
    CHECK(r.params.z == doctest::Approx(0.3f));
    CHECK(r.params.w == 0.0f);
    // The strength and edge clamp, NaN gives the defaults.
    l.cookie_strength = 3.0f;
    l.cookie_edge = -1.0f;
    const GpuCookieRecord clamped = cookie_record(l, 0);
    CHECK(clamped.params.y == 1.0f);
    CHECK(clamped.params.z == 0.0f);
    l.cookie_strength = kNaN;
    l.cookie_edge = kNaN;
    const GpuCookieRecord nan = cookie_record(l, 0);
    CHECK(nan.params.y == 1.0f);
    CHECK(nan.params.z == 0.0f);
}

TEST_CASE("light cookie: the tiers keep cookies on and bound the cost") {
    const QualitySettings low = quality_settings(QualityTier::Low);
    const QualitySettings medium = quality_settings(QualityTier::Medium);
    const QualitySettings high = quality_settings(QualityTier::High);
    CHECK(low.light_cookies);
    CHECK(medium.light_cookies);
    CHECK(high.light_cookies);
    CHECK(low.light_cookie_size == 128);
    CHECK(medium.light_cookie_size == 256);
    CHECK(high.light_cookie_size == 512);
    CHECK(low.light_cookie_layers == 4);
    CHECK(medium.light_cookie_layers == 8);
    CHECK(high.light_cookie_layers == 16);
    CHECK(low.light_cookie_lights == 4);
    CHECK(medium.light_cookie_lights == 8);
    CHECK(high.light_cookie_lights == 16);
    CHECK(high.light_cookie_layers == kMaxCookieLayers);
    CHECK(high.light_cookie_lights == kMaxCookieLights);
}

TEST_CASE("light cookie: apply_quality honors the tier flag and r.light_cookies") {
    ResetCookieCvar reset;
    PointLight l = spot(11);
    SunCookie sun;
    sun.texture = 12;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        PointLight a = l;
        apply_quality(quality_settings(tier), a);
        CHECK(a.cookie == 11);
        SunCookie s = sun;
        apply_quality(quality_settings(tier), s);
        CHECK(s.texture == 12);
    }
    QualitySettings off = quality_settings(QualityTier::High);
    off.light_cookies = false;
    PointLight a = l;
    apply_quality(off, a);
    CHECK(a.cookie == ~0u);
    SunCookie s = sun;
    apply_quality(off, s);
    CHECK(s.texture == ~0u);

    cookie_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        PointLight b = l;
        apply_quality(quality_settings(tier), b);
        CHECK(b.cookie == ~0u);
        SunCookie t = sun;
        apply_quality(quality_settings(tier), t);
        CHECK(t.texture == ~0u);
    }
    cookie_cvar().set_string("1");
    PointLight c = l;
    apply_quality(quality_settings(QualityTier::High), c);
    CHECK(c.cookie == 11);
}

TEST_CASE("light cookie: apply_quality clamps the ranges and sanitizes NaN") {
    const QualitySettings q = quality_settings(QualityTier::High);
    PointLight l = spot(1);
    l.cookie_strength = 4.0f;
    l.cookie_edge = -2.0f;
    l.cookie_roll = 3.0f * kPi;  // wraps to pi
    apply_quality(q, l);
    CHECK(l.cookie_strength == 1.0f);
    CHECK(l.cookie_edge == 0.0f);
    CHECK(std::fabs(l.cookie_roll) == doctest::Approx(kPi).epsilon(1e-4));
    l.cookie_strength = -1.0f;
    l.cookie_edge = 7.0f;
    l.cookie_roll = -0.5f;
    apply_quality(q, l);
    CHECK(l.cookie_strength == 0.0f);
    CHECK(l.cookie_edge == 1.0f);
    CHECK(l.cookie_roll == doctest::Approx(-0.5f));
    l.cookie_strength = kNaN;
    l.cookie_edge = kNaN;
    l.cookie_roll = kNaN;
    apply_quality(q, l);
    CHECK(l.cookie_strength == 1.0f);
    CHECK(l.cookie_edge == 0.0f);
    CHECK(l.cookie_roll == 0.0f);
    l.cookie_roll = kInf;
    apply_quality(q, l);
    CHECK(l.cookie_roll == 0.0f);
    // The rest of the light is untouched.
    CHECK(l.spot_outer == 0.6f);
    CHECK(l.radius == 5.0f);

    SunCookie s;
    s.texture = 2;
    s.strength = 9.0f;
    s.size = 5000.0f;
    s.roll = -5.0f * kPi;
    s.offset = {1.0e6f, -3.0f};
    apply_quality(q, s);
    CHECK(s.strength == 1.0f);
    CHECK(s.size == 1000.0f);
    CHECK(std::fabs(s.roll) == doctest::Approx(kPi).epsilon(1e-4));
    CHECK(s.offset.x == 1.0e6f);  // an offset is any finite value
    CHECK(s.offset.y == -3.0f);
    s.size = 0.1f;
    apply_quality(q, s);
    CHECK(s.size == 0.5f);
    s.strength = kNaN;
    s.size = kNaN;
    s.offset = {kNaN, kInf};
    s.roll = kNaN;
    apply_quality(q, s);
    CHECK(s.strength == 1.0f);
    CHECK(s.size == 20.0f);
    CHECK(s.offset.x == 0.0f);
    CHECK(s.offset.y == 0.0f);
    CHECK(s.roll == 0.0f);
    CHECK(s.texture == 2);
}

TEST_CASE("light cookie: in-range values and the defaults pass through apply_quality unchanged") {
    const QualitySettings q = quality_settings(QualityTier::High);
    PointLight l = spot(3, 0.5f);
    l.cookie_edge = 0.25f;
    l.cookie_roll = 1.25f;
    apply_quality(q, l);
    CHECK(l.cookie == 3);
    CHECK(l.cookie_strength == 0.5f);
    CHECK(l.cookie_edge == 0.25f);
    CHECK(l.cookie_roll == 1.25f);
    PointLight plain;
    apply_quality(q, plain);
    CHECK(plain.cookie == ~0u);
    CHECK(plain.cookie_strength == 1.0f);
    CHECK(plain.cookie_roll == 0.0f);
    CHECK(plain.cookie_edge == 0.0f);
    SunCookie s;
    s.texture = 6;
    s.offset = {3.0f, -4.0f};
    s.roll = 0.5f;
    s.size = 12.5f;
    s.strength = 0.25f;
    apply_quality(q, s);
    CHECK(s.texture == 6);
    CHECK(s.offset.x == 3.0f);
    CHECK(s.offset.y == -4.0f);
    CHECK(s.roll == 0.5f);
    CHECK(s.size == 12.5f);
    CHECK(s.strength == 0.25f);
    SunCookie def;
    apply_quality(q, def);
    CHECK(def.texture == ~0u);
    CHECK(def.size == 20.0f);
    CHECK(def.strength == 1.0f);
}
