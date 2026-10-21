#pragma once
// mce::math — scalar-first math library (core.md §2.7, §3.4).
//
// BINDING CONVENTIONS (asserted by the oracle tests):
//   right-handed · Y-up · meters · radians · column vectors · column-major storage
//   mat4::c[column][row]; v' = M * v; translation lives in c[3].
//   GL clip space z ∈ [-1, 1] (perspective_rh_no); perspective_rh_zo gives z ∈ [0, 1].
//   quat is Hamilton, w-last storage {x, y, z, w}.

#include <cmath>

#include "mce/types.h"

namespace mce::math {

inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kTau = 2.0f * kPi;

constexpr f32 radians(f32 deg) { return deg * (kPi / 180.0f); }
constexpr f32 degrees(f32 rad) { return rad * (180.0f / kPi); }
template <class T>
constexpr T min(T a, T b) { return b < a ? b : a; }
template <class T>
constexpr T max(T a, T b) { return a < b ? b : a; }
template <class T>
constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (hi < v ? hi : v); }
constexpr f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

// ---------------------------------------------------------------- vectors
struct vec2 {
    f32 x = 0, y = 0;
    constexpr vec2() = default;
    constexpr vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
    constexpr explicit vec2(f32 s) : x(s), y(s) {}
    constexpr f32& operator[](usize i) { return (&x)[i]; }
    constexpr f32 operator[](usize i) const { return (&x)[i]; }
};

struct vec3 {
    f32 x = 0, y = 0, z = 0;
    constexpr vec3() = default;
    constexpr vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit vec3(f32 s) : x(s), y(s), z(s) {}
    constexpr vec3(vec2 v, f32 z_) : x(v.x), y(v.y), z(z_) {}
    constexpr f32& operator[](usize i) { return (&x)[i]; }
    constexpr f32 operator[](usize i) const { return (&x)[i]; }
    constexpr vec2 xy() const { return {x, y}; }
};

struct vec4 {
    f32 x = 0, y = 0, z = 0, w = 0;
    constexpr vec4() = default;
    constexpr vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr explicit vec4(f32 s) : x(s), y(s), z(s), w(s) {}
    constexpr vec4(vec3 v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    constexpr f32& operator[](usize i) { return (&x)[i]; }
    constexpr f32 operator[](usize i) const { return (&x)[i]; }
    constexpr vec3 xyz() const { return {x, y, z}; }
};

#define MCE_VEC_OPS(V, ...)                                                                              \
    constexpr V operator+(V a, V b) { return V{__VA_ARGS__(+)}; }                                        \
    constexpr V operator-(V a, V b) { return V{__VA_ARGS__(-)}; }                                        \
    constexpr V operator*(V a, V b) { return V{__VA_ARGS__(*)}; }                                        \
    constexpr V operator/(V a, V b) { return V{__VA_ARGS__(/)}; }                                        \
    constexpr V operator*(V a, f32 s) { return a * V(s); }                                               \
    constexpr V operator*(f32 s, V a) { return a * V(s); }                                               \
    constexpr V operator/(V a, f32 s) { return a / V(s); }                                               \
    constexpr V operator-(V a) { return V(0.0f) - a; }                                                   \
    constexpr V& operator+=(V& a, V b) { return a = a + b; }                                             \
    constexpr V& operator-=(V& a, V b) { return a = a - b; }                                             \
    constexpr V& operator*=(V& a, f32 s) { return a = a * s; }                                           \
    constexpr V& operator/=(V& a, f32 s) { return a = a / s; }

#define MCE_V2(op) a.x op b.x, a.y op b.y
#define MCE_V3(op) a.x op b.x, a.y op b.y, a.z op b.z
#define MCE_V4(op) a.x op b.x, a.y op b.y, a.z op b.z, a.w op b.w
MCE_VEC_OPS(vec2, MCE_V2)
MCE_VEC_OPS(vec3, MCE_V3)
MCE_VEC_OPS(vec4, MCE_V4)
#undef MCE_V2
#undef MCE_V3
#undef MCE_V4
#undef MCE_VEC_OPS

constexpr bool operator==(vec2 a, vec2 b) { return a.x == b.x && a.y == b.y; }
constexpr bool operator==(vec3 a, vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
constexpr bool operator==(vec4 a, vec4 b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

constexpr f32 dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr f32 dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr f32 dot(vec4 a, vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
constexpr f32 cross(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
constexpr vec3 cross(vec3 a, vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

inline f32 length(vec2 v) { return std::sqrt(dot(v, v)); }
inline f32 length(vec3 v) { return std::sqrt(dot(v, v)); }
inline f32 length(vec4 v) { return std::sqrt(dot(v, v)); }
constexpr f32 length_sq(vec2 v) { return dot(v, v); }
constexpr f32 length_sq(vec3 v) { return dot(v, v); }
// Zero-length input (including lengths that underflow to 0) returns the zero vector instead of
// NaN, matching XMVector*Normalize (oracle-tested), so a stray zero never poisons sim state.
inline vec2 normalize(vec2 v) {
    const f32 l = length(v);
    return l > 0.0f ? v / l : vec2{};
}
inline vec3 normalize(vec3 v) {
    const f32 l = length(v);
    return l > 0.0f ? v / l : vec3{};
}
inline vec4 normalize(vec4 v) {
    const f32 l = length(v);
    return l > 0.0f ? v / l : vec4{};
}
constexpr vec2 lerp(vec2 a, vec2 b, f32 t) { return a + (b - a) * t; }
constexpr vec3 lerp(vec3 a, vec3 b, f32 t) { return a + (b - a) * t; }
constexpr vec4 lerp(vec4 a, vec4 b, f32 t) { return a + (b - a) * t; }
constexpr vec2 min(vec2 a, vec2 b) { return {min(a.x, b.x), min(a.y, b.y)}; }
constexpr vec2 max(vec2 a, vec2 b) { return {max(a.x, b.x), max(a.y, b.y)}; }
constexpr vec3 min(vec3 a, vec3 b) { return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)}; }
constexpr vec3 max(vec3 a, vec3 b) { return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)}; }

// ---------------------------------------------------------------- quaternion
struct quat {
    f32 x = 0, y = 0, z = 0, w = 1;
    constexpr quat() = default;
    constexpr quat(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    static constexpr quat identity() { return {}; }
};

constexpr bool operator==(quat a, quat b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

// Hamilton product: (a * b) rotates by b first, then a.
constexpr quat operator*(quat a, quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
constexpr f32 dot(quat a, quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
constexpr quat conjugate(quat q) { return {-q.x, -q.y, -q.z, q.w}; }
// Zero quaternion in -> zero quaternion out (matches XMQuaternionNormalize), never NaN.
inline quat normalize(quat q) {
    const f32 l = std::sqrt(dot(q, q));
    if (!(l > 0.0f)) return {0, 0, 0, 0};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
inline quat quat_from_axis_angle(vec3 axis_unit, f32 angle) {
    const f32 s = std::sin(angle * 0.5f);
    return {axis_unit.x * s, axis_unit.y * s, axis_unit.z * s, std::cos(angle * 0.5f)};
}
constexpr vec3 rotate(quat q, vec3 v) {
    const vec3 u{q.x, q.y, q.z};
    const vec3 t = 2.0f * cross(u, v);
    return v + q.w * t + cross(u, t);
}
// Shortest-arc spherical interpolation; falls back to normalized lerp for nearly-equal inputs.
quat slerp(quat a, quat b, f32 t);

// ---------------------------------------------------------------- matrices
struct mat4 {
    f32 c[4][4] = {};  // c[column][row]

    static constexpr mat4 identity() {
        mat4 m;
        m.c[0][0] = m.c[1][1] = m.c[2][2] = m.c[3][3] = 1.0f;
        return m;
    }
    constexpr vec4 col(usize i) const { return {c[i][0], c[i][1], c[i][2], c[i][3]}; }
    constexpr vec4 row(usize i) const { return {c[0][i], c[1][i], c[2][i], c[3][i]}; }
    constexpr f32 operator()(usize row_i, usize col_i) const { return c[col_i][row_i]; }
    const f32* data() const { return &c[0][0]; }
};

constexpr mat4 mat4_identity() { return mat4::identity(); }

constexpr mat4 mul(const mat4& a, const mat4& b) {
    mat4 r;
    for (usize col = 0; col < 4; ++col)
        for (usize row = 0; row < 4; ++row) {
            f32 s = 0;
            for (usize k = 0; k < 4; ++k) s += a.c[k][row] * b.c[col][k];
            r.c[col][row] = s;
        }
    return r;
}
constexpr mat4 operator*(const mat4& a, const mat4& b) { return mul(a, b); }
constexpr vec4 operator*(const mat4& m, vec4 v) {
    return {m.c[0][0] * v.x + m.c[1][0] * v.y + m.c[2][0] * v.z + m.c[3][0] * v.w,
            m.c[0][1] * v.x + m.c[1][1] * v.y + m.c[2][1] * v.z + m.c[3][1] * v.w,
            m.c[0][2] * v.x + m.c[1][2] * v.y + m.c[2][2] * v.z + m.c[3][2] * v.w,
            m.c[0][3] * v.x + m.c[1][3] * v.y + m.c[2][3] * v.z + m.c[3][3] * v.w};
}
constexpr vec3 transform_point(const mat4& m, vec3 p) { return (m * vec4(p, 1.0f)).xyz(); }
constexpr vec3 transform_dir(const mat4& m, vec3 d) { return (m * vec4(d, 0.0f)).xyz(); }

constexpr mat4 transpose(const mat4& m) {
    mat4 r;
    for (usize i = 0; i < 4; ++i)
        for (usize j = 0; j < 4; ++j) r.c[i][j] = m.c[j][i];
    return r;
}

constexpr mat4 translation(vec3 t) {
    mat4 m = mat4::identity();
    m.c[3][0] = t.x;
    m.c[3][1] = t.y;
    m.c[3][2] = t.z;
    return m;
}
constexpr mat4 scaling(vec3 s) {
    mat4 m;
    m.c[0][0] = s.x;
    m.c[1][1] = s.y;
    m.c[2][2] = s.z;
    m.c[3][3] = 1.0f;
    return m;
}
mat4 rotation(quat q);
// T * R * S
mat4 trs(vec3 t, quat r, vec3 s);
// General inverse (cofactor expansion). Returns identity and sets *ok=false for singular input.
mat4 inverse(const mat4& m, bool* ok = nullptr);
// Inverse of a rigid transform (rotation + translation only).
mat4 inverse_rigid(const mat4& m);
f32 determinant(const mat4& m);

// Right-handed view matrix looking from eye toward at.
mat4 look_at_rh(vec3 eye, vec3 at, vec3 up);
// Right-handed perspective, GL clip space (z ∈ [-1, 1]).
mat4 perspective_rh_no(f32 fov_y_rad, f32 aspect, f32 zn, f32 zf);
// Right-handed perspective, zero-to-one clip space (z ∈ [0, 1]); matches XMMatrixPerspectiveFovRH.
mat4 perspective_rh_zo(f32 fov_y_rad, f32 aspect, f32 zn, f32 zf);
// Right-handed orthographic, GL clip space.
mat4 ortho_rh_no(f32 left, f32 right, f32 bottom, f32 top, f32 zn, f32 zf);

// ---------------------------------------------------------------- bounds & color
struct aabb2 {
    vec2 min, max;  // invariant: min <= max
    constexpr bool contains(vec2 p) const { return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y; }
    constexpr bool overlaps(const aabb2& o) const {
        return min.x <= o.max.x && o.min.x <= max.x && min.y <= o.max.y && o.min.y <= max.y;
    }
    constexpr vec2 center() const { return (min + max) * 0.5f; }
    constexpr vec2 extents() const { return (max - min) * 0.5f; }
};
struct aabb3 {
    vec3 min, max;
    constexpr bool contains(vec3 p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
    constexpr bool overlaps(const aabb3& o) const {
        return min.x <= o.max.x && o.min.x <= max.x && min.y <= o.max.y && o.min.y <= max.y && min.z <= o.max.z &&
               o.min.z <= max.z;
    }
    constexpr vec3 center() const { return (min + max) * 0.5f; }
    constexpr vec3 extents() const { return (max - min) * 0.5f; }
};
constexpr aabb2 merge(const aabb2& a, const aabb2& b) { return {min(a.min, b.min), max(a.max, b.max)}; }
constexpr aabb3 merge(const aabb3& a, const aabb3& b) { return {min(a.min, b.min), max(a.max, b.max)}; }

// Linear-space RGBA, 0..1.
struct Color {
    f32 r = 1, g = 1, b = 1, a = 1;
    static constexpr Color white() { return {1, 1, 1, 1}; }
    static constexpr Color black() { return {0, 0, 0, 1}; }
    static constexpr Color rgba8(u8 r, u8 g, u8 b, u8 a = 255) { return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f}; }
    constexpr vec4 to_vec4() const { return {r, g, b, a}; }
};
constexpr bool operator==(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

// Approximate comparisons for tests and tolerance checks.
inline bool near(f32 a, f32 b, f32 eps = 1e-5f) { return std::fabs(a - b) <= eps; }
bool near(const mat4& a, const mat4& b, f32 eps = 1e-5f);
bool near(vec3 a, vec3 b, f32 eps = 1e-5f);
bool near(quat a, quat b, f32 eps = 1e-5f);  // treats q and -q as equal

}  // namespace mce::math
