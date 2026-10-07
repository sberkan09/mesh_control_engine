#include "mce/math/math.h"

namespace mce::math {

quat slerp(quat a, quat b, f32 t) {
    f32 cos_theta = dot(a, b);
    if (cos_theta < 0.0f) {  // take the shortest arc
        b = {-b.x, -b.y, -b.z, -b.w};
        cos_theta = -cos_theta;
    }
    if (cos_theta > 0.9995f) {
        return normalize(quat{lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)});
    }
    const f32 theta = std::acos(cos_theta);
    const f32 sin_theta = std::sin(theta);
    const f32 wa = std::sin((1.0f - t) * theta) / sin_theta;
    const f32 wb = std::sin(t * theta) / sin_theta;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

mat4 rotation(quat q) {
    const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    mat4 m = mat4::identity();
    m.c[0][0] = 1.0f - 2.0f * (yy + zz);
    m.c[0][1] = 2.0f * (xy + wz);
    m.c[0][2] = 2.0f * (xz - wy);
    m.c[1][0] = 2.0f * (xy - wz);
    m.c[1][1] = 1.0f - 2.0f * (xx + zz);
    m.c[1][2] = 2.0f * (yz + wx);
    m.c[2][0] = 2.0f * (xz + wy);
    m.c[2][1] = 2.0f * (yz - wx);
    m.c[2][2] = 1.0f - 2.0f * (xx + yy);
    return m;
}

mat4 trs(vec3 t, quat r, vec3 s) {
    mat4 m = rotation(r);
    for (usize i = 0; i < 3; ++i) {
        m.c[0][i] *= s.x;
        m.c[1][i] *= s.y;
        m.c[2][i] *= s.z;
    }
    m.c[3][0] = t.x;
    m.c[3][1] = t.y;
    m.c[3][2] = t.z;
    return m;
}

namespace {
// Cofactors of the flattened column-major matrix (MESA gluInvertMatrix layout).
void cofactors(const f32* m, f32* inv) {
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
}
}  // namespace

f32 determinant(const mat4& mat) {
    f32 inv[16];
    const f32* m = mat.data();
    cofactors(m, inv);
    return m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
}

mat4 inverse(const mat4& mat, bool* ok) {
    f32 inv[16];
    const f32* m = mat.data();
    cofactors(m, inv);
    const f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        if (ok) *ok = false;
        return mat4::identity();
    }
    const f32 inv_det = 1.0f / det;
    mat4 r;
    f32* out = &r.c[0][0];
    for (usize i = 0; i < 16; ++i) out[i] = inv[i] * inv_det;
    if (ok) *ok = true;
    return r;
}

mat4 inverse_rigid(const mat4& m) {
    mat4 r = mat4::identity();
    for (usize i = 0; i < 3; ++i)
        for (usize j = 0; j < 3; ++j) r.c[i][j] = m.c[j][i];
    const vec3 t{m.c[3][0], m.c[3][1], m.c[3][2]};
    const vec3 rt = transform_dir(r, t);
    r.c[3][0] = -rt.x;
    r.c[3][1] = -rt.y;
    r.c[3][2] = -rt.z;
    return r;
}

mat4 look_at_rh(vec3 eye, vec3 at, vec3 up) {
    const vec3 f = normalize(at - eye);
    const vec3 s = normalize(cross(f, up));
    const vec3 u = cross(s, f);
    mat4 m = mat4::identity();
    m.c[0][0] = s.x;
    m.c[1][0] = s.y;
    m.c[2][0] = s.z;
    m.c[0][1] = u.x;
    m.c[1][1] = u.y;
    m.c[2][1] = u.z;
    m.c[0][2] = -f.x;
    m.c[1][2] = -f.y;
    m.c[2][2] = -f.z;
    m.c[3][0] = -dot(s, eye);
    m.c[3][1] = -dot(u, eye);
    m.c[3][2] = dot(f, eye);
    return m;
}

mat4 perspective_rh_no(f32 fov_y_rad, f32 aspect, f32 zn, f32 zf) {
    const f32 f = 1.0f / std::tan(fov_y_rad * 0.5f);
    mat4 m;
    m.c[0][0] = f / aspect;
    m.c[1][1] = f;
    m.c[2][2] = (zf + zn) / (zn - zf);
    m.c[2][3] = -1.0f;
    m.c[3][2] = (2.0f * zf * zn) / (zn - zf);
    return m;
}

mat4 perspective_rh_zo(f32 fov_y_rad, f32 aspect, f32 zn, f32 zf) {
    const f32 f = 1.0f / std::tan(fov_y_rad * 0.5f);
    mat4 m;
    m.c[0][0] = f / aspect;
    m.c[1][1] = f;
    m.c[2][2] = zf / (zn - zf);
    m.c[2][3] = -1.0f;
    m.c[3][2] = (zn * zf) / (zn - zf);
    return m;
}

mat4 ortho_rh_no(f32 left, f32 right, f32 bottom, f32 top, f32 zn, f32 zf) {
    mat4 m = mat4::identity();
    m.c[0][0] = 2.0f / (right - left);
    m.c[1][1] = 2.0f / (top - bottom);
    m.c[2][2] = -2.0f / (zf - zn);
    m.c[3][0] = -(right + left) / (right - left);
    m.c[3][1] = -(top + bottom) / (top - bottom);
    m.c[3][2] = -(zf + zn) / (zf - zn);
    return m;
}

bool near(const mat4& a, const mat4& b, f32 eps) {
    for (usize i = 0; i < 4; ++i)
        for (usize j = 0; j < 4; ++j)
            if (!near(a.c[i][j], b.c[i][j], eps)) return false;
    return true;
}

bool near(vec3 a, vec3 b, f32 eps) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

bool near(quat a, quat b, f32 eps) {
    const bool same = near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps) && near(a.w, b.w, eps);
    const bool flipped = near(a.x, -b.x, eps) && near(a.y, -b.y, eps) && near(a.z, -b.z, eps) && near(a.w, -b.w, eps);
    return same || flipped;
}

}  // namespace mce::math
