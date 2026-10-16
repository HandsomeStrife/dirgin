/* =====================================================================
 * dmath.c  --  Implementations for dmath.h (vec3 normalize, mat4, quat).
 * Column-major, right-handed. Portable C, no platform dependencies.
 * ===================================================================== */
#include "dmath.h"

dvec3 dvec3_normalize(dvec3 a) {
    float len = dvec3_length(a);
    if (len <= 1e-20f) return dvec3_make(0.0f, 0.0f, 0.0f);
    float inv = 1.0f / len;
    return dvec3_scale(a, inv);
}

/* column-major accessor: (row r, col c) -> index c*4 + r */
#define M(mat, r, c) ((mat).m[(c) * 4 + (r)])

dmat4 dmat4_identity(void) {
    dmat4 r = { { 0 } };
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

dmat4 dmat4_mul(dmat4 a, dmat4 b) {
    dmat4 r;
    for (int c = 0; c < 4; c++) {
        for (int rr = 0; rr < 4; rr++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += M(a, rr, k) * M(b, k, c);
            M(r, rr, c) = s;
        }
    }
    return r;
}

dmat4 dmat4_transpose(dmat4 a) {
    dmat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            M(r, rr, c) = M(a, c, rr);
    return r;
}

dmat4 dmat4_translation(dvec3 t) {
    dmat4 r = dmat4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

dmat4 dmat4_scaling(dvec3 s) {
    dmat4 r = dmat4_identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

dmat4 dmat4_rotation(dvec3 axis, float radians) {
    dvec3 a = dvec3_normalize(axis);
    float c = cosf(radians), s = sinf(radians), ic = 1.0f - c;
    dmat4 r = dmat4_identity();
    M(r, 0, 0) = c + a.x * a.x * ic;
    M(r, 1, 0) = a.y * a.x * ic + a.z * s;
    M(r, 2, 0) = a.z * a.x * ic - a.y * s;
    M(r, 0, 1) = a.x * a.y * ic - a.z * s;
    M(r, 1, 1) = c + a.y * a.y * ic;
    M(r, 2, 1) = a.z * a.y * ic + a.x * s;
    M(r, 0, 2) = a.x * a.z * ic + a.y * s;
    M(r, 1, 2) = a.y * a.z * ic - a.x * s;
    M(r, 2, 2) = c + a.z * a.z * ic;
    return r;
}

dvec4 dmat4_mul_vec4(dmat4 a, dvec4 v) {
    dvec4 r;
    r.x = M(a, 0, 0) * v.x + M(a, 0, 1) * v.y + M(a, 0, 2) * v.z + M(a, 0, 3) * v.w;
    r.y = M(a, 1, 0) * v.x + M(a, 1, 1) * v.y + M(a, 1, 2) * v.z + M(a, 1, 3) * v.w;
    r.z = M(a, 2, 0) * v.x + M(a, 2, 1) * v.y + M(a, 2, 2) * v.z + M(a, 2, 3) * v.w;
    r.w = M(a, 3, 0) * v.x + M(a, 3, 1) * v.y + M(a, 3, 2) * v.z + M(a, 3, 3) * v.w;
    return r;
}

dvec3 dmat4_transform_point(dmat4 a, dvec3 p) {
    dvec4 r = dmat4_mul_vec4(a, dvec4_make(p.x, p.y, p.z, 1.0f));
    if (r.w != 0.0f && r.w != 1.0f) {
        float inv = 1.0f / r.w;
        return dvec3_make(r.x * inv, r.y * inv, r.z * inv);
    }
    return dvec3_make(r.x, r.y, r.z);
}

dvec3 dmat4_transform_dir(dmat4 a, dvec3 d) {
    dvec4 r = dmat4_mul_vec4(a, dvec4_make(d.x, d.y, d.z, 0.0f));
    return dvec3_make(r.x, r.y, r.z);
}

dmat4 dmat4_perspective(float fovy_radians, float aspect, float znear, float zfar) {
    dmat4 r = { { 0 } };
    float f = 1.0f / tanf(fovy_radians * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

dmat4 dmat4_look_at(dvec3 eye, dvec3 center, dvec3 up) {
    dvec3 f = dvec3_normalize(dvec3_sub(center, eye));
    dvec3 s = dvec3_normalize(dvec3_cross(f, up));
    dvec3 u = dvec3_cross(s, f);
    dmat4 r = dmat4_identity();
    M(r, 0, 0) = s.x;  M(r, 0, 1) = s.y;  M(r, 0, 2) = s.z;
    M(r, 1, 0) = u.x;  M(r, 1, 1) = u.y;  M(r, 1, 2) = u.z;
    M(r, 2, 0) = -f.x; M(r, 2, 1) = -f.y; M(r, 2, 2) = -f.z;
    M(r, 0, 3) = -dvec3_dot(s, eye);
    M(r, 1, 3) = -dvec3_dot(u, eye);
    M(r, 2, 3) =  dvec3_dot(f, eye);
    return r;
}

/* ---- quaternion ------------------------------------------------------ */
dquat dquat_from_axis_angle(dvec3 axis, float radians) {
    dvec3 a = dvec3_normalize(axis);
    float h = radians * 0.5f, s = sinf(h);
    dquat q = { a.x * s, a.y * s, a.z * s, cosf(h) };
    return q;
}

dquat dquat_mul(dquat a, dquat b) {
    dquat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

dquat dquat_normalize(dquat q) {
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 1e-20f) return dquat_identity();
    float inv = 1.0f / len;
    dquat r = { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
    return r;
}

dvec3 dquat_rotate_vec3(dquat q, dvec3 v) {
    /* v' = v + 2*cross(qv, cross(qv, v) + w*v) */
    dvec3 qv = dvec3_make(q.x, q.y, q.z);
    dvec3 t  = dvec3_scale(dvec3_cross(qv, v), 2.0f);
    return dvec3_add(v, dvec3_add(dvec3_scale(t, q.w), dvec3_cross(qv, t)));
}

dmat4 dquat_to_mat4(dquat q) {
    dquat n = dquat_normalize(q);
    float x = n.x, y = n.y, z = n.z, w = n.w;
    dmat4 r = dmat4_identity();
    M(r, 0, 0) = 1.0f - 2.0f * (y * y + z * z);
    M(r, 1, 0) = 2.0f * (x * y + z * w);
    M(r, 2, 0) = 2.0f * (x * z - y * w);
    M(r, 0, 1) = 2.0f * (x * y - z * w);
    M(r, 1, 1) = 1.0f - 2.0f * (x * x + z * z);
    M(r, 2, 1) = 2.0f * (y * z + x * w);
    M(r, 0, 2) = 2.0f * (x * z + y * w);
    M(r, 1, 2) = 2.0f * (y * z - x * w);
    M(r, 2, 2) = 1.0f - 2.0f * (x * x + y * y);
    return r;
}

dquat dquat_slerp(dquat a, dquat b, float t) {
    a = dquat_normalize(a);
    b = dquat_normalize(b);
    float cosom = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cosom < 0.0f) {       /* take the shorter arc */
        cosom = -cosom;
        b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
    }
    float s0, s1;
    if (cosom > 0.9995f) {    /* nearly parallel -> linear */
        s0 = 1.0f - t;
        s1 = t;
    } else {
        float omega = acosf(dm_clampf(cosom, -1.0f, 1.0f));
        float sinom = sinf(omega);
        s0 = sinf((1.0f - t) * omega) / sinom;
        s1 = sinf(t * omega) / sinom;
    }
    dquat r = { s0 * a.x + s1 * b.x, s0 * a.y + s1 * b.y,
                s0 * a.z + s1 * b.z, s0 * a.w + s1 * b.w };
    return dquat_normalize(r);
}
