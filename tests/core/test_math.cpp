#include <doctest/doctest.h>

#include "mce/math/math.h"

using namespace mce;
using namespace mce::math;

// Hand-computed reference cases for the M0 function set. The DirectXMath randomized oracle sweep
// lives in test_math_oracle.cpp.

TEST_CASE("vector basics") {
    CHECK(dot(vec3{1, 2, 3}, vec3{4, 5, 6}) == 32.0f);
    CHECK(cross(vec3{1, 0, 0}, vec3{0, 1, 0}) == vec3{0, 0, 1});  // right-handed
    CHECK(near(length(vec3{3, 4, 0}), 5.0f));
    CHECK(near(normalize(vec3{0, 0, 9}), vec3{0, 0, 1}));
    CHECK(lerp(vec2{0, 0}, vec2{2, 4}, 0.5f) == vec2{1, 2});
}

TEST_CASE("matrix multiply and identity") {
    const mat4 t = translation({1, 2, 3});
    const mat4 s = scaling({2, 2, 2});
    CHECK(near(mul(mat4::identity(), t), t));
    // T*S applies S first.
    CHECK(near(transform_point(t * s, {1, 1, 1}), vec3{3, 4, 5}));
    CHECK(near(transform_point(s * t, {1, 1, 1}), vec3{4, 6, 8}));
    CHECK(t(0, 3) == 1.0f);  // row 0, column 3: translation lives in the last column
}

TEST_CASE("quaternion rotation is right-handed") {
    const quat q = quat_from_axis_angle({0, 1, 0}, kPi / 2);
    // +90° about +Y takes +X to -Z.
    CHECK(near(rotate(q, {1, 0, 0}), vec3{0, 0, -1}));
    CHECK(near(transform_dir(rotation(q), {1, 0, 0}), vec3{0, 0, -1}));
    const quat q2 = q * q;
    CHECK(near(rotate(q2, {1, 0, 0}), vec3{-1, 0, 0}));
    CHECK(near(rotate(conjugate(q), rotate(q, {0.3f, 0.5f, -2})), vec3{0.3f, 0.5f, -2}));
}

TEST_CASE("slerp endpoints, midpoint and shortest arc") {
    const quat a = quat::identity();
    const quat b = quat_from_axis_angle({0, 0, 1}, kPi / 2);
    CHECK(near(slerp(a, b, 0), a));
    CHECK(near(slerp(a, b, 1), b));
    CHECK(near(slerp(a, b, 0.5f), quat_from_axis_angle({0, 0, 1}, kPi / 4)));
    const quat neg_b{-b.x, -b.y, -b.z, -b.w};
    CHECK(near(slerp(a, neg_b, 0.5f), quat_from_axis_angle({0, 0, 1}, kPi / 4)));
}

TEST_CASE("trs and inverse") {
    const mat4 m = trs({1, -2, 3}, quat_from_axis_angle(normalize(vec3{1, 1, 0}), 0.7f), {2, 3, 4});
    bool ok = false;
    const mat4 inv = inverse(m, &ok);
    REQUIRE(ok);
    CHECK(near(m * inv, mat4::identity(), 1e-5f));
    CHECK(near(determinant(m), 24.0f, 1e-4f));

    const mat4 rigid = trs({5, 6, 7}, quat_from_axis_angle({0, 1, 0}, 1.1f), vec3{1});
    CHECK(near(inverse_rigid(rigid), inverse(rigid), 1e-5f));

    bool singular_ok = true;
    (void)inverse(scaling({1, 0, 1}), &singular_ok);
    CHECK_FALSE(singular_ok);
}

TEST_CASE("look_at_rh maps eye to origin and target to -Z") {
    const vec3 eye{0, 0, 5};
    const mat4 v = look_at_rh(eye, {0, 0, 0}, {0, 1, 0});
    CHECK(near(transform_point(v, eye), vec3{0, 0, 0}));
    CHECK(near(transform_point(v, {0, 0, 0}), vec3{0, 0, -5}));
    CHECK(near(transform_point(v, {1, 0, 5}), vec3{1, 0, 0}));  // +X stays right
}

TEST_CASE("perspective maps near/far planes into clip range") {
    const f32 zn = 0.1f, zf = 100.0f;
    auto ndc_z = [](const mat4& p, f32 view_z) {
        const vec4 c = p * vec4{0, 0, view_z, 1};
        return c.z / c.w;
    };
    const mat4 gl = perspective_rh_no(radians(60), 16.0f / 9.0f, zn, zf);
    CHECK(near(ndc_z(gl, -zn), -1.0f, 1e-4f));
    CHECK(near(ndc_z(gl, -zf), 1.0f, 1e-4f));
    const mat4 zo = perspective_rh_zo(radians(60), 16.0f / 9.0f, zn, zf);
    CHECK(near(ndc_z(zo, -zn), 0.0f, 1e-4f));
    CHECK(near(ndc_z(zo, -zf), 1.0f, 1e-4f));
    const mat4 o = ortho_rh_no(-1, 1, -1, 1, zn, zf);
    CHECK(near(ndc_z(o, -zn), -1.0f, 1e-4f));
}

TEST_CASE("aabb") {
    const aabb2 a{{0, 0}, {2, 2}};
    const aabb2 b{{1, 1}, {3, 3}};
    CHECK(a.overlaps(b));
    CHECK(a.contains({1, 1}));
    CHECK_FALSE(a.contains({2.5f, 1}));
    const aabb2 m = merge(a, b);
    CHECK(m.min == vec2{0, 0});
    CHECK(m.max == vec2{3, 3});
}

TEST_CASE("math: mat4_identity leaves points alone and is the identity of mul") {
    const mat4 id = mat4_identity();
    for (usize c = 0; c < 4; ++c)
        for (usize r = 0; r < 4; ++r) CHECK(id.c[c][r] == (c == r ? 1.0f : 0.0f));
    const mat4 t = translation(vec3{1, 2, 3});
    CHECK(near(transform_point(mul(id, t), {0, 0, 0}), vec3{1, 2, 3}));
    CHECK(near(transform_point(mul(t, id), {0, 0, 0}), vec3{1, 2, 3}));
}

TEST_CASE("math: Color::to_vec4 keeps the channel order r g b a") {
    const vec4 v = Color{0.1f, 0.2f, 0.3f, 0.4f}.to_vec4();
    CHECK(v.x == 0.1f);
    CHECK(v.y == 0.2f);
    CHECK(v.z == 0.3f);
    CHECK(v.w == 0.4f);
}
