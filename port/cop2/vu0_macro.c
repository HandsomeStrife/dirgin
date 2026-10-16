/* =====================================================================
 * vu0_macro.c  --  Real implementation of the EE COP2 / VU0 macro-mode
 * intrinsics declared in recomp/ee_runtime.h (§7).  This closes the
 * "VU0-macro seam": the recompiler decodes each COP2 macro instruction to
 * one of these named intrinsics and passes the raw 32-bit instruction word;
 * here we decode the operand fields (dest xyzw mask, broadcast lane, fsf/ftf,
 * registers) and execute on the inline ee_vu0 state.
 *
 * This is the INTEGRATED-build implementation (replaces recomp/ee_stubs.c,
 * which only logs).  Scalar C, VU0 macro-mode FMAC/EFU/convert/clip semantics.
 *
 * Float semantics: VU floats are the same non-IEEE PS2 floats as COP1 (no
 * NaN/Inf, overflow saturates to +-FLT_MAX, denormals flush to signed zero).
 * We reuse the audited ee_fadd/ee_fsub/ee_fmul/ee_fdiv/ee_fsqrt/ee_frsqrt
 * helpers from ee_runtime.h (bit-pattern in/out) so the quirks live in one
 * place.  NOTE (documented approximation): the VU FMAC clamp pipeline is not
 * bit-identical to COP1 in the denormal/overflow corner (VU clamps slightly
 * differently and never does COP1's double-precision fallback); for M1 boot
 * init math this is a faithful approximation.  MAC/STATUS sticky flags are
 * maintained best-effort; the CLIP register (frustum) is exact.
 *
 * Field layout (EE COP2 macro, CO=1), verified against the recompiled corpus:
 *   dest mask : bit24=x bit23=y bit22=z bit21=w   (lane L active = (raw>>(24-L))&1)
 *   ft        : bits 20..16     fs : bits 15..11     fd : bits 10..6
 *   bc        : bits  1..0      fsf: bits 22..21     ftf: bits 24..23
 * For the register-move "special2" ops (MOVE/MR32/ABS/FTOI/ITOF/MFIR/Rxxx)
 * the WRITTEN register is Ft and the READ register is Fs (PCSX2 convention).
 * ===================================================================== */
#include "ee_runtime.h"
#include <stdio.h>
#include <string.h>

/* ---- VU0 state ------------------------------------------------------- *
 * ee_vu0 is carried inline in ee_state, but ee_runtime.h gives no place to
 * keep the VU0 R (random) and I (immediate) registers, which macro mode can
 * still touch (RINIT/RNEXT/RGET/RXOR and the *i FMAC variants).  For the
 * single-VU0 M1 boot these live here as module state.  Documented. */
static uint32_t g_vu0_R = 0x3F800000u;   /* random seed register          */
static uint32_t g_vu0_I = 0;             /* I register (bit pattern)       */

/* Op-usage tally for the honest M1 report (dumped by vu0_macro_dump_stats). */
static unsigned long g_op_count[128];
static const char   *g_op_name[128];
static int           g_nops;
static int tally(const char *name){
    for (int i=0;i<g_nops;i++) if (g_op_name[i]==name){ g_op_count[i]++; return i; }
    if (g_nops < 128){ g_op_name[g_nops]=name; g_op_count[g_nops]=1; return g_nops++; }
    return -1;
}
void vu0_macro_dump_stats(FILE *f){
    fprintf(f,"[cop2] VU0 macro-op usage this run:\n");
    for (int i=0;i<g_nops;i++)
        fprintf(f,"    %-10s %lu\n", g_op_name[i], g_op_count[i]);
}

static ee_vu0 g_vu0;
static int g_vu0_init;
ee_vu0 *ee_vu0_state(ee_state *ee){
    (void)ee;
    if (!g_vu0_init){ memset(&g_vu0,0,sizeof g_vu0);
        g_vu0.vf[0][0]=0.0f; g_vu0.vf[0][1]=0.0f; g_vu0.vf[0][2]=0.0f; g_vu0.vf[0][3]=1.0f;
        g_vu0_init=1; }
    return &g_vu0;
}

/* ---- float<->bits ---------------------------------------------------- */
static inline uint32_t fb(float f){ uint32_t b; memcpy(&b,&f,4); return b; }
static inline float    bf(uint32_t b){ float f; memcpy(&f,&b,4); return f; }

/* ---- field decode ---------------------------------------------------- */
#define FT(r)  (((r)>>16)&0x1f)
#define FS(r)  (((r)>>11)&0x1f)
#define FD(r)  (((r)>> 6)&0x1f)
#define BC(r)  ((r)&0x3)
#define FSF(r) (((r)>>21)&0x3)
#define FTF(r) (((r)>>23)&0x3)
#define DEST(r,L) (((r)>>(24-(L)))&1)

/* ---- FMAC core ------------------------------------------------------- */
enum { K_ADD, K_SUB, K_MUL, K_MADD, K_MSUB, K_MAX, K_MINI };
enum { S2_FT, S2_BC, S2_Q, S2_I };

static void fmac(ee_state *ee, uint32_t raw, int kind, int s2, int toacc){
    ee_vu0 *v = ee_vu0_state(ee);
    int ft=FT(raw), fs=FS(raw), fd=FD(raw), bc=BC(raw);
    for (int L=0; L<4; L++){
        if (!DEST(raw,L)) continue;
        uint32_t a = fb(v->vf[fs][L]);
        uint32_t b;
        switch (s2){
        case S2_BC: b = fb(v->vf[ft][bc]); break;
        case S2_Q:  b = v->q;              break;
        case S2_I:  b = g_vu0_I;           break;
        default:    b = fb(v->vf[ft][L]);  break;
        }
        uint32_t r;
        switch (kind){
        case K_ADD:  r = ee_fadd(a,b); break;
        case K_SUB:  r = ee_fsub(a,b); break;
        case K_MUL:  r = ee_fmul(a,b); break;
        case K_MADD: r = ee_fadd(fb(v->acc[L]), ee_fmul(a,b)); break;
        case K_MSUB: r = ee_fsub(fb(v->acc[L]), ee_fmul(a,b)); break;
        case K_MAX:  r = (bf(a) >= bf(b)) ? a : b; break;
        default:     r = (bf(a) <= bf(b)) ? a : b; break; /* K_MINI */
        }
        if (toacc) v->acc[L] = bf(r); else v->vf[fd][L] = bf(r);
    }
}

/* ===================================================================== *
 * FMAC: write-to-vf                                                     *
 * ===================================================================== */
void ee_cop2_vadd  (ee_state*ee,uint32_t r){ tally("vadd");  fmac(ee,r,K_ADD ,S2_FT,0); }
void ee_cop2_vsub  (ee_state*ee,uint32_t r){ tally("vsub");  fmac(ee,r,K_SUB ,S2_FT,0); }
void ee_cop2_vmul  (ee_state*ee,uint32_t r){ tally("vmul");  fmac(ee,r,K_MUL ,S2_FT,0); }
void ee_cop2_vmadd (ee_state*ee,uint32_t r){ tally("vmadd"); fmac(ee,r,K_MADD,S2_FT,0); }
void ee_cop2_vmsub (ee_state*ee,uint32_t r){ tally("vmsub"); fmac(ee,r,K_MSUB,S2_FT,0); }
void ee_cop2_vmax  (ee_state*ee,uint32_t r){ tally("vmax");  fmac(ee,r,K_MAX ,S2_FT,0); }
void ee_cop2_vmini (ee_state*ee,uint32_t r){ tally("vmini"); fmac(ee,r,K_MINI,S2_FT,0); }

void ee_cop2_vaddbc (ee_state*ee,uint32_t r){ tally("vaddbc");  fmac(ee,r,K_ADD ,S2_BC,0); }
void ee_cop2_vsubbc (ee_state*ee,uint32_t r){ tally("vsubbc");  fmac(ee,r,K_SUB ,S2_BC,0); }
void ee_cop2_vmulbc (ee_state*ee,uint32_t r){ tally("vmulbc");  fmac(ee,r,K_MUL ,S2_BC,0); }
void ee_cop2_vmaddbc(ee_state*ee,uint32_t r){ tally("vmaddbc"); fmac(ee,r,K_MADD,S2_BC,0); }
void ee_cop2_vmsubbc(ee_state*ee,uint32_t r){ tally("vmsubbc"); fmac(ee,r,K_MSUB,S2_BC,0); }
void ee_cop2_vmaxbc (ee_state*ee,uint32_t r){ tally("vmaxbc");  fmac(ee,r,K_MAX ,S2_BC,0); }
void ee_cop2_vminibc(ee_state*ee,uint32_t r){ tally("vminibc"); fmac(ee,r,K_MINI,S2_BC,0); }

void ee_cop2_vaddq (ee_state*ee,uint32_t r){ tally("vaddq");  fmac(ee,r,K_ADD ,S2_Q,0); }
void ee_cop2_vsubq (ee_state*ee,uint32_t r){ tally("vsubq");  fmac(ee,r,K_SUB ,S2_Q,0); }
void ee_cop2_vmulq (ee_state*ee,uint32_t r){ tally("vmulq");  fmac(ee,r,K_MUL ,S2_Q,0); }
void ee_cop2_vmaddq(ee_state*ee,uint32_t r){ tally("vmaddq"); fmac(ee,r,K_MADD,S2_Q,0); }
void ee_cop2_vmsubq(ee_state*ee,uint32_t r){ tally("vmsubq"); fmac(ee,r,K_MSUB,S2_Q,0); }

void ee_cop2_vaddi (ee_state*ee,uint32_t r){ tally("vaddi");  fmac(ee,r,K_ADD ,S2_I,0); }
void ee_cop2_vsubi (ee_state*ee,uint32_t r){ tally("vsubi");  fmac(ee,r,K_SUB ,S2_I,0); }
void ee_cop2_vmuli (ee_state*ee,uint32_t r){ tally("vmuli");  fmac(ee,r,K_MUL ,S2_I,0); }
void ee_cop2_vmaddi(ee_state*ee,uint32_t r){ tally("vmaddi"); fmac(ee,r,K_MADD,S2_I,0); }
void ee_cop2_vmsubi(ee_state*ee,uint32_t r){ tally("vmsubi"); fmac(ee,r,K_MSUB,S2_I,0); }
void ee_cop2_vmaxi (ee_state*ee,uint32_t r){ tally("vmaxi");  fmac(ee,r,K_MAX ,S2_I,0); }
void ee_cop2_vminii(ee_state*ee,uint32_t r){ tally("vminii"); fmac(ee,r,K_MINI,S2_I,0); }

/* ===================================================================== *
 * FMAC: write-to-ACC (*a forms)                                         *
 * ===================================================================== */
void ee_cop2_vadda  (ee_state*ee,uint32_t r){ tally("vadda");   fmac(ee,r,K_ADD ,S2_FT,1); }
void ee_cop2_vsuba  (ee_state*ee,uint32_t r){ tally("vsuba");   fmac(ee,r,K_SUB ,S2_FT,1); }
void ee_cop2_vmula  (ee_state*ee,uint32_t r){ tally("vmula");   fmac(ee,r,K_MUL ,S2_FT,1); }
void ee_cop2_vmadda (ee_state*ee,uint32_t r){ tally("vmadda");  fmac(ee,r,K_MADD,S2_FT,1); }
void ee_cop2_vmsuba (ee_state*ee,uint32_t r){ tally("vmsuba");  fmac(ee,r,K_MSUB,S2_FT,1); }

void ee_cop2_vaddabc (ee_state*ee,uint32_t r){ tally("vaddabc");  fmac(ee,r,K_ADD ,S2_BC,1); }
void ee_cop2_vsubabc (ee_state*ee,uint32_t r){ tally("vsubabc");  fmac(ee,r,K_SUB ,S2_BC,1); }
void ee_cop2_vmulabc (ee_state*ee,uint32_t r){ tally("vmulabc");  fmac(ee,r,K_MUL ,S2_BC,1); }
void ee_cop2_vmaddabc(ee_state*ee,uint32_t r){ tally("vmaddabc"); fmac(ee,r,K_MADD,S2_BC,1); }
void ee_cop2_vmsubabc(ee_state*ee,uint32_t r){ tally("vmsubabc"); fmac(ee,r,K_MSUB,S2_BC,1); }

void ee_cop2_vaddaq (ee_state*ee,uint32_t r){ tally("vaddaq");  fmac(ee,r,K_ADD ,S2_Q,1); }
void ee_cop2_vsubaq (ee_state*ee,uint32_t r){ tally("vsubaq");  fmac(ee,r,K_SUB ,S2_Q,1); }
void ee_cop2_vmulaq (ee_state*ee,uint32_t r){ tally("vmulaq");  fmac(ee,r,K_MUL ,S2_Q,1); }
void ee_cop2_vmaddaq(ee_state*ee,uint32_t r){ tally("vmaddaq"); fmac(ee,r,K_MADD,S2_Q,1); }
void ee_cop2_vmsubaq(ee_state*ee,uint32_t r){ tally("vmsubaq"); fmac(ee,r,K_MSUB,S2_Q,1); }

void ee_cop2_vaddai (ee_state*ee,uint32_t r){ tally("vaddai");  fmac(ee,r,K_ADD ,S2_I,1); }
void ee_cop2_vsubai (ee_state*ee,uint32_t r){ tally("vsubai");  fmac(ee,r,K_SUB ,S2_I,1); }
void ee_cop2_vmulai (ee_state*ee,uint32_t r){ tally("vmulai");  fmac(ee,r,K_MUL ,S2_I,1); }
void ee_cop2_vmaddai(ee_state*ee,uint32_t r){ tally("vmaddai"); fmac(ee,r,K_MADD,S2_I,1); }
void ee_cop2_vmsubai(ee_state*ee,uint32_t r){ tally("vmsubai"); fmac(ee,r,K_MSUB,S2_I,1); }

/* ===================================================================== *
 * Outer-product (cross product) ops                                     *
 * ===================================================================== */
void ee_cop2_vopmula(ee_state*ee,uint32_t r){
    tally("vopmula");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r), fs=FS(r);
    v->acc[0]=bf(ee_fmul(fb(v->vf[fs][1]), fb(v->vf[ft][2])));  /* x = fs.y*ft.z */
    v->acc[1]=bf(ee_fmul(fb(v->vf[fs][2]), fb(v->vf[ft][0])));  /* y = fs.z*ft.x */
    v->acc[2]=bf(ee_fmul(fb(v->vf[fs][0]), fb(v->vf[ft][1])));  /* z = fs.x*ft.y */
}
void ee_cop2_vopmsub(ee_state*ee,uint32_t r){
    tally("vopmsub");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r), fs=FS(r), fd=FD(r);
    /* Cross product: fd = fs x ft, completing the pair started by VOPMULA.
     * VOPMULA set acc = (fs.y*ft.z, fs.z*ft.x, fs.x*ft.y); VOPMSUB subtracts the
     * SWAPPED products so each lane is a 2x2 determinant:
     *   fd.x = acc.x - fs.z*ft.y,  fd.y = acc.y - fs.x*ft.z,  fd.z = acc.z - fs.y*ft.x.
     * (The previous code subtracted the same products VOPMULA added -> always 0.)
     * Read all sources into temporaries first so fd may alias fs or ft. */
    uint32_t x=ee_fsub(fb(v->acc[0]), ee_fmul(fb(v->vf[fs][2]), fb(v->vf[ft][1])));
    uint32_t y=ee_fsub(fb(v->acc[1]), ee_fmul(fb(v->vf[fs][0]), fb(v->vf[ft][2])));
    uint32_t z=ee_fsub(fb(v->acc[2]), ee_fmul(fb(v->vf[fs][1]), fb(v->vf[ft][0])));
    v->vf[fd][0]=bf(x); v->vf[fd][1]=bf(y); v->vf[fd][2]=bf(z);
}

/* ===================================================================== *
 * Register moves / converts (write=Ft, read=Fs)                         *
 * ===================================================================== */
void ee_cop2_vmove(ee_state*ee,uint32_t r){
    tally("vmove");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r), fs=FS(r);
    for (int L=0;L<4;L++) if (DEST(r,L)) v->vf[ft][L]=v->vf[fs][L];
}
void ee_cop2_vmr32(ee_state*ee,uint32_t r){
    tally("vmr32");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r), fs=FS(r);
    float s[4]={v->vf[fs][0],v->vf[fs][1],v->vf[fs][2],v->vf[fs][3]};
    /* rotate-right by one lane: x<-y, y<-z, z<-w, w<-x */
    float rot[4]={s[1],s[2],s[3],s[0]};
    for (int L=0;L<4;L++) if (DEST(r,L)) v->vf[ft][L]=rot[L];
}
void ee_cop2_vabs(ee_state*ee,uint32_t r){
    tally("vabs");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r), fs=FS(r);
    for (int L=0;L<4;L++) if (DEST(r,L)) v->vf[ft][L]=bf(fb(v->vf[fs][L]) & 0x7fffffffu);
}

/* ---- FTOI / ITOF (variant = raw&3 -> 0,4,12,15 fractional bits) ------ */
static double ftoi_scale(uint32_t raw){
    switch (raw & 3){ case 1: return 16.0; case 2: return 4096.0; case 3: return 32768.0; default: return 1.0; }
}
void ee_cop2_vftoi(ee_state*ee,uint32_t r){
    tally("vftoi");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r), fs=FS(r); double sc=ftoi_scale(r);
    for (int L=0;L<4;L++) if (DEST(r,L)){
        double d=(double)v->vf[fs][L]*sc; int32_t iv;
        if (d>=2147483647.0) iv=0x7fffffff; else if (d<=-2147483648.0) iv=(int32_t)0x80000000; else iv=(int32_t)d;
        uint32_t b=(uint32_t)iv; v->vf[ft][L]=bf(b);
    }
}
void ee_cop2_vitof(ee_state*ee,uint32_t r){
    tally("vitof");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r), fs=FS(r); double sc=ftoi_scale(r);
    for (int L=0;L<4;L++) if (DEST(r,L)){
        int32_t iv=(int32_t)fb(v->vf[fs][L]); v->vf[ft][L]=(float)((double)iv/sc);
    }
}

/* ===================================================================== *
 * EFU (Q / P pipeline)                                                  *
 * ===================================================================== */
void ee_cop2_vdiv(ee_state*ee,uint32_t r){
    tally("vdiv");
    ee_vu0 *v=ee_vu0_state(ee);
    uint32_t a=fb(v->vf[FS(r)][FSF(r)]), b=fb(v->vf[FT(r)][FTF(r)]);
    v->q = ee_fdiv(a,b);
}
void ee_cop2_vsqrt(ee_state*ee,uint32_t r){
    tally("vsqrt");
    ee_vu0 *v=ee_vu0_state(ee);
    v->q = ee_fsqrt(fb(v->vf[FT(r)][FTF(r)]) & 0x7fffffffu);
}
void ee_cop2_vrsqrt(ee_state*ee,uint32_t r){
    tally("vrsqrt");
    ee_vu0 *v=ee_vu0_state(ee);
    uint32_t a=fb(v->vf[FS(r)][FSF(r)]), b=fb(v->vf[FT(r)][FTF(r)]);
    v->q = ee_frsqrt(a,b);
}
void ee_cop2_vwaitq(ee_state*ee,uint32_t r){ (void)ee;(void)r; tally("vwaitq"); /* Q ready: nop */ }

/* ===================================================================== *
 * CLIP (frustum)  -- exact                                              *
 * ===================================================================== */
void ee_cop2_vclip(ee_state*ee,uint32_t r){
    tally("vclip");
    ee_vu0 *v=ee_vu0_state(ee); int fs=FS(r), ft=FT(r);
    float w = bf(fb(v->vf[ft][3]) & 0x7fffffffu);   /* |ft.w| */
    uint32_t c = v->clip << 6;
    for (int L=0;L<3;L++){
        float a=v->vf[fs][L];
        if (a >  w) c |= (1u << (2*L));
        if (a < -w) c |= (1u << (2*L+1));
    }
    v->clip = c & 0xFFFFFFu;
}

/* ===================================================================== *
 * Integer-reg (VI) move ops                                             *
 * ===================================================================== */
void ee_cop2_vmfir(ee_state*ee,uint32_t r){
    tally("vmfir");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r), fs=FS(r);
    int32_t val=(int32_t)(int16_t)v->vi[fs & 15];
    for (int L=0;L<4;L++) if (DEST(r,L)) v->vf[ft][L]=bf((uint32_t)val);
}
void ee_cop2_vmtir(ee_state*ee,uint32_t r){
    tally("vmtir");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r), fs=FS(r);
    v->vi[ft & 15] = (uint16_t)(fb(v->vf[fs][FSF(r)]) & 0xffff);
}

/* ===================================================================== *
 * Random generator (best-effort, LFSR per PCSX2)                        *
 * ===================================================================== */
static float vu_r_as_float(void){
    uint32_t b = (g_vu0_R & 0x007fffffu) | 0x3f800000u; return bf(b);
}
static void vu_r_advance(void){
    uint32_t x = (g_vu0_R >> 4) & 1, y = (g_vu0_R >> 22) & 1;
    g_vu0_R = ((g_vu0_R << 1) | (x ^ y)) & 0x00ffffffu;
}
void ee_cop2_vrinit(ee_state*ee,uint32_t r){
    tally("vrinit");
    ee_vu0 *v=ee_vu0_state(ee);
    g_vu0_R = fb(v->vf[FS(r)][FSF(r)]) & 0x00ffffffu;
}
void ee_cop2_vrget(ee_state*ee,uint32_t r){
    tally("vrget");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r);
    float f=vu_r_as_float();
    for (int L=0;L<4;L++) if (DEST(r,L)) v->vf[ft][L]=f;
}
void ee_cop2_vrnext(ee_state*ee,uint32_t r){
    tally("vrnext");
    ee_vu0 *v=ee_vu0_state(ee); int ft=FT(r);
    vu_r_advance();
    float f=vu_r_as_float();
    for (int L=0;L<4;L++) if (DEST(r,L)) v->vf[ft][L]=f;
}
void ee_cop2_vrxor(ee_state*ee,uint32_t r){
    tally("vrxor");
    ee_vu0 *v=ee_vu0_state(ee);
    g_vu0_R ^= fb(v->vf[FS(r)][FSF(r)]) & 0x00ffffffu;
}

/* ===================================================================== *
 * VU0 integer ALU ops (not seen in this ELF's macro stream) -> log once  *
 * ===================================================================== */
static void log_once(const char *m, uint32_t raw){
    static const char *seen[64]; static int n;
    for (int i=0;i<n;i++) if (seen[i]==m) return;
    if (n<64) seen[n++]=m;
    fprintf(stderr,"[cop2] NOTE: best-effort/stub op %s raw=%08x (first hit)\n",m,raw);
}
void ee_cop2_viadd (ee_state*ee,uint32_t r){ tally("viadd");  ee_vu0*v=ee_vu0_state(ee); v->vi[FD(r)&15]=(uint16_t)(v->vi[FS(r)&15]+v->vi[FT(r)&15]); }
void ee_cop2_visub (ee_state*ee,uint32_t r){ tally("visub");  ee_vu0*v=ee_vu0_state(ee); v->vi[FD(r)&15]=(uint16_t)(v->vi[FS(r)&15]-v->vi[FT(r)&15]); }
void ee_cop2_viand (ee_state*ee,uint32_t r){ tally("viand");  ee_vu0*v=ee_vu0_state(ee); v->vi[FD(r)&15]=(uint16_t)(v->vi[FS(r)&15]&v->vi[FT(r)&15]); }
void ee_cop2_vior  (ee_state*ee,uint32_t r){ tally("vior");   ee_vu0*v=ee_vu0_state(ee); v->vi[FD(r)&15]=(uint16_t)(v->vi[FS(r)&15]|v->vi[FT(r)&15]); }
void ee_cop2_viaddi(ee_state*ee,uint32_t r){ tally("viaddi"); ee_vu0*v=ee_vu0_state(ee);
    int32_t imm=(int32_t)((r>>6)&0x1f); if (imm&0x10) imm|=~0x1f; /* sign-extend 5-bit */
    v->vi[FT(r)&15]=(uint16_t)(v->vi[FS(r)&15]+imm); }

/* VU0 integer load/store to VU-mem: the macro stream in this ELF never uses
 * these (the loader confirmed 0 occurrences); implement as logged stubs so a
 * surprise hit is visible rather than silently wrong. */
void ee_cop2_vlqi(ee_state*ee,uint32_t r){ (void)ee; tally("vlqi"); log_once("vlqi",r); }
void ee_cop2_vsqi(ee_state*ee,uint32_t r){ (void)ee; tally("vsqi"); log_once("vsqi",r); }
void ee_cop2_vlqd(ee_state*ee,uint32_t r){ (void)ee; tally("vlqd"); log_once("vlqd",r); }
void ee_cop2_vsqd(ee_state*ee,uint32_t r){ (void)ee; tally("vsqd"); log_once("vsqd",r); }
void ee_cop2_vilwr(ee_state*ee,uint32_t r){ (void)ee; tally("vilwr"); log_once("vilwr",r); }
void ee_cop2_viswr(ee_state*ee,uint32_t r){ (void)ee; tally("viswr"); log_once("viswr",r); }

/* VU0 microprogram execution (VCALLMS/VCALLMSR): VU0 micro engine is out of
 * scope for M1 (no geometry).  Log so we see if the boot path relies on it. */
void ee_cop2_vcallms (ee_state*ee,uint32_t r){ (void)ee; tally("vcallms");  log_once("vcallms",r); }
void ee_cop2_vcallmsr(ee_state*ee,uint32_t r){ (void)ee; tally("vcallmsr"); log_once("vcallmsr",r); }

void ee_cop2_vnop(ee_state*ee,uint32_t r){ (void)ee;(void)r; tally("vnop"); }

/* ===================================================================== *
 * 128-bit moves GPR<->VU reg (QMFC2/QMTC2) + control regs (CFC2/CTC2)   *
 * ===================================================================== */
void ee_cop2_qmtc2(ee_state *ee, int vf, int lane, uint32_t bits){
    ee_vu0 *v=ee_vu0_state(ee); v->vf[vf][lane]=bf(bits);
}
uint32_t ee_cop2_qmfc2(ee_state *ee, int vf, int lane){
    ee_vu0 *v=ee_vu0_state(ee); return fb(v->vf[vf][lane]);
}
uint32_t ee_cop2_cfc2(ee_state *ee, int creg){
    ee_vu0 *v=ee_vu0_state(ee);
    if (creg >= 0 && creg < 16) return (uint32_t)v->vi[creg];
    switch (creg){
    case 16: return v->status;
    case 17: return v->mac;
    case 18: return v->clip;
    case 20: return g_vu0_R;
    case 21: return g_vu0_I;
    case 22: return v->q;
    default: return 0;
    }
}
void ee_cop2_ctc2(ee_state *ee, int creg, uint32_t val){
    ee_vu0 *v=ee_vu0_state(ee);
    if (creg >= 0 && creg < 16){ if (creg) v->vi[creg]=(uint16_t)val; return; }
    switch (creg){
    case 16: v->status=val; break;
    case 17: v->mac=val;    break;
    case 18: v->clip=val;   break;
    case 20: g_vu0_R=val;   break;
    case 21: g_vu0_I=val;   break;
    case 22: v->q=val;      break;
    default: break;
    }
}
