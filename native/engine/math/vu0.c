/* =====================================================================
 * vu0.c  --  Portable VU0 macro-op semantics (see vu0.h).
 * Translated from the oracle port/cop2/vu0_macro.c, with the raw-opcode
 * field decode replaced by explicit lane/mask parameters and PS2 float
 * quirks replaced by IEEE arithmetic. Dest-mask, broadcast, accumulator
 * and fixed-point (ITOF/FTOI) behaviour preserved exactly.
 * ===================================================================== */
#include "vu0.h"
#include <math.h>
#include <string.h>

#define LANE_ACTIVE(mask, L) (((mask) >> (L)) & 1u)

/* ---- FMAC write-to-vf ------------------------------------------------ */
void vu0_add(float d[4], const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = a[L] + b[L];
}
void vu0_sub(float d[4], const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = a[L] - b[L];
}
void vu0_mul(float d[4], const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = a[L] * b[L];
}
void vu0_max(float d[4], const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = a[L] >= b[L] ? a[L] : b[L];
}
void vu0_mini(float d[4], const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = a[L] <= b[L] ? a[L] : b[L];
}

void vu0_add_bc(float d[4], const float a[4], const float b[4], int bc, unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = a[L] + b[bc];
}
void vu0_sub_bc(float d[4], const float a[4], const float b[4], int bc, unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = a[L] - b[bc];
}
void vu0_mul_bc(float d[4], const float a[4], const float b[4], int bc, unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = a[L] * b[bc];
}

void vu0_madd(float d[4], const vu0_acc *acc, const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = acc->lane[L] + a[L] * b[L];
}
void vu0_msub(float d[4], const vu0_acc *acc, const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = acc->lane[L] - a[L] * b[L];
}

/* ---- FMAC write-to-ACC ----------------------------------------------- */
void vu0_mula(vu0_acc *acc, const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) acc->lane[L] = a[L] * b[L];
}
void vu0_adda(vu0_acc *acc, const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) acc->lane[L] = a[L] + b[L];
}
void vu0_madda(vu0_acc *acc, const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) acc->lane[L] += a[L] * b[L];
}
void vu0_msuba(vu0_acc *acc, const float a[4], const float b[4], unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) acc->lane[L] -= a[L] * b[L];
}
void vu0_mula_bc(vu0_acc *acc, const float a[4], const float b[4], int bc, unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) acc->lane[L] = a[L] * b[bc];
}
void vu0_madda_bc(vu0_acc *acc, const float a[4], const float b[4], int bc, unsigned m) {
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) acc->lane[L] += a[L] * b[bc];
}

/* ---- outer / cross product ------------------------------------------- *
 * Hardware VOPMULA/VOPMSUB:
 *   ACC.x = Fs.y*Ft.z   ACC.y = Fs.z*Ft.x   ACC.z = Fs.x*Ft.y
 *   Fd.x  = ACC.x - Fs.z*Ft.y   (= cross.x)
 *   Fd.y  = ACC.y - Fs.x*Ft.z   (= cross.y)
 *   Fd.z  = ACC.z - Fs.y*Ft.x   (= cross.z)
 * NOTE (oracle divergence): port/cop2/vu0_macro.c's ee_cop2_vopmsub
 * subtracts the SAME products VOPMULA added (Fs.y*Ft.z ...), which yields
 * zero instead of the cross product -- a latent bug in the oracle. We
 * implement the correct hardware semantics (opmula+opmsub == cross), so
 * this reauthored op is intentionally NOT bit-compatible with that oracle
 * routine. Flagged in the P4 report for the oracle to be fixed. */
void vu0_opmula(vu0_acc *acc, const float a[4], const float b[4]) {
    acc->lane[0] = a[1] * b[2];   /* x = a.y*b.z */
    acc->lane[1] = a[2] * b[0];   /* y = a.z*b.x */
    acc->lane[2] = a[0] * b[1];   /* z = a.x*b.y */
}
void vu0_opmsub(float d[4], const vu0_acc *acc, const float a[4], const float b[4]) {
    d[0] = acc->lane[0] - a[2] * b[1];   /* a.y*b.z - a.z*b.y */
    d[1] = acc->lane[1] - a[0] * b[2];   /* a.z*b.x - a.x*b.z */
    d[2] = acc->lane[2] - a[1] * b[0];   /* a.x*b.y - a.y*b.x */
}
void vu0_cross(float d[3], const float a[3], const float b[3]) {
    d[0] = a[1] * b[2] - a[2] * b[1];
    d[1] = a[2] * b[0] - a[0] * b[2];
    d[2] = a[0] * b[1] - a[1] * b[0];
}

/* ---- EFU scalar pipe ------------------------------------------------- */
float vu0_div(float num, float den)  { return num / den; }
float vu0_sqrt(float x)              { return sqrtf(fabsf(x)); }
float vu0_rsqrt(float num, float den){ return num / sqrtf(fabsf(den)); }

/* ---- fixed-point convert --------------------------------------------- */
static float frac_scale(int fracbits) {
    /* VU ITOF/FTOI use {0,4,12,15} fractional bits (scale 2^fracbits). */
    return (float)(1u << (unsigned)fracbits);
}
void vu0_ftoi(int32_t d[4], const float s[4], int fracbits, unsigned m) {
    double sc = (double)frac_scale(fracbits);
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) {
        double v = (double)s[L] * sc;
        if (v >= 2147483647.0)       d[L] = 0x7fffffff;
        else if (v <= -2147483648.0) d[L] = (int32_t)0x80000000;
        else                         d[L] = (int32_t)v;
    }
}
void vu0_itof(float d[4], const int32_t s[4], int fracbits, unsigned m) {
    double sc = (double)frac_scale(fracbits);
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = (float)((double)s[L] / sc);
}

/* ---- misc ------------------------------------------------------------ */
void vu0_mr32(float d[4], const float s[4], unsigned m) {
    float rot[4] = { s[1], s[2], s[3], s[0] };   /* x<-y, y<-z, z<-w, w<-x */
    for (int L = 0; L < 4; L++) if (LANE_ACTIVE(m, L)) d[L] = rot[L];
}

/* ---- frustum clip ---------------------------------------------------- */
uint32_t vu0_clip(const float fs_xyz[3], float ft_w, uint32_t prev) {
    float w = fabsf(ft_w);
    uint32_t c = prev << 6;
    for (int L = 0; L < 3; L++) {
        if (fs_xyz[L] >  w) c |= (1u << (2 * L));
        if (fs_xyz[L] < -w) c |= (1u << (2 * L + 1));
    }
    return c & 0xFFFFFFu;
}
