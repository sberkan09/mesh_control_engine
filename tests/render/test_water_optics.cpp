// M-WATER2 49b: the pure helpers of the water's refraction and reflection (mce/render/water_optics.h), which kWaterFS
// repeats as shader text, and the renderer's flow phase step (49f). Exact CPU checks; the GL twin is
// tests/render_gl/test_render3d_water_optics_gl.cpp.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/render/forward3d.h"
#include "mce/render/water_optics.h"

using namespace mce;
using namespace mce::render;
using math::vec2;
using math::vec3;

namespace {

const f32 kNan = std::numeric_limits<f32>::quiet_NaN();

// A camera 3 m up looking 30 degrees down toward -z, turned 20 degrees about y (a general view matrix).
math::mat4 test_view() {
    const vec3 eye{1.0f, 3.0f, 2.0f};
    const f32 yaw = math::radians(20.0f), pitch = math::radians(-30.0f);
    const vec3 fwd{-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    return math::look_at_rh(eye, eye + fwd, vec3{0.0f, 1.0f, 0.0f});
}

f32 len(vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

}  // namespace

TEST_CASE("Forward3DStats: the water reflect and refract bools default to false") {
    const Forward3DStats st{};
    CHECK_FALSE(st.water_reflect);
    CHECK_FALSE(st.water_refract);
}

TEST_CASE("water_refract_offset: exactly zero for an up normal and for path 0") {
    const math::mat4 v = test_view();
    const vec2 up = water_refract_offset(v, vec3{0.0f, 1.0f, 0.0f}, 2.0f, 6.0f);
    CHECK(up.x == 0.0f);
    CHECK(up.y == 0.0f);
    const vec3 tilted = math::normalize(vec3{0.2f, 1.0f, -0.1f});
    const vec2 p0 = water_refract_offset(v, tilted, 0.0f, 6.0f);
    CHECK(p0.x == 0.0f);
    CHECK(p0.y == 0.0f);
    const vec2 neg = water_refract_offset(v, tilted, -1.0f, 6.0f);  // nothing behind: no bend
    CHECK(neg.x == 0.0f);
    CHECK(neg.y == 0.0f);
    CHECK(len(water_refract_offset(v, tilted, 1.0f, 6.0f)) > 0.0f);
}

TEST_CASE("water_refract_offset: grows with the tilt and the path, shrinks with the view depth") {
    const math::mat4 v = test_view();
    f32 prev = 0.0f;
    for (int i = 1; i <= 5; ++i) {  // tilt, small enough to stay under the uv cap
        const f32 o = len(water_refract_offset(v, math::normalize(vec3{0.02f * f32(i), 1.0f, 0.0f}), 1.0f, 8.0f));
        CHECK(o > prev);
        prev = o;
    }
    prev = 0.0f;
    const vec3 n = math::normalize(vec3{0.05f, 1.0f, 0.03f});
    for (int i = 1; i <= 8; ++i) {  // path, up to kWaterRefractMaxPath
        const f32 o = len(water_refract_offset(v, n, 0.5f * f32(i), 30.0f));
        CHECK(o > prev);
        prev = o;
    }
    CHECK(len(water_refract_offset(v, n, 1.0f, 4.0f)) > len(water_refract_offset(v, n, 1.0f, 8.0f)));
    // The formula: the tilt in view space times scale * path / zw.
    const vec2 o = water_refract_offset(v, n, 1.0f, 30.0f);
    const f32 tx = v.c[0][0] * n.x + v.c[2][0] * n.z, ty = v.c[0][1] * n.x + v.c[2][1] * n.z;
    CHECK(o.x == doctest::Approx(tx * kWaterRefractScale / 30.0f));
    CHECK(o.y == doctest::Approx(ty * kWaterRefractScale / 30.0f));
}

TEST_CASE("water_refract_offset: capped by kWaterRefractMaxPath and kWaterRefractMaxUv; NaN gives zero") {
    const math::mat4 v = test_view();
    const vec3 n = math::normalize(vec3{0.01f, 1.0f, 0.0f});
    const vec2 at_cap = water_refract_offset(v, n, kWaterRefractMaxPath, 40.0f);
    const vec2 past = water_refract_offset(v, n, 50.0f * kWaterRefractMaxPath, 40.0f);
    CHECK(past.x == at_cap.x);
    CHECK(past.y == at_cap.y);
    const vec2 steep = water_refract_offset(v, math::normalize(vec3{0.7f, 1.0f, 0.4f}), 4.0f, 1.0f);
    CHECK(len(steep) == doctest::Approx(kWaterRefractMaxUv));
    CHECK(len(steep) <= kWaterRefractMaxUv * 1.0001f);
    for (const vec2 o : {water_refract_offset(v, vec3{kNan, 1.0f, 0.0f}, 1.0f, 5.0f),
                         water_refract_offset(v, n, 1.0f, kNan), water_refract_offset(v, n, kNan, 5.0f)}) {
        CHECK(o.x == 0.0f);
        CHECK(o.y == 0.0f);
    }
}

TEST_CASE("water_refract_accept: only a surface behind the water and under its level") {
    CHECK(water_refract_accept(6.0f, 5.0f, -0.5f, 0.0f));
    CHECK_FALSE(water_refract_accept(4.0f, 5.0f, -0.5f, 0.0f));  // an object in front of the water
    CHECK_FALSE(water_refract_accept(5.0f, 5.0f, -0.5f, 0.0f));
    CHECK_FALSE(water_refract_accept(0.0f, 5.0f, -0.5f, 0.0f));  // nothing drawn there
    CHECK_FALSE(water_refract_accept(6.0f, 5.0f, 0.3f, 0.0f));   // behind, but above the surface (a post's dry part)
    CHECK_FALSE(water_refract_accept(6.0f, 5.0f, 0.0f, 0.0f));
    CHECK_FALSE(water_refract_accept(kNan, 5.0f, -0.5f, 0.0f));
    CHECK_FALSE(water_refract_accept(6.0f, kNan, -0.5f, 0.0f));
    CHECK_FALSE(water_refract_accept(6.0f, 5.0f, kNan, 0.0f));
    CHECK_FALSE(water_refract_accept(6.0f, 0.0f, -0.5f, 0.0f));
}

TEST_CASE("water_reflect_fade: 1 in the middle, 0 on the border, monotone toward it, 0 past the trace's reach") {
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, 1.0f, -0.5f) == 1.0f);
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, 0.0f, 0.0f) == 1.0f);
    for (const vec2 uv :
         {vec2{0.0f, 0.5f}, vec2{1.0f, 0.5f}, vec2{0.5f, 0.0f}, vec2{0.5f, 1.0f}, vec2{-0.1f, 0.5f}, vec2{0.5f, 1.2f}})
        CHECK(water_reflect_fade(uv, 1.0f, 0.0f) == 0.0f);
    f32 prev = 2.0f;
    for (int i = 0; i <= 50; ++i) {  // from the middle to the left border
        const f32 c = water_reflect_fade(vec2{0.5f - 0.01f * f32(i), 0.4f}, 2.0f, 0.0f);
        CHECK(c <= prev);
        prev = c;
    }
    CHECK(prev == 0.0f);
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, kWaterTraceFadeStart * kWaterTraceMaxDist, 0.0f) == 1.0f);
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, 0.85f * kWaterTraceMaxDist, 0.0f) > 0.0f);
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, 0.85f * kWaterTraceMaxDist, 0.0f) < 1.0f);
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, kWaterTraceMaxDist, 0.0f) == 0.0f);
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, 2.0f * kWaterTraceMaxDist, 0.0f) == 0.0f);
    // Rays toward the camera fade out.
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, 1.0f, 0.3f) < 1.0f);
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, 1.0f, kWaterFacingFade) == 0.0f);
    CHECK(water_reflect_fade(vec2{kNan, 0.5f}, 1.0f, 0.0f) == 0.0f);
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, kNan, 0.0f) == 0.0f);
    CHECK(water_reflect_fade(vec2{0.5f, 0.5f}, 1.0f, kNan) == 0.0f);
}

TEST_CASE("water_flow_cycles: a whole number, 0 at rest, odd in the velocity") {
    const vec2 k{0.26179939f, 0.0f};  // a 24 m wave along x
    CHECK(water_flow_cycles(k, vec2{0.0f, 0.0f}) == 0.0);
    CHECK(water_flow_cycles(k, vec2{0.0f, 3.0f}) == 0.0);  // across the wave
    for (const f32 u : {0.05f, 0.1f, 0.7f, 1.2f, 3.3f, 5.0f}) {
        const f64 c = water_flow_cycles(k, vec2{u, 0.0f});
        CHECK(c == std::floor(c));
        CHECK(water_flow_cycles(k, vec2{-u, 0.0f}) == -c);
        const f64 exact = f64(k.x) * f64(u) * kWaterOpticsPeriod / 6.283185307179586477;
        CHECK(std::fabs(c - exact) <= 0.5);
    }
    CHECK(water_flow_cycles(vec2{5.7f, 0.0f}, vec2{1.2f, 0.0f}) ==
          std::round(f64(5.7f) * f64(1.2f) * 64.0 / 6.283185307179586477));
    CHECK(water_flow_cycles(k, vec2{kNan, 0.0f}) == 0.0);
    CHECK(water_flow_cycles(k, vec2{std::numeric_limits<f32>::infinity(), 0.0f}) == 0.0);
}
