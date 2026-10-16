/* =====================================================================
 * dmath.h  --  Portable math library for the Dirge of Cerberus native
 * engine (purist port, native/engine/).
 *
 * Clean, readable, platform-independent C. NO guest memory, NO EE/VU0
 * intrinsics, NO raw instruction words. This is the math vocabulary the
 * reauthored game logic is written against; the EE/VU0 macro-op
 * semantics the original code expressed inline are translated into the
 * named vu0_* functions in vu0.h.
 *
 * Conventions:
 *   - Right-handed, column-major 4x4 matrices (OpenGL layout):
 *     element (row r, col c) lives at m[c*4 + r]. Translation is in the
 *     last column (m[12..14]). dmat4_mul(a,b) returns a*b.
 *   - Floats are ordinary IEEE-754 single precision. The PS2's non-IEEE
 *     float quirks (no NaN/Inf, overflow saturates, denormal flush) are
 *     NOT reproduced here; that is a deliberate purist-port choice (see
 *     DECOMP-METHOD.md "Floats"). The bit-exact quirk model lives in the
 *     oracle (port/cop2/vu0_macro.c) and is used only for validation.
 * ===================================================================== */
#ifndef DOC_DMATH_H
#define DOC_DMATH_H

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- types ----------------------------------------------------------- */
typedef struct { float x, y; }       dvec2;
typedef struct { float x, y, z; }    dvec3;
typedef struct { float x, y, z, w; } dvec4;
typedef struct { float x, y, z, w; } dquat;   /* x,y,z = imaginary, w = real */
typedef struct { float m[16]; }      dmat4;   /* column-major */

/* ---- scalar helpers -------------------------------------------------- */
static inline float dm_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float dm_lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* ---- dvec2 ----------------------------------------------------------- */
static inline dvec2 dvec2_make(float x, float y) { dvec2 r = { x, y }; return r; }
static inline dvec2 dvec2_add(dvec2 a, dvec2 b)   { return dvec2_make(a.x + b.x, a.y + b.y); }
static inline dvec2 dvec2_sub(dvec2 a, dvec2 b)   { return dvec2_make(a.x - b.x, a.y - b.y); }
static inline dvec2 dvec2_scale(dvec2 a, float s) { return dvec2_make(a.x * s, a.y * s); }
static inline float dvec2_dot(dvec2 a, dvec2 b)   { return a.x * b.x + a.y * b.y; }
static inline float dvec2_length(dvec2 a)         { return sqrtf(dvec2_dot(a, a)); }

/* ---- dvec3 ----------------------------------------------------------- */
static inline dvec3 dvec3_make(float x, float y, float z) { dvec3 r = { x, y, z }; return r; }
static inline dvec3 dvec3_add(dvec3 a, dvec3 b)   { return dvec3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline dvec3 dvec3_sub(dvec3 a, dvec3 b)   { return dvec3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline dvec3 dvec3_scale(dvec3 a, float s) { return dvec3_make(a.x * s, a.y * s, a.z * s); }
static inline float dvec3_dot(dvec3 a, dvec3 b)   { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline dvec3 dvec3_cross(dvec3 a, dvec3 b) {
    return dvec3_make(a.y * b.z - a.z * b.y,
                      a.z * b.x - a.x * b.z,
                      a.x * b.y - a.y * b.x);
}
static inline float dvec3_length(dvec3 a)         { return sqrtf(dvec3_dot(a, a)); }
static inline dvec3 dvec3_lerp(dvec3 a, dvec3 b, float t) {
    return dvec3_make(dm_lerpf(a.x, b.x, t), dm_lerpf(a.y, b.y, t), dm_lerpf(a.z, b.z, t));
}
dvec3 dvec3_normalize(dvec3 a);   /* zero vector -> zero vector (safe) */

/* ---- dvec4 ----------------------------------------------------------- */
static inline dvec4 dvec4_make(float x, float y, float z, float w) { dvec4 r = { x, y, z, w }; return r; }
static inline dvec4 dvec4_add(dvec4 a, dvec4 b)   { return dvec4_make(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
static inline dvec4 dvec4_sub(dvec4 a, dvec4 b)   { return dvec4_make(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
static inline dvec4 dvec4_scale(dvec4 a, float s) { return dvec4_make(a.x * s, a.y * s, a.z * s, a.w * s); }
static inline float dvec4_dot(dvec4 a, dvec4 b)   { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

/* ---- dmat4 ----------------------------------------------------------- */
dmat4 dmat4_identity(void);
dmat4 dmat4_mul(dmat4 a, dmat4 b);                 /* a * b                 */
dmat4 dmat4_transpose(dmat4 a);
dmat4 dmat4_translation(dvec3 t);
dmat4 dmat4_scaling(dvec3 s);
dmat4 dmat4_rotation(dvec3 axis, float radians);   /* axis need not be unit */
dvec4 dmat4_mul_vec4(dmat4 a, dvec4 v);
dvec3 dmat4_transform_point(dmat4 a, dvec3 p);     /* w = 1, perspective div */
dvec3 dmat4_transform_dir(dmat4 a, dvec3 d);       /* w = 0                 */
dmat4 dmat4_perspective(float fovy_radians, float aspect, float znear, float zfar);
dmat4 dmat4_look_at(dvec3 eye, dvec3 center, dvec3 up);

/* ---- dquat ----------------------------------------------------------- */
static inline dquat dquat_identity(void) { dquat q = { 0.0f, 0.0f, 0.0f, 1.0f }; return q; }
dquat dquat_from_axis_angle(dvec3 axis, float radians);
dquat dquat_mul(dquat a, dquat b);
dquat dquat_normalize(dquat q);
dvec3 dquat_rotate_vec3(dquat q, dvec3 v);
dmat4 dquat_to_mat4(dquat q);
dquat dquat_slerp(dquat a, dquat b, float t);

#ifdef __cplusplus
}
#endif
#endif /* DOC_DMATH_H */
