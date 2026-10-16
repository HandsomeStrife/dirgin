/* =====================================================================
 * vu0.h  --  Portable re-expression of the EE COP2 / VU0 macro-mode
 * vector ops the engine uses inline (819 source files in the corpus use
 * _vmul/_vadd/_vmadd/_vdiv/_vrsqrt/_vclip/_vftoi/_vitof/...).
 *
 * The oracle (port/cop2/vu0_macro.c) decodes a raw 32-bit COP2
 * instruction word, reads dest-mask / broadcast / fsf-ftf fields out of
 * it, and executes on inline guest VU0 state. That is exactly the shape
 * we are reauthoring AWAY from: here each op is a named function that
 * takes plain float[4] lanes and an explicit dest mask, so reauthored
 * engine code reads as math instead of as decoded opcodes.
 *
 * Correspondence with the oracle (port/cop2/vu0_macro.c):
 *   vu0_mul/add/sub/madd/msub/max/mini   <- ee_cop2_v{mul,add,...}
 *   vu0_*_bc (broadcast one source lane) <- ee_cop2_v{...}bc
 *   vu0_mula/madda/... (accumulator)     <- ee_cop2_v{...}a / a-bc
 *   vu0_opmula / vu0_opmsub (cross prod) <- ee_cop2_vopmula / vopmsub
 *   vu0_div / vu0_sqrt / vu0_rsqrt       <- ee_cop2_vdiv / vsqrt / vrsqrt
 *   vu0_ftoi / vu0_itof (fixed-point)    <- ee_cop2_vftoi / vitof
 *   vu0_clip                             <- ee_cop2_vclip
 *   vu0_mr32                             <- ee_cop2_vmr32
 *
 * FLOATS: ordinary IEEE-754 (see dmath.h note). The oracle reproduces the
 * PS2's non-IEEE clamp/flush quirks for bit-exact validation; the native
 * engine does not. The DEST MASK, BROADCAST, ACCUMULATOR and FIXED-POINT
 * (ITOF/FTOI) semantics ARE reproduced exactly, because geometry decode
 * depends on them (e.g. ITOF4 = divide by 16; see P1 model notes).
 *
 * Lane indices: 0=x 1=y 2=z 3=w. Dest mask bits: VU0_X=bit0 .. VU0_W=bit3.
 * A masked-out lane in `dst` is left untouched (hardware preserves it).
 * ===================================================================== */
#ifndef DOC_VU0_H
#define DOC_VU0_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dest-mask bits (choose per-lane which components are written) */
enum {
    VU0_X    = 1 << 0,
    VU0_Y    = 1 << 1,
    VU0_Z    = 1 << 2,
    VU0_W    = 1 << 3,
    VU0_XYZ  = VU0_X | VU0_Y | VU0_Z,
    VU0_XYZW = VU0_X | VU0_Y | VU0_Z | VU0_W
};

/* lane ids for broadcast / fsf-ftf scalar selectors */
enum { VU0_LANE_X = 0, VU0_LANE_Y = 1, VU0_LANE_Z = 2, VU0_LANE_W = 3 };

/* VU0 accumulator (the ACC register the *a / madd forms read & write). */
typedef struct { float lane[4]; } vu0_acc;

/* ---- FMAC: per-lane dst[L] = dst[L] (masked) ------------------------- */
void vu0_add (float dst[4], const float a[4], const float b[4], unsigned mask);
void vu0_sub (float dst[4], const float a[4], const float b[4], unsigned mask);
void vu0_mul (float dst[4], const float a[4], const float b[4], unsigned mask);
void vu0_max (float dst[4], const float a[4], const float b[4], unsigned mask);
void vu0_mini(float dst[4], const float a[4], const float b[4], unsigned mask);

/* broadcast: every active lane uses b[bc] as the second operand        */
void vu0_add_bc(float dst[4], const float a[4], const float b[4], int bc, unsigned mask);
void vu0_sub_bc(float dst[4], const float a[4], const float b[4], int bc, unsigned mask);
void vu0_mul_bc(float dst[4], const float a[4], const float b[4], int bc, unsigned mask);

/* multiply-add using an accumulator: dst = acc +/- a*b                  */
void vu0_madd(float dst[4], const vu0_acc *acc, const float a[4], const float b[4], unsigned mask);
void vu0_msub(float dst[4], const vu0_acc *acc, const float a[4], const float b[4], unsigned mask);

/* ---- FMAC: write to the accumulator (the *a forms) ------------------- */
void vu0_mula   (vu0_acc *acc, const float a[4], const float b[4], unsigned mask);
void vu0_adda   (vu0_acc *acc, const float a[4], const float b[4], unsigned mask);
void vu0_madda  (vu0_acc *acc, const float a[4], const float b[4], unsigned mask);
void vu0_msuba  (vu0_acc *acc, const float a[4], const float b[4], unsigned mask);
void vu0_mula_bc (vu0_acc *acc, const float a[4], const float b[4], int bc, unsigned mask);
void vu0_madda_bc(vu0_acc *acc, const float a[4], const float b[4], int bc, unsigned mask);

/* ---- outer product (cross product) ----------------------------------- *
 * vu0_opmula loads ACC.xyz with the "first half" of a cross product and
 * vu0_opmsub finishes it: together dst.xyz = cross(a, b). This is the
 * exact OPMULA/OPMSUB pairing the oracle implements (acc x/y/z = fs.y*ft.z
 * etc.), surfaced as one convenience that returns the full cross product. */
void vu0_opmula(vu0_acc *acc, const float a[4], const float b[4]);
void vu0_opmsub(float dst[4], const vu0_acc *acc, const float a[4], const float b[4]);
void vu0_cross (float dst[3], const float a[3], const float b[3]); /* a x b */

/* ---- EFU scalar pipe (Q register) ------------------------------------ */
float vu0_div  (float num, float den);   /* num / den                      */
float vu0_sqrt (float x);                /* sqrt(|x|)                      */
float vu0_rsqrt(float num, float den);   /* num / sqrt(|den|)              */

/* ---- fixed-point convert (fracbits in {0,4,12,15}) ------------------- *
 * FTOI: result lane = (int32)(src * 2^fracbits), reinterpreted as the
 *       integer bit pattern. ITOF: inverse. ITOF4 (fracbits=4) is the
 *       /16 dequant used for MOD int8 positions (P1 model notes). */
void vu0_ftoi(int32_t dst[4], const float src[4], int fracbits, unsigned mask);
void vu0_itof(float dst[4], const int32_t src[4], int fracbits, unsigned mask);

/* ---- misc ------------------------------------------------------------ */
void vu0_mr32(float dst[4], const float src[4], unsigned mask); /* rotate x<-y<-z<-w<-x */

/* ---- frustum clip ---------------------------------------------------- *
 * Returns the 24-bit clip code: for each of x,y,z two bits say whether the
 * lane is beyond +|w| / below -|w|. `prev` is shifted left 6 and OR'd in,
 * matching the hardware CLIP register's rolling 4-deep history. */
uint32_t vu0_clip(const float fs_xyz[3], float ft_w, uint32_t prev);

#ifdef __cplusplus
}
#endif
#endif /* DOC_VU0_H */
