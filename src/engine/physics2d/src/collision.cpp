#include "collision.h"

#include "mce/assert.h"

namespace mce::physics::detail {

namespace {

constexpr u32 kMaxManifoldPoints = 2;

void finalize_polygon(Shape& s) {
    // Normals and centroid (triangle fan about v[0] for precision).
    for (u32 i = 0; i < s.count; ++i) {
        const u32 i2 = i + 1 < s.count ? i + 1 : 0;
        s.n[i] = math::normalize(cross(s.v[i2] - s.v[i], 1.0f));
    }
    vec2 c{};
    f32 area = 0.0f;
    const vec2 origin = s.v[0];
    for (u32 i = 1; i + 1 < s.count; ++i) {
        const vec2 e1 = s.v[i] - origin;
        const vec2 e2 = s.v[i + 1] - origin;
        const f32 a = 0.5f * cross(e1, e2);
        c += a * (1.0f / 3.0f) * (e1 + e2);
        area += a;
    }
    s.centroid = c / area + origin;
}

// Gift-wrapping hull with welding (Box2D 2.x polygon Set). Output is CCW.
bool compute_hull(const vec2* in, u32 count, Shape& out) {
    if (count < 3 || count > kMaxPolygonVertices) return false;
    vec2 ps[kMaxPolygonVertices];
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        if (!std::isfinite(in[i].x) || !std::isfinite(in[i].y)) return false;
        bool unique = true;
        for (u32 j = 0; j < n; ++j)
            if (dist_sq(in[i], ps[j]) < (0.5f * kLinearSlop) * (0.5f * kLinearSlop)) {
                unique = false;
                break;
            }
        if (unique) ps[n++] = in[i];
    }
    if (n < 3) return false;

    u32 i0 = 0;
    for (u32 i = 1; i < n; ++i)
        if (ps[i].x > ps[i0].x || (ps[i].x == ps[i0].x && ps[i].y < ps[i0].y)) i0 = i;

    u32 hull[kMaxPolygonVertices];
    u32 m = 0;
    u32 ih = i0;
    for (;;) {
        if (m == kMaxPolygonVertices) return false;
        hull[m] = ih;
        u32 ie = 0;
        for (u32 j = 1; j < n; ++j) {
            if (ie == ih) {
                ie = j;
                continue;
            }
            const vec2 r = ps[ie] - ps[hull[m]];
            const vec2 v = ps[j] - ps[hull[m]];
            const f32 c = cross(r, v);
            if (c < 0.0f) ie = j;
            if (c == 0.0f && math::length_sq(v) > math::length_sq(r)) ie = j;
        }
        ++m;
        ih = ie;
        if (ie == i0) break;
    }
    if (m < 3) return false;

    out.count = m;
    out.radius = 0.0f;
    for (u32 i = 0; i < m; ++i) out.v[i] = ps[hull[i]];
    for (u32 i = 0; i < m; ++i) {
        const u32 i2 = i + 1 < m ? i + 1 : 0;
        if (math::length_sq(out.v[i2] - out.v[i]) < kEpsilon * kEpsilon) return false;
    }
    // Reject (near-)zero area.
    f32 area2 = 0.0f;
    for (u32 i = 1; i + 1 < m; ++i) area2 += cross(out.v[i] - out.v[0], out.v[i + 1] - out.v[0]);
    if (!(area2 > kLinearSlop * kLinearSlop)) return false;
    finalize_polygon(out);
    return true;
}

}  // namespace

bool build_shape(const ColliderShape& in, Shape& out) {
    out = Shape{};
    switch (in.type) {
        case ShapeType::Circle:
            if (!(in.radius > 0.0f) || !std::isfinite(in.radius)) return false;
            out.count = 1;
            out.v[0] = in.offset;
            out.n[0] = {};
            out.centroid = in.offset;
            out.radius = in.radius;
            return true;
        case ShapeType::Box: {
            if (!(in.half.x > 0.0f) || !(in.half.y > 0.0f) || !std::isfinite(in.half.x) || !std::isfinite(in.half.y))
                return false;
            const Rot q = Rot::of(in.rotation);
            const vec2 h = in.half;
            const vec2 corners[4] = {{-h.x, -h.y}, {h.x, -h.y}, {h.x, h.y}, {-h.x, h.y}};
            const vec2 normals[4] = {{0.0f, -1.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}};
            out.count = 4;
            for (u32 i = 0; i < 4; ++i) {
                out.v[i] = mul(q, corners[i]) + in.offset;
                out.n[i] = mul(q, normals[i]);
            }
            out.centroid = in.offset;
            out.radius = 0.0f;
            return true;
        }
        case ShapeType::Convex: return compute_hull(in.vertices, in.vertex_count, out);
    }
    return false;
}

MassData compute_mass(const Shape& s, f32 density) {
    MassData md;
    if (s.is_circle()) {
        const f32 r2 = s.radius * s.radius;
        md.mass = density * math::kPi * r2;
        md.center = s.v[0];
        md.inertia = md.mass * (0.5f * r2 + dot(s.v[0], s.v[0]));
        return md;
    }
    // Box2D 2.x polygon mass (radius 0).
    vec2 center{};
    f32 area = 0.0f;
    f32 inertia = 0.0f;
    const vec2 origin = s.v[0];
    constexpr f32 kInv3 = 1.0f / 3.0f;
    for (u32 i = 0; i < s.count; ++i) {
        const vec2 e1 = s.v[i] - origin;
        const vec2 e2 = (i + 1 < s.count ? s.v[i + 1] : s.v[0]) - origin;
        const f32 d = cross(e1, e2);
        const f32 tri = 0.5f * d;
        area += tri;
        center += tri * kInv3 * (e1 + e2);
        const f32 intx2 = e1.x * e1.x + e2.x * e1.x + e2.x * e2.x;
        const f32 inty2 = e1.y * e1.y + e2.y * e1.y + e2.y * e2.y;
        inertia += (0.25f * kInv3 * d) * (intx2 + inty2);
    }
    md.mass = density * area;
    center = center * (1.0f / area);
    md.center = center + origin;
    md.inertia = density * inertia + md.mass * (dot(md.center, md.center) - dot(center, center));
    return md;
}

math::aabb2 compute_aabb(const Shape& s, const Xf& xf) {
    vec2 lo = mul(xf, s.v[0]);
    vec2 hi = lo;
    for (u32 i = 1; i < s.count; ++i) {
        const vec2 p = mul(xf, s.v[i]);
        lo = math::min(lo, p);
        hi = math::max(hi, p);
    }
    const vec2 r{s.radius, s.radius};
    return {lo - r, hi + r};
}

// ---- manifolds ----------------------------------------------------------------------------------

namespace {

struct ClipVertex {
    vec2 v;
    u32 id;
};

constexpr u32 make_id(u32 index_a, u32 index_b, u32 type_a, u32 type_b) {
    return index_a | (index_b << 8) | (type_a << 16) | (type_b << 24);
}
constexpr u32 kVertex = 0;
constexpr u32 kFace = 1;
constexpr u32 flip_id(u32 id) {
    return ((id >> 8) & 0xFF) | ((id & 0xFF) << 8) | (((id >> 24) & 0xFF) << 16) | (((id >> 16) & 0xFF) << 24);
}

void collide_circles(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb, Manifold& m) {
    m.count = 0;
    const vec2 pa = mul(xa, a.v[0]);
    const vec2 pb = mul(xb, b.v[0]);
    const f32 r = a.radius + b.radius;
    if (dist_sq(pa, pb) > r * r) return;
    m.type = Manifold::Type::Circles;
    m.local_point = a.v[0];
    m.local_normal = {};
    m.count = 1;
    m.points[0].local_point = b.v[0];
    m.points[0].id = 0;
}

void collide_polygon_circle(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb, Manifold& m) {
    m.count = 0;
    const vec2 c = mul(xb, b.v[0]);
    const vec2 cl = mul_t(xa, c);
    u32 normal_index = 0;
    f32 separation = -3.4e38f;
    const f32 radius = a.radius + b.radius;
    for (u32 i = 0; i < a.count; ++i) {
        const f32 s = dot(a.n[i], cl - a.v[i]);
        if (s > radius) return;
        if (s > separation) {
            separation = s;
            normal_index = i;
        }
    }
    const vec2 v1 = a.v[normal_index];
    const vec2 v2 = a.v[normal_index + 1 < a.count ? normal_index + 1 : 0];
    m.type = Manifold::Type::FaceA;
    m.points[0].local_point = b.v[0];
    m.points[0].id = 0;
    if (separation < kEpsilon) {
        m.count = 1;
        m.local_normal = a.n[normal_index];
        m.local_point = 0.5f * (v1 + v2);
        return;
    }
    const f32 u1 = dot(cl - v1, v2 - v1);
    const f32 u2 = dot(cl - v2, v1 - v2);
    if (u1 <= 0.0f) {
        if (dist_sq(cl, v1) > radius * radius) return;
        m.count = 1;
        m.local_normal = math::normalize(cl - v1);
        m.local_point = v1;
    } else if (u2 <= 0.0f) {
        if (dist_sq(cl, v2) > radius * radius) return;
        m.count = 1;
        m.local_normal = math::normalize(cl - v2);
        m.local_point = v2;
    } else {
        const vec2 face_center = 0.5f * (v1 + v2);
        if (dot(cl - face_center, a.n[normal_index]) > radius) return;
        m.count = 1;
        m.local_normal = a.n[normal_index];
        m.local_point = face_center;
    }
}

// Max separation of poly2 from the edges of poly1 (in poly2's frame).
f32 find_max_separation(u32& edge_index, const Shape& p1, const Xf& x1, const Shape& p2, const Xf& x2) {
    const Xf xf = mul_t(x2, x1);
    u32 best = 0;
    f32 max_sep = -3.4e38f;
    for (u32 i = 0; i < p1.count; ++i) {
        const vec2 n = mul(xf.q, p1.n[i]);
        const vec2 v1 = mul(xf, p1.v[i]);
        f32 si = 3.4e38f;
        for (u32 j = 0; j < p2.count; ++j) {
            const f32 sij = dot(n, p2.v[j] - v1);
            if (sij < si) si = sij;
        }
        if (si > max_sep) {
            max_sep = si;
            best = i;
        }
    }
    edge_index = best;
    return max_sep;
}

void find_incident_edge(ClipVertex c[2], const Shape& p1, const Xf& x1, u32 edge1, const Shape& p2, const Xf& x2) {
    const vec2 normal1 = mul_t(x2.q, mul(x1.q, p1.n[edge1]));
    u32 index = 0;
    f32 min_dot = 3.4e38f;
    for (u32 i = 0; i < p2.count; ++i) {
        const f32 d = dot(normal1, p2.n[i]);
        if (d < min_dot) {
            min_dot = d;
            index = i;
        }
    }
    const u32 i1 = index;
    const u32 i2 = i1 + 1 < p2.count ? i1 + 1 : 0;
    c[0] = {mul(x2, p2.v[i1]), make_id(edge1, i1, kFace, kVertex)};
    c[1] = {mul(x2, p2.v[i2]), make_id(edge1, i2, kFace, kVertex)};
}

u32 clip_segment_to_line(ClipVertex out[2], const ClipVertex in[2], vec2 normal, f32 offset, u32 vertex_index_a) {
    u32 n = 0;
    const f32 d0 = dot(normal, in[0].v) - offset;
    const f32 d1 = dot(normal, in[1].v) - offset;
    if (d0 <= 0.0f) out[n++] = in[0];
    if (d1 <= 0.0f) out[n++] = in[1];
    if (d0 * d1 < 0.0f) {
        const f32 interp = d0 / (d0 - d1);
        out[n].v = in[0].v + interp * (in[1].v - in[0].v);
        out[n].id = make_id(vertex_index_a, (in[0].id >> 8) & 0xFF, kVertex, kFace);
        ++n;
    }
    return n;
}

void collide_polygons(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb, Manifold& m) {
    m.count = 0;
    const f32 total_radius = a.radius + b.radius;
    u32 edge_a = 0;
    const f32 sep_a = find_max_separation(edge_a, a, xa, b, xb);
    if (sep_a > total_radius) return;
    u32 edge_b = 0;
    const f32 sep_b = find_max_separation(edge_b, b, xb, a, xa);
    if (sep_b > total_radius) return;

    const Shape* p1 = nullptr;
    const Shape* p2 = nullptr;
    Xf x1, x2;
    u32 edge1 = 0;
    bool flip = false;
    constexpr f32 kTol = 0.1f * kLinearSlop;
    if (sep_b > sep_a + kTol) {
        p1 = &b;
        p2 = &a;
        x1 = xb;
        x2 = xa;
        edge1 = edge_b;
        m.type = Manifold::Type::FaceB;
        flip = true;
    } else {
        p1 = &a;
        p2 = &b;
        x1 = xa;
        x2 = xb;
        edge1 = edge_a;
        m.type = Manifold::Type::FaceA;
        flip = false;
    }

    ClipVertex incident[2];
    find_incident_edge(incident, *p1, x1, edge1, *p2, x2);

    const u32 iv1 = edge1;
    const u32 iv2 = edge1 + 1 < p1->count ? edge1 + 1 : 0;
    vec2 v11 = p1->v[iv1];
    vec2 v12 = p1->v[iv2];
    const vec2 local_tangent = math::normalize(v12 - v11);
    const vec2 local_normal = cross(local_tangent, 1.0f);
    const vec2 plane_point = 0.5f * (v11 + v12);
    const vec2 tangent = mul(x1.q, local_tangent);
    const vec2 normal = cross(tangent, 1.0f);
    v11 = mul(x1, v11);
    v12 = mul(x1, v12);
    const f32 front_offset = dot(normal, v11);
    const f32 side_offset1 = -dot(tangent, v11) + total_radius;
    const f32 side_offset2 = dot(tangent, v12) + total_radius;

    ClipVertex clip1[2];
    ClipVertex clip2[2];
    if (clip_segment_to_line(clip1, incident, -tangent, side_offset1, iv1) < 2) return;
    if (clip_segment_to_line(clip2, clip1, tangent, side_offset2, iv2) < 2) return;

    m.local_normal = local_normal;
    m.local_point = plane_point;
    u32 count = 0;
    for (u32 i = 0; i < kMaxManifoldPoints; ++i) {
        const f32 separation = dot(normal, clip2[i].v) - front_offset;
        if (separation <= total_radius) {
            ManifoldPoint& cp = m.points[count];
            cp.local_point = mul_t(x2, clip2[i].v);
            cp.id = flip ? flip_id(clip2[i].id) : clip2[i].id;
            ++count;
        }
    }
    m.count = count;
}

}  // namespace

void collide(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb, Manifold& m) {
    if (a.is_circle()) {
        MCE_ASSERT_MSG(b.is_circle(), "collide: polygon must be shape A");
        collide_circles(a, xa, b, xb, m);
    } else if (b.is_circle()) {
        collide_polygon_circle(a, xa, b, xb, m);
    } else {
        collide_polygons(a, xa, b, xb, m);
    }
}

void collide_any(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb, Manifold& m, bool* swapped) {
    const bool swap = a.is_circle() && !b.is_circle();
    if (swapped) *swapped = swap;
    if (swap) collide(b, xb, a, xa, m);
    else collide(a, xa, b, xb, m);
}

void world_manifold(const Manifold& m, const Xf& xa, f32 ra, const Xf& xb, f32 rb, WorldManifold& out) {
    if (m.count == 0) return;
    switch (m.type) {
        case Manifold::Type::Circles: {
            out.normal = {1.0f, 0.0f};
            const vec2 pa = mul(xa, m.local_point);
            const vec2 pb = mul(xb, m.points[0].local_point);
            if (dist_sq(pa, pb) > kEpsilon * kEpsilon) out.normal = math::normalize(pb - pa);
            const vec2 ca = pa + ra * out.normal;
            const vec2 cb = pb - rb * out.normal;
            out.points[0] = 0.5f * (ca + cb);
            out.separations[0] = dot(cb - ca, out.normal);
            break;
        }
        case Manifold::Type::FaceA: {
            out.normal = mul(xa.q, m.local_normal);
            const vec2 plane = mul(xa, m.local_point);
            for (u32 i = 0; i < m.count; ++i) {
                const vec2 clip = mul(xb, m.points[i].local_point);
                const vec2 ca = clip + (ra - dot(clip - plane, out.normal)) * out.normal;
                const vec2 cb = clip - rb * out.normal;
                out.points[i] = 0.5f * (ca + cb);
                out.separations[i] = dot(cb - ca, out.normal);
            }
            break;
        }
        case Manifold::Type::FaceB: {
            out.normal = mul(xb.q, m.local_normal);
            const vec2 plane = mul(xb, m.local_point);
            for (u32 i = 0; i < m.count; ++i) {
                const vec2 clip = mul(xa, m.points[i].local_point);
                const vec2 cb = clip + (rb - dot(clip - plane, out.normal)) * out.normal;
                const vec2 ca = clip - ra * out.normal;
                out.points[i] = 0.5f * (ca + cb);
                out.separations[i] = dot(ca - cb, out.normal);
            }
            out.normal = -out.normal;
            break;
        }
    }
}

// ---- GJK distance (Box2D 2.x shape, no cache) ---------------------------------------------------------

namespace {

u32 support(const Shape& s, vec2 d) {
    u32 best = 0;
    f32 best_v = dot(s.v[0], d);
    for (u32 i = 1; i < s.count; ++i) {
        const f32 v = dot(s.v[i], d);
        if (v > best_v) {
            best = i;
            best_v = v;
        }
    }
    return best;
}

struct SimplexVertex {
    vec2 wa, wb, w;
    f32 a;
    u32 ia, ib;
};

struct Simplex {
    SimplexVertex v[3];
    u32 count;

    vec2 search_direction() const {
        if (count == 1) return -v[0].w;
        const vec2 e12 = v[1].w - v[0].w;
        const f32 sgn = cross(e12, -v[0].w);
        return sgn > 0.0f ? cross(1.0f, e12) : cross(e12, 1.0f);
    }

    void witness(vec2& pa, vec2& pb) const {
        switch (count) {
            case 1:
                pa = v[0].wa;
                pb = v[0].wb;
                break;
            case 2:
                pa = v[0].a * v[0].wa + v[1].a * v[1].wa;
                pb = v[0].a * v[0].wb + v[1].a * v[1].wb;
                break;
            default:
                pa = v[0].a * v[0].wa + v[1].a * v[1].wa + v[2].a * v[2].wa;
                pb = pa;
                break;
        }
    }

    void solve2() {
        const vec2 w1 = v[0].w, w2 = v[1].w;
        const vec2 e12 = w2 - w1;
        const f32 d12_2 = -dot(w1, e12);
        if (d12_2 <= 0.0f) {
            v[0].a = 1.0f;
            count = 1;
            return;
        }
        const f32 d12_1 = dot(w2, e12);
        if (d12_1 <= 0.0f) {
            v[1].a = 1.0f;
            count = 1;
            v[0] = v[1];
            return;
        }
        const f32 inv = 1.0f / (d12_1 + d12_2);
        v[0].a = d12_1 * inv;
        v[1].a = d12_2 * inv;
        count = 2;
    }

    void solve3() {
        const vec2 w1 = v[0].w, w2 = v[1].w, w3 = v[2].w;
        const vec2 e12 = w2 - w1;
        const f32 d12_1 = dot(w2, e12);
        const f32 d12_2 = -dot(w1, e12);
        const vec2 e13 = w3 - w1;
        const f32 d13_1 = dot(w3, e13);
        const f32 d13_2 = -dot(w1, e13);
        const vec2 e23 = w3 - w2;
        const f32 d23_1 = dot(w3, e23);
        const f32 d23_2 = -dot(w2, e23);
        const f32 n123 = cross(e12, e13);
        const f32 d123_1 = n123 * cross(w2, w3);
        const f32 d123_2 = n123 * cross(w3, w1);
        const f32 d123_3 = n123 * cross(w1, w2);

        if (d12_2 <= 0.0f && d13_2 <= 0.0f) {
            v[0].a = 1.0f;
            count = 1;
            return;
        }
        if (d12_1 > 0.0f && d12_2 > 0.0f && d123_3 <= 0.0f) {
            const f32 inv = 1.0f / (d12_1 + d12_2);
            v[0].a = d12_1 * inv;
            v[1].a = d12_2 * inv;
            count = 2;
            return;
        }
        if (d13_1 > 0.0f && d13_2 > 0.0f && d123_2 <= 0.0f) {
            const f32 inv = 1.0f / (d13_1 + d13_2);
            v[0].a = d13_1 * inv;
            v[2].a = d13_2 * inv;
            count = 2;
            v[1] = v[2];
            return;
        }
        if (d12_1 <= 0.0f && d23_2 <= 0.0f) {
            v[1].a = 1.0f;
            count = 1;
            v[0] = v[1];
            return;
        }
        if (d13_1 <= 0.0f && d23_1 <= 0.0f) {
            v[2].a = 1.0f;
            count = 1;
            v[0] = v[2];
            return;
        }
        if (d23_1 > 0.0f && d23_2 > 0.0f && d123_1 <= 0.0f) {
            const f32 inv = 1.0f / (d23_1 + d23_2);
            v[1].a = d23_1 * inv;
            v[2].a = d23_2 * inv;
            count = 2;
            v[0] = v[2];
            return;
        }
        const f32 inv = 1.0f / (d123_1 + d123_2 + d123_3);
        v[0].a = d123_1 * inv;
        v[1].a = d123_2 * inv;
        v[2].a = d123_3 * inv;
        count = 3;
    }
};

}  // namespace

DistanceResult distance(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb) {
    Simplex s{};
    s.count = 1;
    s.v[0].ia = 0;
    s.v[0].ib = 0;
    s.v[0].wa = mul(xa, a.v[0]);
    s.v[0].wb = mul(xb, b.v[0]);
    s.v[0].w = s.v[0].wb - s.v[0].wa;
    s.v[0].a = 1.0f;

    constexpr u32 kMaxIters = 20;
    u32 save_a[3], save_b[3];
    for (u32 iter = 0; iter < kMaxIters; ++iter) {
        const u32 save_count = s.count;
        for (u32 i = 0; i < save_count; ++i) {
            save_a[i] = s.v[i].ia;
            save_b[i] = s.v[i].ib;
        }
        if (s.count == 2) s.solve2();
        else if (s.count == 3) s.solve3();
        if (s.count == 3) break;

        const vec2 d = s.search_direction();
        if (math::length_sq(d) < kEpsilon * kEpsilon) break;

        SimplexVertex& vx = s.v[s.count];
        vx.ia = support(a, mul_t(xa.q, -d));
        vx.wa = mul(xa, a.v[vx.ia]);
        vx.ib = support(b, mul_t(xb.q, d));
        vx.wb = mul(xb, b.v[vx.ib]);
        vx.w = vx.wb - vx.wa;

        bool duplicate = false;
        for (u32 i = 0; i < save_count; ++i)
            if (vx.ia == save_a[i] && vx.ib == save_b[i]) {
                duplicate = true;
                break;
            }
        if (duplicate) break;
        ++s.count;
    }

    DistanceResult r;
    s.witness(r.point_a, r.point_b);
    r.distance = math::length(r.point_b - r.point_a);
    const f32 ra = a.radius, rb = b.radius;
    if (r.distance > ra + rb && r.distance > kEpsilon) {
        r.distance -= ra + rb;
        const vec2 n = math::normalize(r.point_b - r.point_a);
        r.point_a += ra * n;
        r.point_b -= rb * n;
    } else {
        const vec2 p = 0.5f * (r.point_a + r.point_b);
        r.point_a = p;
        r.point_b = p;
        r.distance = 0.0f;
    }
    return r;
}

// ---- rays & casts -------------------------------------------------------------------------------

bool raycast(const Shape& s, const Xf& xf, vec2 p1, vec2 p2, f32& fraction, vec2& normal) {
    if (s.is_circle()) {
        const vec2 position = mul(xf, s.v[0]);
        const vec2 sv = p1 - position;
        const f32 b = dot(sv, sv) - s.radius * s.radius;
        if (b < 0.0f) return false;  // starts inside
        const vec2 r = p2 - p1;
        const f32 c = dot(sv, r);
        const f32 rr = dot(r, r);
        const f32 sigma = c * c - rr * b;
        if (sigma < 0.0f || rr < kEpsilon) return false;
        f32 t = -(c + std::sqrt(sigma));
        if (t < 0.0f || t > rr) return false;
        t /= rr;
        fraction = t;
        normal = math::normalize(sv + t * r);
        return true;
    }
    const vec2 l1 = mul_t(xf, p1);
    const vec2 d = mul_t(xf.q, p2 - p1);
    f32 lower = 0.0f, upper = 1.0f;
    i32 index = -1;
    for (u32 i = 0; i < s.count; ++i) {
        const f32 numerator = dot(s.n[i], s.v[i] - l1);
        const f32 denominator = dot(s.n[i], d);
        if (denominator == 0.0f) {
            if (numerator < 0.0f) return false;
        } else if (denominator < 0.0f && numerator < lower * denominator) {
            lower = numerator / denominator;
            index = static_cast<i32>(i);
        } else if (denominator > 0.0f && numerator < upper * denominator) {
            upper = numerator / denominator;
        }
        if (upper < lower) return false;
    }
    if (index < 0) return false;
    fraction = lower;
    normal = mul(xf.q, s.n[index]);
    return true;
}

CastResult shape_cast(const Shape& a, const Xf& xa, vec2 delta, const Shape& b, const Xf& xb, f32 target) {
    CastResult out;
    constexpr u32 kMaxIters = 32;
    const f32 tolerance = 0.25f * kLinearSlop;
    f32 t = 0.0f;
    vec2 last_n;
    for (u32 iter = 0; iter < kMaxIters; ++iter) {
        const Xf xt{xa.p + t * delta, xa.q};
        const DistanceResult d = distance(a, xt, b, xb);
        if (d.distance <= 0.0f) {
            // Touching/overlapping: at the start there is no usable normal; later the advance
            // landed exactly on contact, so the previous iteration's normal is the answer.
            if (iter == 0) out.kind = CastResult::Kind::Overlap;
            else {
                out.kind = CastResult::Kind::Hit;
                out.t = t;
                out.normal = -last_n;
                out.point = d.point_b;
            }
            return out;
        }
        const vec2 n = math::normalize(d.point_b - d.point_a);  // a -> b
        last_n = n;
        const f32 approach = dot(delta, n);
        if (approach <= kEpsilon) return out;  // separating: distance only grows (convexity)
        if (d.distance < target + tolerance) {
            out.kind = CastResult::Kind::Hit;
            out.t = t;
            out.normal = -n;
            out.point = d.point_b;
            return out;
        }
        t += (d.distance - target) / approach;
        if (t >= 1.0f) return out;
    }
    return out;  // did not converge: treat as miss (deterministic either way)
}

bool test_point(const Shape& s, const Xf& xf, vec2 p) {
    const vec2 l = mul_t(xf, p);
    if (s.is_circle()) return dist_sq(l, s.v[0]) <= s.radius * s.radius;
    for (u32 i = 0; i < s.count; ++i)
        if (dot(s.n[i], l - s.v[i]) > s.radius) return false;
    return true;
}

}  // namespace mce::physics::detail
