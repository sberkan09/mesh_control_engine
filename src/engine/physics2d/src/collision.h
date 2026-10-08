#pragma once
// Narrowphase + geometric queries (physics.md §3.2). Shapes the Box2D 2.x way: every collision
// shape is a convex polygon with a radius; a circle is the one-vertex case. Manifold, clipping,
// GJK distance and world-manifold code follow Box2D 2.x's collide/distance routines (Catto) — hand-written.

#include <cmath>

#include "mce/physics/components.h"

namespace mce::physics::detail {

using math::cross;
using math::dot;
using math::vec2;

// ---- solver constants (Box2D 2.x defaults; physics.md §3.2) ---------------------------------
inline constexpr f32 kLinearSlop = 0.005f;
inline constexpr f32 kPolygonRadius = 2.0f * kLinearSlop;  // unused by default: polygons are sharp
inline constexpr f32 kAabbMargin = 0.1f;
inline constexpr f32 kBaumgarte = 0.2f;
inline constexpr f32 kMaxLinearCorrection = 0.2f;
inline constexpr f32 kRestitutionThreshold = 1.0f;  // m/s
inline constexpr f32 kMaxLinearSpeed = 200.0f;      // m/s velocity clamp (tunneling guard)
inline constexpr f32 kMaxRotation = 0.5f * math::kPi;  // rad per step
inline constexpr f32 kEpsilon = 1.1920929e-7f;

inline vec2 cross(vec2 a, f32 s) { return {s * a.y, -s * a.x}; }
inline vec2 cross(f32 s, vec2 a) { return {-s * a.y, s * a.x}; }
inline vec2 abs(vec2 v) { return {std::fabs(v.x), std::fabs(v.y)}; }
inline f32 dist_sq(vec2 a, vec2 b) { return math::length_sq(a - b); }

struct Rot {
    f32 s = 0.0f, c = 1.0f;
    static Rot of(f32 angle) { return {std::sin(angle), std::cos(angle)}; }
};
inline vec2 mul(Rot q, vec2 v) { return {q.c * v.x - q.s * v.y, q.s * v.x + q.c * v.y}; }
inline vec2 mul_t(Rot q, vec2 v) { return {q.c * v.x + q.s * v.y, -q.s * v.x + q.c * v.y}; }
inline Rot mul_t(Rot a, Rot b) { return {a.c * b.s - a.s * b.c, a.c * b.c + a.s * b.s}; }

struct Xf {
    vec2 p;
    Rot q;
};
inline vec2 mul(const Xf& t, vec2 v) { return mul(t.q, v) + t.p; }
inline vec2 mul_t(const Xf& t, vec2 v) { return mul_t(t.q, v - t.p); }
// B expressed in A's frame.
inline Xf mul_t(const Xf& a, const Xf& b) { return {mul_t(a.q, b.p - a.p), mul_t(a.q, b.q)}; }

// Convex polygon with radius, in body-local space. count == 1 is a circle centred at v[0].
struct Shape {
    vec2 v[kMaxPolygonVertices];
    vec2 n[kMaxPolygonVertices];
    vec2 centroid;
    u32 count = 0;
    f32 radius = 0.0f;
    bool is_circle() const { return count == 1; }
};

struct MassData {
    f32 mass = 0.0f;
    vec2 center;
    f32 inertia = 0.0f;  // about the body origin
};

// Builds the internal shape from the authored one. Returns false for degenerate input
// (non-positive sizes, < 3 hull points, collinear points).
bool build_shape(const ColliderShape& in, Shape& out);
MassData compute_mass(const Shape& s, f32 density);
math::aabb2 compute_aabb(const Shape& s, const Xf& xf);

// ---- manifolds --------------------------------------------------------------------------------
struct ManifoldPoint {
    vec2 local_point;
    u32 id = 0;  // feature id (indexA, indexB, typeA, typeB) — warm-start key for M3.5
};
struct Manifold {
    enum class Type : u8 { Circles, FaceA, FaceB };
    ManifoldPoint points[2];
    vec2 local_normal;
    vec2 local_point;
    Type type = Type::Circles;
    u32 count = 0;
};
struct WorldManifold {
    vec2 normal;  // from A to B
    vec2 points[2];
    f32 separations[2] = {};
};

// Dispatches on shape kinds. Precondition: A is not a circle when B is a polygon (callers order
// the pair so the polygon comes first; collide_any handles the swap).
void collide(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb, Manifold& m);
// Any order; *swapped is set when the manifold was computed as (b, a).
void collide_any(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb, Manifold& m, bool* swapped);
void world_manifold(const Manifold& m, const Xf& xa, f32 ra, const Xf& xb, f32 rb, WorldManifold& out);

// ---- distance / casts ----------------------------------------------------------------------
struct DistanceResult {
    vec2 point_a;
    vec2 point_b;
    f32 distance = 0.0f;
};
// GJK distance between radius-inflated convex shapes (radii applied).
DistanceResult distance(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb);

// Ray p1 + t (p2 - p1), t in [0, 1]. Returns false on miss or when p1 starts inside the shape.
bool raycast(const Shape& s, const Xf& xf, vec2 p1, vec2 p2, f32& fraction, vec2& normal);

struct CastResult {
    enum class Kind : u8 { Miss, Hit, Overlap };
    Kind kind = Kind::Miss;
    f32 t = 1.0f;     // fraction of `delta` travelled before stopping at `target` distance
    vec2 normal;      // from b toward a (the surface normal the mover hits)
    vec2 point;       // on b's surface
};
// Conservative advancement of `a` translating by `delta` against static `b`, stopping at
// separation `target`. A shape already closer than `target` only counts when moving toward b.
CastResult shape_cast(const Shape& a, const Xf& xa, vec2 delta, const Shape& b, const Xf& xb, f32 target);

// Point inside (radius-inflated) shape.
bool test_point(const Shape& s, const Xf& xf, vec2 p);

}  // namespace mce::physics::detail
