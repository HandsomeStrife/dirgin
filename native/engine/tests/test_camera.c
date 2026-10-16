/* =====================================================================
 * test_camera.c  --  Unit + oracle-parity test for the reauthored camera
 * / view subsystem.
 *
 * Two layers (DECOMP-METHOD.md section 6):
 *   1. BEHAVIOUR: the clean state API (position/fov/angles/set) stores and
 *      reads what the KerberosCamera bridge expects; the rigid inverse
 *      really inverts; the view matrix maps the eye to the origin.
 *   2. ORACLE PARITY: byte-accurate transcriptions of the decompiled
 *      originals -- the three Euler axis builders (FUN_00259928/990/f8),
 *      their composition through the FUN_0026125c 4x4 multiply (as
 *      FUN_00259c50 orders it), the FUN_00263968 projection, and the
 *      FUN_002d9cd0 matrix->Euler extraction -- are built in a plain
 *      row-major float[16] exactly as the guest, and diffed element-wise
 *      against the clean dmat4 build. Because the guest's row-major-row-
 *      vector storage is byte-identical to the engine's column-major-
 *      column-vector storage of the same transform (see camera.c layout
 *      note), the assertion is clean.m[k] == oracle[k] with NO transpose,
 *      which pins both the math and the float-index map.
 *
 * EPSILON: 1e-4f absolute. The single-axis builders and the projection
 * perform element-for-element identical float ops on both sides, so they
 * match to the bit (diff ~0). The composed view rotation differs only in
 * multiply association -- clean uses dmat4_mul's accumulation order, the
 * oracle uses the guest's row-combination order -- which is non-associative
 * in IEEE float and perturbs entries at the ~1e-7 relative level. The
 * matrix->Euler path shares asinf/atan2f but divides by cos(Y); on the
 * non-degenerate synthetic inputs used here that stays well under 1e-5.
 * 1e-4 sits far above this FP noise and far below any real logic error, so
 * a true mismatch (wrong index, wrong sign, wrong order) still trips it.
 * The PS2's non-IEEE float quirks are deliberately NOT modelled (purist
 * port; DECOMP-METHOD.md "Floats") -- the camera math is not quirk-
 * dependent, so IEEE parity within epsilon is the correct bar.
 * ===================================================================== */
#include "camera.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define EPS 1.0e-4f
static int approx(float a, float b) { return fabsf(a - b) <= EPS; }

static void check_mat(const char *where, const dmat4 *clean, const float *oracle) {
    for (int k = 0; k < 16; k++)
        CHECK(approx(clean->m[k], oracle[k]),
              "%s: m[%d] clean=%.7g oracle=%.7g (oracle mismatch)",
              where, k, (double)clean->m[k], (double)oracle[k]);
}

/* ===================================================================== *
 * Byte-accurate reference: the decompiled originals in row-major float[16].
 * ===================================================================== */

/* FUN_002613b8: full 4x4 identity. */
static void ref_identity(float *o) {
    memset(o, 0, 16 * sizeof(float));
    o[0] = o[5] = o[10] = o[15] = 1.0f;
}

/* FUN_00259928 (X): 3x3 identity + [5]=c [6]=s [9]=-s [10]=c. */
static void ref_rot_x(float *o, float a) {
    float c = cosf(a), s = sinf(a);
    ref_identity(o);
    o[5] = c; o[6] = s; o[9] = -s; o[10] = c;
}
/* FUN_00259990 (Y): [0]=c [2]=-s [8]=s [10]=c. */
static void ref_rot_y(float *o, float a) {
    float c = cosf(a), s = sinf(a);
    ref_identity(o);
    o[0] = c; o[2] = -s; o[8] = s; o[10] = c;
}
/* FUN_002599f8 (Z): [0]=c [1]=s [4]=-s [5]=c. */
static void ref_rot_z(float *o, float a) {
    float c = cosf(a), s = sinf(a);
    ref_identity(o);
    o[0] = c; o[1] = s; o[4] = -s; o[5] = c;
}
static void ref_axis(float *o, DocAxis ax, float a) {
    if (ax == DOC_AXIS_X) ref_rot_x(o, a);
    else if (ax == DOC_AXIS_Y) ref_rot_y(o, a);
    else ref_rot_z(o, a);
}

/* FUN_0026125c as FUN_00259c50 invokes it: out = lhs * rhs in ROW terms,
 * out[i*4+j] = sum_k lhs[i*4+k]*rhs[k*4+j]. */
static void ref_rowmul(float *out, const float *lhs, const float *rhs) {
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += lhs[i * 4 + k] * rhs[k * 4 + j];
            t[i * 4 + j] = s;
        }
    memcpy(out, t, sizeof t);
}

/* FUN_00259c50: A = rot(order0); A = A*rot(order1); A = A*rot(order2). */
static void ref_view_rotation(float *out, const DocAxis order[3], const dvec3 ang) {
    float ax_ang[3] = { ang.x, ang.y, ang.z };
    float A[16], T[16];
    ref_axis(A, order[0], ax_ang[order[0]]);
    ref_axis(T, order[1], ax_ang[order[1]]); ref_rowmul(A, A, T);
    ref_axis(T, order[2], ax_ang[order[2]]); ref_rowmul(A, A, T);
    memcpy(out, A, 16 * sizeof(float));
}

/* FUN_00263968 projection into row-major float[16]. */
static void ref_projection(float *o, float w, float h, float d, float n, float f) {
    ref_identity(o);
    if (n < 1.0f) n = 1.0f;
    o[0]  = (d + d) / w;
    o[5]  = (d + d) / h;
    o[10] = (f + n) / (f - n);
    o[11] = 1.0f;
    o[14] = (f * n * -2.0f) / (f - n);
    o[15] = 0.0f;
}

/* FUN_002d9cd0: matrix -> Euler (radians, before the 4096/2pi scale). */
static void ref_matrix_to_angles(const float *p, float out[3]) {
    float ax, ay, az;
    ay = asinf(p[8]);
    float cay = cosf(ay);
    ax = -atan2f(p[9], p[10]);
    if (cay == 0.0f) { ax = -atan2f(p[6], p[5]); az = 0.0f; }
    else {
        float t = asinf(p[4] / cay);
        az = -t;
        if (p[0] < 0.0f) az = 3.14159265358979f - az;
    }
    out[0] = ax; out[1] = ay; out[2] = az;
}

int main(void) {
    /* ---- 1. single-axis builder parity (3 angles each) --------------- */
    const float angs[3] = { 0.3f, -1.1f, 2.4f };
    for (int i = 0; i < 3; i++) {
        float o[16];
        dmat4 cx = doc_camera_rot_x(angs[i]); ref_rot_x(o, angs[i]); check_mat("rot_x", &cx, o);
        dmat4 cy = doc_camera_rot_y(angs[i]); ref_rot_y(o, angs[i]); check_mat("rot_y", &cy, o);
        dmat4 cz = doc_camera_rot_z(angs[i]); ref_rot_z(o, angs[i]); check_mat("rot_z", &cz, o);
    }

    /* ---- 2. composed view rotation parity (two orders) --------------- */
    {
        DocCamera c; doc_camera_init(&c);
        doc_camera_set_angles(&c, dvec3_make(0.4f, -0.7f, 1.3f));
        dmat4 clean = doc_camera_view_rotation(&c);
        float o[16]; ref_view_rotation(o, c.order, c.angles);
        check_mat("view_rotation XYZ", &clean, o);

        c.order[0] = DOC_AXIS_Z; c.order[1] = DOC_AXIS_X; c.order[2] = DOC_AXIS_Y;
        clean = doc_camera_view_rotation(&c);
        ref_view_rotation(o, c.order, c.angles);
        check_mat("view_rotation ZXY", &clean, o);
    }

    /* ---- 3. projection parity (incl. the near<1 clamp) --------------- */
    {
        dmat4 clean = doc_camera_projection(640.0f, 448.0f, 500.0f, 0.2f, 4096.0f);
        float o[16]; ref_projection(o, 640.0f, 448.0f, 500.0f, 0.2f, 4096.0f);
        check_mat("projection (near clamped)", &clean, o);

        clean = doc_camera_projection(512.0f, 448.0f, 300.0f, 4.0f, 2000.0f);
        ref_projection(o, 512.0f, 448.0f, 300.0f, 4.0f, 2000.0f);
        check_mat("projection", &clean, o);
    }

    /* ---- 4. matrix -> Euler parity ----------------------------------- */
    {
        DocCamera c; doc_camera_init(&c);
        doc_camera_set_angles(&c, dvec3_make(0.35f, -0.6f, 1.0f));
        dmat4 R = doc_camera_view_rotation(&c);
        dvec3 clean = doc_camera_matrix_to_angles(R);
        float o[3]; ref_matrix_to_angles(R.m, o);
        CHECK(approx(clean.x, o[0]), "m2euler x clean=%.7g oracle=%.7g", (double)clean.x, (double)o[0]);
        CHECK(approx(clean.y, o[1]), "m2euler y clean=%.7g oracle=%.7g", (double)clean.y, (double)o[1]);
        CHECK(approx(clean.z, o[2]), "m2euler z clean=%.7g oracle=%.7g", (double)clean.z, (double)o[2]);
    }

    /* ---- 5. state setters/getters (KerberosCamera bridge) ------------ */
    {
        DocCamera c; doc_camera_init(&c);
        c.eye = dvec3_make(1.0f, 2.0f, 3.0f);
        dvec3 p = doc_camera_get_position(&c);
        CHECK(p.x == 1.0f && p.y == 2.0f && p.z == 3.0f, "getpos wrong");

        doc_camera_move(&c, 0.5f, -1.0f, 2.0f);           /* KerberosCamera.move */
        CHECK(approx(c.eye.x, 1.5f) && approx(c.eye.y, 1.0f) && approx(c.eye.z, 5.0f),
              "move wrong (%.3g,%.3g,%.3g)", (double)c.eye.x, (double)c.eye.y, (double)c.eye.z);

        doc_camera_set_fov(&c, 1.2f);                     /* KerberosCamera.setfov */
        CHECK(approx(c.fov, 1.2f), "setfov wrong");
        CHECK(c.proj_dist > 0.0f, "proj_dist should be positive after setfov");
    }

    /* ---- 6. rigid inverse really inverts ----------------------------- */
    {
        DocCamera c; doc_camera_init(&c);
        doc_camera_set_angles(&c, dvec3_make(0.4f, -0.7f, 1.3f));
        c.eye = dvec3_make(10.0f, -5.0f, 20.0f);
        dmat4 world = doc_camera_view_rotation(&c);
        world.m[12] = c.eye.x; world.m[13] = c.eye.y; world.m[14] = c.eye.z;
        dmat4 inv = doc_camera_rigid_inverse(world);
        dmat4 id = dmat4_mul(inv, world);
        for (int k = 0; k < 16; k++) {
            float want = (k % 5 == 0) ? 1.0f : 0.0f;   /* identity diagonal */
            CHECK(approx(id.m[k], want), "inverse*world not identity at m[%d]=%.6g", k, (double)id.m[k]);
        }
        /* the view matrix must send the eye to the camera origin */
        dmat4 view = doc_camera_view_matrix(&c);
        dvec3 at_origin = dmat4_transform_point(view, c.eye);
        CHECK(approx(at_origin.x, 0.0f) && approx(at_origin.y, 0.0f) && approx(at_origin.z, 0.0f),
              "view(eye) should be origin, got (%.4g,%.4g,%.4g)",
              (double)at_origin.x, (double)at_origin.y, (double)at_origin.z);
    }

    /* ---- 7. set(eye,target,up) looks toward the target --------------- */
    {
        DocCamera c; doc_camera_init(&c);
        dvec3 eye = dvec3_make(0.0f, 0.0f, 0.0f);
        dvec3 target = dvec3_make(0.0f, 0.0f, 10.0f);   /* straight down +Z */
        dvec3 up = dvec3_make(0.0f, 1.0f, 0.0f);
        doc_camera_set(&c, eye, target, up);
        dvec3 v[3];
        doc_camera_get_vectors(&c, v);
        dvec3 dir = dvec3_normalize(dvec3_sub(v[1], v[0]));
        CHECK(approx(dir.x, 0.0f) && approx(dir.y, 0.0f) && approx(dir.z, 1.0f),
              "set() forward should be +Z, got (%.4g,%.4g,%.4g)",
              (double)dir.x, (double)dir.y, (double)dir.z);
    }

    if (g_fail == 0) printf("test_camera: ALL PASS\n");
    else             printf("test_camera: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
