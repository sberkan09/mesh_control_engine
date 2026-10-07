// DirectXMath oracle for mce::math (core.md §2.7, §8 test 1; ROADMAP M0 "math oracle").
//
// Every mce::math function with a DirectXMath counterpart is compared against it over a seeded
// randomized sweep. Tolerances are stated in the quantity's own unit (meters for positions and
// translations, radians for angles, unitless for directions and matrix entries) and scale with
// input magnitude.
//
// Convention bridge (asserted, not assumed — see "layout bridge" below):
//   mce:  column vectors, column-major storage, mat4::c[col][row], v' = M * v
//   DXM:  row vectors,    row-major storage,    XMFLOAT4X4::m[row][col], v' = v * M
// The two transpositions cancel, so one transform has the same 16 floats in both libraries:
// mce c[i][j] == DXM m[i][j]. Composition order flips: mce (a * b) == XMMatrixMultiply(b, a), and
// mce quat (a * b) == XMQuaternionMultiply(b, a).
// Handedness: compared against the DXM *RH* builders. DXM projections are zero-to-one; the GL
// (negative-one-to-one) variants are checked against DXM followed by the clip remap z' = 2z - w.

#include <DirectXCollision.h>
#include <DirectXMath.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>

#include "mce/math/math.h"

using namespace mce;
using namespace mce::math;
namespace dx = DirectX;

namespace {

constexpr int kIterations = 2000;

// ---------------------------------------------------------------- deterministic RNG (splitmix64)
struct Rng {
    u64 state;
    explicit Rng(u64 seed) : state(seed) {}
    u64 next() {
        u64 z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    f32 unit() { return static_cast<f32>(next() >> 40) * (1.0f / 16777216.0f); }  // [0, 1)
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
    vec2 v2(f32 r) { return {range(-r, r), range(-r, r)}; }
    vec3 v3(f32 r) { return {range(-r, r), range(-r, r), range(-r, r)}; }
    vec4 v4(f32 r) { return {range(-r, r), range(-r, r), range(-r, r), range(-r, r)}; }
    vec3 dir() {
        for (;;) {
            const vec3 v = v3(1.0f);
            const f32 l2 = length_sq(v);
            if (l2 > 0.01f && l2 <= 1.0f) return v / std::sqrt(l2);
        }
    }
    quat rot() { return quat_from_axis_angle(dir(), range(-kPi, kPi)); }
    vec3 scale() {  // positive, well-conditioned scales
        return {range(0.25f, 4.0f), range(0.25f, 4.0f), range(0.25f, 4.0f)};
    }
};

// ---------------------------------------------------------------- conversions
dx::XMVECTOR xm(vec2 v) { return dx::XMVectorSet(v.x, v.y, 0, 0); }
dx::XMVECTOR xm(vec3 v) { return dx::XMVectorSet(v.x, v.y, v.z, 0); }
dx::XMVECTOR xm(vec4 v) { return dx::XMVectorSet(v.x, v.y, v.z, v.w); }
dx::XMVECTOR xm(quat q) { return dx::XMVectorSet(q.x, q.y, q.z, q.w); }
dx::XMMATRIX xm(const mat4& m) {
    dx::XMFLOAT4X4 f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) f.m[i][j] = m.c[i][j];
    return dx::XMLoadFloat4x4(&f);
}
vec2 to_v2(dx::FXMVECTOR v) { return {dx::XMVectorGetX(v), dx::XMVectorGetY(v)}; }
vec3 to_v3(dx::FXMVECTOR v) { return {dx::XMVectorGetX(v), dx::XMVectorGetY(v), dx::XMVectorGetZ(v)}; }
vec4 to_v4(dx::FXMVECTOR v) {
    return {dx::XMVectorGetX(v), dx::XMVectorGetY(v), dx::XMVectorGetZ(v), dx::XMVectorGetW(v)};
}
quat to_q(dx::FXMVECTOR v) {
    return {dx::XMVectorGetX(v), dx::XMVectorGetY(v), dx::XMVectorGetZ(v), dx::XMVectorGetW(v)};
}
mat4 to_m(dx::FXMMATRIX m) {
    dx::XMFLOAT4X4 f;
    dx::XMStoreFloat4x4(&f, m);
    mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.c[i][j] = f.m[i][j];
    return r;
}

// ---------------------------------------------------------------- tolerant comparison
// |a - b| <= abs_tol + rel_tol * max(|a|, |b|): abs_tol is in the value's unit (meters, radians…).
struct Tol {
    f32 abs_tol;
    f32 rel_tol;
};
bool close(f32 a, f32 b, Tol t) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return std::fabs(a - b) <= t.abs_tol + t.rel_tol * max(std::fabs(a), std::fabs(b));
}
bool close(vec2 a, vec2 b, Tol t) { return close(a.x, b.x, t) && close(a.y, b.y, t); }
bool close(vec3 a, vec3 b, Tol t) { return close(a.x, b.x, t) && close(a.y, b.y, t) && close(a.z, b.z, t); }
bool close(vec4 a, vec4 b, Tol t) { return close(a.xyz(), b.xyz(), t) && close(a.w, b.w, t); }
bool close(quat a, quat b, Tol t) {  // q and -q are the same rotation
    const auto same = [&](f32 s) {
        return close(a.x, s * b.x, t) && close(a.y, s * b.y, t) && close(a.z, s * b.z, t) && close(a.w, s * b.w, t);
    };
    return same(1.0f) || same(-1.0f);
}
// Matrices: entries compared relative to the matrix's own magnitude, so translation columns in
// meters and rotation entries share one bound.
bool close(const mat4& a, const mat4& b, Tol t) {
    f32 mag = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) mag = max(mag, max(std::fabs(a.c[i][j]), std::fabs(b.c[i][j])));
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            if (std::isnan(a.c[i][j]) || std::isnan(b.c[i][j])) return false;
            if (std::fabs(a.c[i][j] - b.c[i][j]) > t.abs_tol + t.rel_tol * mag) return false;
        }
    return true;
}

void print(const char* what, const mat4& m) {
    std::printf("%s =\n", what);
    for (int r = 0; r < 4; ++r) std::printf("  [% .7g % .7g % .7g % .7g]\n", m(r, 0), m(r, 1), m(r, 2), m(r, 3));
}

constexpr Tol kExact{1e-6f, 1e-6f};     // a handful of roundings
constexpr Tol kTrig{2e-6f, 4e-6f};      // DXM uses polynomial sin/cos (XMScalarSinCos)
constexpr Tol kComposite{1e-5f, 1e-5f}; // products / inverses of well-conditioned matrices

// Report the first failing iteration with its inputs, then stop the sweep for that case.
#define ORACLE_CHECK(cond, ...)                                                                    \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            FAIL_CHECK("oracle mismatch at iteration " << it << ": " #cond);                       \
            __VA_ARGS__;                                                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

}  // namespace

// ================================================================ layout and convention bridge

TEST_CASE("oracle: layout bridge — one transform has identical floats in both libraries") {
    // Translation lives in c[3] for mce and in row 3 for DXM: same 16 floats.
    const vec3 t{1.5f, -2.0f, 3.25f};
    dx::XMFLOAT4X4 f;
    dx::XMStoreFloat4x4(&f, dx::XMMatrixTranslation(t.x, t.y, t.z));
    const mat4 m = translation(t);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) CHECK(m.c[i][j] == f.m[i][j]);
    // Points go through the same transform either way.
    const vec3 p{0.5f, 4.0f, -1.0f};
    CHECK(close(transform_point(m, p), to_v3(dx::XMVector3Transform(xm(p), xm(m))), kExact));
}

TEST_CASE("oracle: right-handed rotation sense matches XMMatrixRotationY") {
    // +90 degrees about +Y takes +X to -Z in a right-handed system.
    const mat4 ours = rotation(quat_from_axis_angle({0, 1, 0}, kPi / 2));
    const mat4 theirs = to_m(dx::XMMatrixRotationY(dx::XM_PIDIV2));
    CHECK(close(ours, theirs, kTrig));
    CHECK(close(transform_dir(ours, {1, 0, 0}), vec3{0, 0, -1}, kTrig));
}

// ================================================================ scalars

TEST_CASE("oracle: radians / degrees") {
    Rng rng(0x5EED0001);
    for (int it = 0; it < kIterations; ++it) {
        const f32 d = rng.range(-720, 720);
        ORACLE_CHECK(close(radians(d), dx::XMConvertToRadians(d), kExact));
        const f32 r = rng.range(-4 * kPi, 4 * kPi);
        ORACLE_CHECK(close(degrees(r), dx::XMConvertToDegrees(r), kExact));
    }
}

TEST_CASE("oracle: scalar lerp / clamp") {
    Rng rng(0x5EED0002);
    for (int it = 0; it < kIterations; ++it) {
        const f32 a = rng.range(-100, 100), b = rng.range(-100, 100), t = rng.range(-0.5f, 1.5f);
        ORACLE_CHECK(close(math::lerp(a, b, t), dx::XMVectorGetX(dx::XMVectorLerp(dx::XMVectorReplicate(a), dx::XMVectorReplicate(b), t)),
                           kExact));
        const f32 lo = min(a, b), hi = max(a, b), v = rng.range(-150, 150);
        ORACLE_CHECK(clamp(v, lo, hi) == dx::XMVectorGetX(dx::XMVectorClamp(dx::XMVectorReplicate(v), dx::XMVectorReplicate(lo),
                                                                              dx::XMVectorReplicate(hi))));
    }
}

// ================================================================ vectors

TEST_CASE("oracle: vec2 dot / cross / length / normalize / lerp / min / max") {
    Rng rng(0x5EED0010);
    for (int it = 0; it < kIterations; ++it) {
        const vec2 a = rng.v2(100), b = rng.v2(100);
        const f32 t = rng.range(0, 1);
        ORACLE_CHECK(close(dot(a, b), dx::XMVectorGetX(dx::XMVector2Dot(xm(a), xm(b))), Tol{1e-6f * length(a) * length(b), 0}));
        ORACLE_CHECK(close(cross(a, b), dx::XMVectorGetX(dx::XMVector2Cross(xm(a), xm(b))), Tol{1e-6f * length(a) * length(b), 0}));
        ORACLE_CHECK(close(length(a), dx::XMVectorGetX(dx::XMVector2Length(xm(a))), kExact));
        ORACLE_CHECK(close(length_sq(a), dx::XMVectorGetX(dx::XMVector2LengthSq(xm(a))), kExact));
        if (length(a) > 1e-3f) ORACLE_CHECK(close(normalize(a), to_v2(dx::XMVector2Normalize(xm(a))), kExact));
        ORACLE_CHECK(close(lerp(a, b, t), to_v2(dx::XMVectorLerp(xm(a), xm(b), t)), kExact));
        ORACLE_CHECK(min(a, b) == to_v2(dx::XMVectorMin(xm(a), xm(b))));
        ORACLE_CHECK(max(a, b) == to_v2(dx::XMVectorMax(xm(a), xm(b))));
        ORACLE_CHECK(close(a + b, to_v2(dx::XMVectorAdd(xm(a), xm(b))), kExact));
        ORACLE_CHECK(close(a - b, to_v2(dx::XMVectorSubtract(xm(a), xm(b))), kExact));
        ORACLE_CHECK(close(a * t, to_v2(dx::XMVectorScale(xm(a), t)), kExact));
    }
}

TEST_CASE("oracle: vec3 dot / cross / length / normalize / lerp / min / max") {
    Rng rng(0x5EED0011);
    for (int it = 0; it < kIterations; ++it) {
        const vec3 a = rng.v3(100), b = rng.v3(100);
        const f32 t = rng.range(0, 1);
        ORACLE_CHECK(close(dot(a, b), dx::XMVectorGetX(dx::XMVector3Dot(xm(a), xm(b))), Tol{1e-6f * length(a) * length(b), 0}));
        // cross: compare in meters^2 relative to |a||b|, the natural scale of the result
        const vec3 c = cross(a, b);
        const vec3 cx = to_v3(dx::XMVector3Cross(xm(a), xm(b)));
        const Tol ct{1e-6f * length(a) * length(b), 0};
        ORACLE_CHECK(close(c, cx, ct));
        ORACLE_CHECK(close(length(a), dx::XMVectorGetX(dx::XMVector3Length(xm(a))), kExact));
        ORACLE_CHECK(close(length_sq(a), dx::XMVectorGetX(dx::XMVector3LengthSq(xm(a))), kExact));
        if (length(a) > 1e-3f) ORACLE_CHECK(close(normalize(a), to_v3(dx::XMVector3Normalize(xm(a))), kExact));
        ORACLE_CHECK(close(lerp(a, b, t), to_v3(dx::XMVectorLerp(xm(a), xm(b), t)), kExact));
        ORACLE_CHECK(min(a, b) == to_v3(dx::XMVectorMin(xm(a), xm(b))));
        ORACLE_CHECK(max(a, b) == to_v3(dx::XMVectorMax(xm(a), xm(b))));
        ORACLE_CHECK(close(-a, to_v3(dx::XMVectorNegate(xm(a))), kExact));
        ORACLE_CHECK(close(a * b, to_v3(dx::XMVectorMultiply(xm(a), xm(b))), kExact));
        if (std::fabs(b.x) > 1e-3f && std::fabs(b.y) > 1e-3f && std::fabs(b.z) > 1e-3f)
            ORACLE_CHECK(close(a / b, to_v3(dx::XMVectorDivide(xm(a), xm(b))), kExact));
    }
}

TEST_CASE("oracle: vec4 dot / length / normalize / lerp") {
    Rng rng(0x5EED0012);
    for (int it = 0; it < kIterations; ++it) {
        const vec4 a = rng.v4(100), b = rng.v4(100);
        const f32 t = rng.range(0, 1);
        ORACLE_CHECK(close(dot(a, b), dx::XMVectorGetX(dx::XMVector4Dot(xm(a), xm(b))), Tol{1e-6f * length(a) * length(b), 0}));
        ORACLE_CHECK(close(length(a), dx::XMVectorGetX(dx::XMVector4Length(xm(a))), kExact));
        if (length(a) > 1e-3f) ORACLE_CHECK(close(normalize(a), to_v4(dx::XMVector4Normalize(xm(a))), kExact));
        ORACLE_CHECK(close(lerp(a, b, t), to_v4(dx::XMVectorLerp(xm(a), xm(b), t)), kExact));
    }
}

TEST_CASE("oracle: normalize of zero and underflowing vectors returns zero, not NaN") {
    const f32 tiny = 1e-30f;  // length_sq underflows to 0
    CHECK(normalize(vec2{0, 0}) == to_v2(dx::XMVector2Normalize(dx::XMVectorZero())));
    CHECK(normalize(vec3{0, 0, 0}) == to_v3(dx::XMVector3Normalize(dx::XMVectorZero())));
    CHECK(normalize(vec4{0, 0, 0, 0}) == to_v4(dx::XMVector4Normalize(dx::XMVectorZero())));
    CHECK(normalize(vec3{tiny, 0, 0}) == to_v3(dx::XMVector3Normalize(xm(vec3{tiny, 0, 0}))));
    CHECK(normalize(quat{0, 0, 0, 0}) == to_q(dx::XMQuaternionNormalize(dx::XMVectorZero())));
    // Small but representable lengths still normalize.
    CHECK(close(normalize(vec3{1e-18f, 0, 0}), to_v3(dx::XMVector3Normalize(xm(vec3{1e-18f, 0, 0}))), kExact));
}

// ================================================================ quaternions

TEST_CASE("oracle: quat_from_axis_angle / multiply / conjugate / normalize / dot") {
    Rng rng(0x5EED0020);
    for (int it = 0; it < kIterations; ++it) {
        const vec3 axis = rng.dir();
        const f32 angle = rng.range(-kPi, kPi);
        const quat q = quat_from_axis_angle(axis, angle);
        ORACLE_CHECK(close(q, to_q(dx::XMQuaternionRotationNormal(xm(axis), angle)), kTrig));
        ORACLE_CHECK(close(q, to_q(dx::XMQuaternionRotationAxis(xm(axis), angle)), kTrig));

        const quat a = rng.rot(), b = rng.rot();
        // mce (a * b) applies b first; DXM's XMQuaternionMultiply(Q1, Q2) applies Q1 first.
        ORACLE_CHECK(close(a * b, to_q(dx::XMQuaternionMultiply(xm(b), xm(a))), kExact));
        ORACLE_CHECK(conjugate(a) == to_q(dx::XMQuaternionConjugate(xm(a))));
        ORACLE_CHECK(close(dot(a, b), dx::XMVectorGetX(dx::XMQuaternionDot(xm(a), xm(b))), kExact));
        const quat u{rng.range(-3, 3), rng.range(-3, 3), rng.range(-3, 3), rng.range(-3, 3)};
        if (std::sqrt(dot(u, u)) > 1e-2f) ORACLE_CHECK(close(normalize(u), to_q(dx::XMQuaternionNormalize(xm(u))), kExact));
    }
}

TEST_CASE("oracle: rotate(quat, vec3) vs XMVector3Rotate (meters)") {
    Rng rng(0x5EED0021);
    for (int it = 0; it < kIterations; ++it) {
        const quat q = rng.rot();
        const vec3 v = rng.v3(50);
        ORACLE_CHECK(close(rotate(q, v), to_v3(dx::XMVector3Rotate(xm(v), xm(q))), Tol{1e-5f * max(1.0f, length(v)), 0}));
    }
}

TEST_CASE("oracle: slerp vs XMQuaternionSlerp (radians)") {
    Rng rng(0x5EED0022);
    // Angular distance between two unit quaternions as rotations, in radians.
    // atan2 form: acos(|dot|) is ill-conditioned near 1 and would report ~1e-3 rad of pure noise.
    auto angle_between = [](quat a, quat b) {
        const f32 s = dot(a, b) < 0 ? -1.0f : 1.0f;
        const vec4 va{a.x, a.y, a.z, a.w}, vb{s * b.x, s * b.y, s * b.z, s * b.w};
        return 4.0f * std::atan2(length(va - vb), length(va + vb));
    };
    for (int it = 0; it < kIterations; ++it) {
        const quat a = rng.rot();
        quat b;
        switch (it % 4) {
            case 0: b = rng.rot(); break;                                           // anywhere
            case 1: b = a * quat_from_axis_angle(rng.dir(), rng.range(-0.05f, 0.05f)); break;  // nearly equal
            case 2: b = a * quat_from_axis_angle(rng.dir(), rng.range(2.9f, 3.1f)); break;     // nearly opposite
            default: {                                                              // other hemisphere
                const quat r = rng.rot();
                b = {-r.x, -r.y, -r.z, -r.w};
            }
        }
        const f32 t = rng.range(0, 1);
        const quat ours = slerp(a, b, t);
        const quat theirs = to_q(dx::XMQuaternionSlerp(xm(a), xm(b), t));
        ORACLE_CHECK(close(std::sqrt(dot(ours, ours)), 1.0f, Tol{1e-5f, 0}), std::printf("|slerp| = %g\n", std::sqrt(dot(ours, ours))));
        // 2e-5 rad ~ 0.001 degrees.
        ORACLE_CHECK(angle_between(ours, normalize(theirs)) <= 2e-5f + 2e-6f,
                     std::printf("t=%g a=(%g %g %g %g) b=(%g %g %g %g) err=%g rad\n", t, a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w,
                                 angle_between(ours, normalize(theirs))));
    }
}

// ================================================================ matrices

TEST_CASE("oracle: mul / mat*vec4 / transform_point / transform_dir / transpose") {
    Rng rng(0x5EED0030);
    for (int it = 0; it < kIterations; ++it) {
        const mat4 a = trs(rng.v3(20), rng.rot(), rng.scale());
        const mat4 b = trs(rng.v3(20), rng.rot(), rng.scale());
        mat4 g;  // a general (non-affine) matrix too
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) g.c[i][j] = rng.range(-5, 5);

        ORACLE_CHECK(close(mul(a, b), to_m(dx::XMMatrixMultiply(xm(b), xm(a))), kComposite), print("ours", mul(a, b)),
                     print("dxm", to_m(dx::XMMatrixMultiply(xm(b), xm(a)))));
        ORACLE_CHECK(close(g * a, to_m(dx::XMMatrixMultiply(xm(a), xm(g))), kComposite));
        ORACLE_CHECK(close(transpose(g), to_m(dx::XMMatrixTranspose(xm(g))), Tol{0, 0}));

        const vec4 v = rng.v4(10);
        const vec3 p = rng.v3(10);
        ORACLE_CHECK(close(g * v, to_v4(dx::XMVector4Transform(xm(v), xm(g))), Tol{1e-5f, 1e-6f}));
        ORACLE_CHECK(close(transform_point(a, p), to_v3(dx::XMVector3Transform(xm(p), xm(a))), Tol{1e-5f, 1e-6f}));
        ORACLE_CHECK(close(transform_dir(a, p), to_v3(dx::XMVector3TransformNormal(xm(p), xm(a))), Tol{1e-5f, 1e-6f}));
    }
}

TEST_CASE("oracle: translation / scaling / rotation / trs") {
    Rng rng(0x5EED0031);
    for (int it = 0; it < kIterations; ++it) {
        const vec3 t = rng.v3(100), s = rng.scale();
        const quat r = rng.rot();
        ORACLE_CHECK(close(translation(t), to_m(dx::XMMatrixTranslation(t.x, t.y, t.z)), Tol{0, 0}));
        ORACLE_CHECK(close(scaling(s), to_m(dx::XMMatrixScaling(s.x, s.y, s.z)), Tol{0, 0}));
        ORACLE_CHECK(close(rotation(r), to_m(dx::XMMatrixRotationQuaternion(xm(r))), kExact));
        // mce trs = T * R * S (column vectors) == DXM S * R * T (row vectors) == XMMatrixAffineTransformation.
        const mat4 ours = trs(t, r, s);
        const mat4 theirs = to_m(dx::XMMatrixAffineTransformation(xm(s), dx::XMVectorZero(), xm(r), xm(t)));
        ORACLE_CHECK(close(ours, theirs, kComposite), print("ours", ours), print("dxm", theirs));
        ORACLE_CHECK(close(ours, translation(t) * rotation(r) * scaling(s), kComposite));
    }
}

TEST_CASE("oracle: inverse / determinant / inverse_rigid") {
    Rng rng(0x5EED0032);
    for (int it = 0; it < kIterations; ++it) {
        const mat4 m = trs(rng.v3(20), rng.rot(), rng.scale());
        dx::XMVECTOR det_x;
        const mat4 theirs = to_m(dx::XMMatrixInverse(&det_x, xm(m)));
        bool ok = false;
        const mat4 ours = inverse(m, &ok);
        ORACLE_CHECK(ok);
        ORACLE_CHECK(close(determinant(m), dx::XMVectorGetX(det_x), Tol{1e-5f, 1e-5f}));
        ORACLE_CHECK(close(determinant(m), dx::XMVectorGetX(dx::XMMatrixDeterminant(xm(m))), Tol{1e-5f, 1e-5f}));
        ORACLE_CHECK(close(ours, theirs, kComposite), print("m", m), print("ours", ours), print("dxm", theirs));

        // Rigid (rotation + translation) fast path vs the full DXM inverse.
        const mat4 rigid = trs(rng.v3(100), rng.rot(), vec3{1});
        const mat4 rigid_dx = to_m(dx::XMMatrixInverse(nullptr, xm(rigid)));
        ORACLE_CHECK(close(inverse_rigid(rigid), rigid_dx, kComposite), print("ours", inverse_rigid(rigid)), print("dxm", rigid_dx));

        // General (non-affine) matrices: compare the round trip, which is what callers rely on.
        mat4 g;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) g.c[i][j] = rng.range(-5, 5);
        const f32 det_g = dx::XMVectorGetX(dx::XMMatrixDeterminant(xm(g)));
        ORACLE_CHECK(close(determinant(g), det_g, Tol{1e-3f, 1e-5f}));
        if (std::fabs(det_g) > 1.0f) {
            const mat4 gi = inverse(g);
            ORACLE_CHECK(close(gi, to_m(dx::XMMatrixInverse(nullptr, xm(g))), Tol{1e-4f, 1e-4f}));
            ORACLE_CHECK(close(g * gi, mat4::identity(), Tol{1e-4f, 0}));
        }
    }
}

TEST_CASE("oracle: look_at_rh vs XMMatrixLookAtRH") {
    Rng rng(0x5EED0040);
    for (int it = 0; it < kIterations; ++it) {
        const vec3 eye = rng.v3(50);
        const vec3 at = eye + rng.dir() * rng.range(0.5f, 50.0f);
        vec3 up = rng.dir();
        if (length(cross(normalize(at - eye), up)) < 0.1f) up = {0, 1, 0};  // skip near-degenerate bases
        if (length(cross(normalize(at - eye), up)) < 0.1f) continue;
        const mat4 ours = look_at_rh(eye, at, up);
        const mat4 theirs = to_m(dx::XMMatrixLookAtRH(xm(eye), xm(at), xm(up)));
        // Rotation part unitless; translation column in meters relative to |eye|.
        ORACLE_CHECK(close(ours, theirs, Tol{1e-5f, 1e-6f}), print("ours", ours), print("dxm", theirs));
        // Camera conventions: eye -> origin, target on -Z.
        ORACLE_CHECK(close(transform_point(ours, eye), vec3{0}, Tol{1e-4f, 0}));
        const vec3 at_v = transform_point(ours, at);
        ORACLE_CHECK(at_v.z < 0 && close(at_v.xy(), vec2{0, 0}, Tol{1e-4f, 0}));
    }
}

namespace {
// GL clip remap applied after a zero-to-one projection: z' = 2z - w (column-vector form).
mat4 zo_to_no() {
    mat4 r = mat4::identity();
    r.c[2][2] = 2.0f;
    r.c[3][2] = -1.0f;
    return r;
}
vec3 ndc(const mat4& p, vec3 view) {
    const vec4 c = p * vec4(view, 1.0f);
    return c.xyz() / c.w;
}
}  // namespace

TEST_CASE("oracle: perspective_rh_zo vs XMMatrixPerspectiveFovRH; perspective_rh_no vs remapped DXM") {
    Rng rng(0x5EED0050);
    for (int it = 0; it < kIterations; ++it) {
        const f32 fov = rng.range(radians(20), radians(120));
        const f32 aspect = rng.range(0.5f, 3.0f);
        const f32 zn = rng.range(0.01f, 1.0f);
        const f32 zf = zn + rng.range(10.0f, 1000.0f);
        const mat4 dxm_zo = to_m(dx::XMMatrixPerspectiveFovRH(fov, aspect, zn, zf));

        const mat4 zo = perspective_rh_zo(fov, aspect, zn, zf);
        ORACLE_CHECK(close(zo, dxm_zo, Tol{1e-5f, 2e-6f}), print("ours", zo), print("dxm", dxm_zo));

        const mat4 no = perspective_rh_no(fov, aspect, zn, zf);
        const mat4 dxm_no = zo_to_no() * dxm_zo;
        ORACLE_CHECK(close(no, dxm_no, Tol{1e-5f, 2e-6f}), print("ours", no), print("dxm remapped", dxm_no));

        // GL clip-space contract: near -> -1, far -> +1 (zo: 0, 1). Tolerance in NDC units.
        ORACLE_CHECK(close(ndc(no, {0, 0, -zn}).z, -1.0f, Tol{1e-4f, 0}));
        ORACLE_CHECK(close(ndc(no, {0, 0, -zf}).z, 1.0f, Tol{1e-3f, 0}));
        ORACLE_CHECK(close(ndc(zo, {0, 0, -zn}).z, 0.0f, Tol{1e-4f, 0}));
        ORACLE_CHECK(close(ndc(zo, {0, 0, -zf}).z, 1.0f, Tol{1e-4f, 0}));

        // A random point inside the frustum lands at the same NDC x/y in both.
        const f32 d = rng.range(zn, zf);
        const vec3 pv{rng.range(-1, 1) * d, rng.range(-1, 1) * d, -d};
        const vec3 a = ndc(no, pv), b = ndc(dxm_no, pv);
        ORACLE_CHECK(close(a.xy(), b.xy(), Tol{1e-5f, 1e-5f}));
    }
}

TEST_CASE("oracle: ortho_rh_no vs remapped XMMatrixOrthographicOffCenterRH") {
    Rng rng(0x5EED0051);
    for (int it = 0; it < kIterations; ++it) {
        const f32 l = rng.range(-100, 0), r = l + rng.range(1, 200);
        const f32 b = rng.range(-100, 0), t = b + rng.range(1, 200);
        const f32 zn = rng.range(-10, 10), zf = zn + rng.range(1, 500);
        const mat4 ours = ortho_rh_no(l, r, b, t, zn, zf);
        const mat4 theirs = zo_to_no() * to_m(dx::XMMatrixOrthographicOffCenterRH(l, r, b, t, zn, zf));
        ORACLE_CHECK(close(ours, theirs, Tol{1e-5f, 1e-5f}), print("ours", ours), print("dxm remapped", theirs));
        ORACLE_CHECK(close(ndc(ours, {l, b, -zn}), vec3{-1, -1, -1}, Tol{1e-4f, 0}));
        ORACLE_CHECK(close(ndc(ours, {r, t, -zf}), vec3{1, 1, 1}, Tol{1e-4f, 0}));
    }
}

// ================================================================ bounds

TEST_CASE("oracle: aabb3 contains / overlaps / merge vs DirectX::BoundingBox") {
    Rng rng(0x5EED0060);
    auto box = [&](f32 r) {
        const vec3 a = rng.v3(r), b = rng.v3(r);
        return aabb3{min(a, b), max(a, b)};
    };
    auto to_bb = [](const aabb3& a) {
        dx::BoundingBox bb;
        dx::BoundingBox::CreateFromPoints(bb, xm(a.min), xm(a.max));
        return bb;
    };
    for (int it = 0; it < kIterations; ++it) {
        const aabb3 a = box(10), b = box(10);
        const dx::BoundingBox ba = to_bb(a), bb = to_bb(b);
        ORACLE_CHECK(a.overlaps(b) == ba.Intersects(bb));
        const vec3 p = rng.v3(12);
        ORACLE_CHECK(a.contains(p) == (ba.Contains(xm(p)) != dx::DISJOINT));
        ORACLE_CHECK(close(a.center(), vec3{ba.Center.x, ba.Center.y, ba.Center.z}, kExact));
        ORACLE_CHECK(close(a.extents(), vec3{ba.Extents.x, ba.Extents.y, ba.Extents.z}, kExact));
        dx::BoundingBox m;
        dx::BoundingBox::CreateMerged(m, ba, bb);
        const aabb3 ours = merge(a, b);
        ORACLE_CHECK(close(ours.center(), vec3{m.Center.x, m.Center.y, m.Center.z}, kExact));
        ORACLE_CHECK(close(ours.extents(), vec3{m.Extents.x, m.Extents.y, m.Extents.z}, kExact));
    }
}
