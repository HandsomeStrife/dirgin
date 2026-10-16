/* =====================================================================
 * test_math.c  --  Sanity + oracle-parity checks for dmath and vu0.
 * Covers the math identities the engine relies on and the VU0 ops whose
 * exact behaviour geometry decode depends on (dest mask, broadcast,
 * accumulator MADD, ITOF4 = /16 dequant, cross via OPMULA/OPMSUB, clip).
 * ===================================================================== */
#include "dmath.h"
#include "vu0.h"
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int feq(float a, float b) { return fabsf(a - b) <= 1e-4f * (1.0f + fabsf(b)); }

int main(void) {
    /* ---- mat4: identity * v = v; translation moves a point ---------- */
    dmat4 I = dmat4_identity();
    dvec3 p = dmat4_transform_point(I, dvec3_make(1, 2, 3));
    CHECK(feq(p.x, 1) && feq(p.y, 2) && feq(p.z, 3), "identity transform");

    dmat4 T = dmat4_translation(dvec3_make(10, -5, 2));
    p = dmat4_transform_point(T, dvec3_make(1, 1, 1));
    CHECK(feq(p.x, 11) && feq(p.y, -4) && feq(p.z, 3), "translation");

    /* a translation applied to a direction (w=0) must NOT move it */
    dvec3 d = dmat4_transform_dir(T, dvec3_make(1, 0, 0));
    CHECK(feq(d.x, 1) && feq(d.y, 0) && feq(d.z, 0), "direction ignores translation");

    /* ---- mat4 mul: T(a) * T(b) = T(a+b) ----------------------------- */
    dmat4 TA = dmat4_translation(dvec3_make(1, 2, 3));
    dmat4 TB = dmat4_translation(dvec3_make(4, 5, 6));
    dmat4 TC = dmat4_mul(TA, TB);
    p = dmat4_transform_point(TC, dvec3_make(0, 0, 0));
    CHECK(feq(p.x, 5) && feq(p.y, 7) && feq(p.z, 9), "mat mul compose translations");

    /* ---- rotation 90deg about Z maps +X -> +Y ----------------------- */
    dmat4 Rz = dmat4_rotation(dvec3_make(0, 0, 1), (float)M_PI / 2.0f);
    p = dmat4_transform_point(Rz, dvec3_make(1, 0, 0));
    CHECK(feq(p.x, 0) && feq(p.y, 1), "rotate +X about Z");

    /* ---- quaternion matches matrix rotation ------------------------- */
    dquat qz = dquat_from_axis_angle(dvec3_make(0, 0, 1), (float)M_PI / 2.0f);
    dvec3 qp = dquat_rotate_vec3(qz, dvec3_make(1, 0, 0));
    CHECK(feq(qp.x, 0) && feq(qp.y, 1), "quat rotate +X about Z");
    dmat4 Rq = dquat_to_mat4(qz);
    dvec3 mp = dmat4_transform_point(Rq, dvec3_make(1, 0, 0));
    CHECK(feq(mp.x, qp.x) && feq(mp.y, qp.y) && feq(mp.z, qp.z), "quat_to_mat4 parity");

    /* slerp endpoints */
    dquat qa = dquat_identity();
    dquat qs0 = dquat_slerp(qa, qz, 0.0f), qs1 = dquat_slerp(qa, qz, 1.0f);
    CHECK(feq(qs0.w, 1), "slerp t=0 == a");
    CHECK(feq(qs1.x, qz.x) && feq(qs1.w, qz.w), "slerp t=1 == b");

    /* ---- VU0: masked multiply (only x,z written) -------------------- */
    float a[4] = { 2, 3, 4, 5 }, b[4] = { 10, 10, 10, 10 };
    float r[4] = { -1, -1, -1, -1 };
    vu0_mul(r, a, b, VU0_X | VU0_Z);
    CHECK(feq(r[0], 20) && feq(r[2], 40), "vu0_mul writes masked lanes");
    CHECK(feq(r[1], -1) && feq(r[3], -1), "vu0_mul preserves unmasked lanes");

    /* ---- VU0: broadcast ---------------------------------------------- */
    float rb[4] = { 0, 0, 0, 0 };
    vu0_mul_bc(rb, a, b, VU0_LANE_X, VU0_XYZW);  /* b.x = 10 broadcast */
    CHECK(feq(rb[0], 20) && feq(rb[1], 30) && feq(rb[2], 40) && feq(rb[3], 50),
          "vu0_mul_bc broadcast lane");

    /* ---- VU0: accumulator MADD (acc + a*b) -------------------------- */
    vu0_acc acc;
    vu0_mula(&acc, a, b, VU0_XYZW);     /* acc = a*b = {20,30,40,50} */
    float rm[4] = { 0, 0, 0, 0 };
    float ones[4] = { 1, 1, 1, 1 };
    vu0_madd(rm, &acc, a, ones, VU0_XYZW);  /* acc + a*1 = {22,33,44,55} */
    CHECK(feq(rm[0], 22) && feq(rm[1], 33) && feq(rm[2], 44) && feq(rm[3], 55),
          "vu0_madd uses accumulator");

    /* ---- VU0: ITOF4 = divide by 16 (MOD int8 dequant) --------------- */
    int32_t qi[4] = { 16, 32, -48, 8 };
    float   qf[4] = { 0, 0, 0, 0 };
    vu0_itof(qf, qi, 4, VU0_XYZW);
    CHECK(feq(qf[0], 1) && feq(qf[1], 2) && feq(qf[2], -3) && feq(qf[3], 0.5f),
          "vu0_itof fracbits=4 divides by 16");
    int32_t back[4] = { 0, 0, 0, 0 };
    vu0_ftoi(back, qf, 4, VU0_XYZW);
    CHECK(back[0] == 16 && back[1] == 32 && back[2] == -48 && back[3] == 8,
          "vu0_ftoi inverts vu0_itof");

    /* ---- VU0: cross product two ways -------------------------------- */
    float cx[3], ca[3] = { 1, 0, 0 }, cb[3] = { 0, 1, 0 };
    vu0_cross(cx, ca, cb);
    CHECK(feq(cx[0], 0) && feq(cx[1], 0) && feq(cx[2], 1), "vu0_cross X x Y = Z");

    float va[4] = { 1, 0, 0, 0 }, vb[4] = { 0, 1, 0, 0 }, vd[4] = { 0, 0, 0, 0 };
    vu0_acc oacc;
    vu0_opmula(&oacc, va, vb);
    vu0_opmsub(vd, &oacc, va, vb);
    CHECK(feq(vd[0], 0) && feq(vd[1], 0) && feq(vd[2], 1),
          "vu0_opmula/opmsub == cross");

    /* ---- VU0: clip flags -------------------------------------------- */
    float fs[3] = { 2.0f, -2.0f, 0.0f };   /* x > |w|, y < -|w| */
    uint32_t clip = vu0_clip(fs, 1.0f, 0);
    CHECK((clip & 0x01) != 0, "clip: x beyond +w");
    CHECK((clip & 0x08) != 0, "clip: y below -w");
    CHECK((clip & 0x30) == 0, "clip: z within range");

    if (g_fail == 0) printf("test_math: ALL PASS\n");
    else             printf("test_math: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
