/* GENERATED logging stubs for the COP2/VU0 macro intrinsics. These are
 * used ONLY for the recompiler's standalone test link; the integrated
 * build links the real implementations from vu/ (WS-V). */
#include "ee_runtime.h"
#include <stdio.h>
#ifdef EE_COP2_LOG
#define LOG(m,raw) fprintf(stderr,"[vu0] %-9s raw=%08x\n",m,raw)
#else
#define LOG(m,raw) ((void)(raw))
#endif
void ee_cop2_vaddbc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vaddbc",raw); }
void ee_cop2_vsubbc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsubbc",raw); }
void ee_cop2_vmaddbc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmaddbc",raw); }
void ee_cop2_vmsubbc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmsubbc",raw); }
void ee_cop2_vmaxbc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmaxbc",raw); }
void ee_cop2_vminibc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vminibc",raw); }
void ee_cop2_vmulbc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmulbc",raw); }
void ee_cop2_vmulq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmulq",raw); }
void ee_cop2_vmaxi(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmaxi",raw); }
void ee_cop2_vmuli(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmuli",raw); }
void ee_cop2_vminii(ee_state *ee, uint32_t raw){ (void)ee; LOG("vminii",raw); }
void ee_cop2_vaddq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vaddq",raw); }
void ee_cop2_vmaddq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmaddq",raw); }
void ee_cop2_vaddi(ee_state *ee, uint32_t raw){ (void)ee; LOG("vaddi",raw); }
void ee_cop2_vmaddi(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmaddi",raw); }
void ee_cop2_vsubq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsubq",raw); }
void ee_cop2_vmsubq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmsubq",raw); }
void ee_cop2_vsubi(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsubi",raw); }
void ee_cop2_vmsubi(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmsubi",raw); }
void ee_cop2_vadd(ee_state *ee, uint32_t raw){ (void)ee; LOG("vadd",raw); }
void ee_cop2_vmadd(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmadd",raw); }
void ee_cop2_vmul(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmul",raw); }
void ee_cop2_vmax(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmax",raw); }
void ee_cop2_vsub(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsub",raw); }
void ee_cop2_vmsub(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmsub",raw); }
void ee_cop2_vopmsub(ee_state *ee, uint32_t raw){ (void)ee; LOG("vopmsub",raw); }
void ee_cop2_vmini(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmini",raw); }
void ee_cop2_viadd(ee_state *ee, uint32_t raw){ (void)ee; LOG("viadd",raw); }
void ee_cop2_visub(ee_state *ee, uint32_t raw){ (void)ee; LOG("visub",raw); }
void ee_cop2_viaddi(ee_state *ee, uint32_t raw){ (void)ee; LOG("viaddi",raw); }
void ee_cop2_viand(ee_state *ee, uint32_t raw){ (void)ee; LOG("viand",raw); }
void ee_cop2_vior(ee_state *ee, uint32_t raw){ (void)ee; LOG("vior",raw); }
void ee_cop2_vcallms(ee_state *ee, uint32_t raw){ (void)ee; LOG("vcallms",raw); }
void ee_cop2_vcallmsr(ee_state *ee, uint32_t raw){ (void)ee; LOG("vcallmsr",raw); }
void ee_cop2_vaddabc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vaddabc",raw); }
void ee_cop2_vsubabc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsubabc",raw); }
void ee_cop2_vmaddabc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmaddabc",raw); }
void ee_cop2_vmsubabc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmsubabc",raw); }
void ee_cop2_vitof(ee_state *ee, uint32_t raw){ (void)ee; LOG("vitof",raw); }
void ee_cop2_vftoi(ee_state *ee, uint32_t raw){ (void)ee; LOG("vftoi",raw); }
void ee_cop2_vmulabc(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmulabc",raw); }
void ee_cop2_vmulaq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmulaq",raw); }
void ee_cop2_vabs(ee_state *ee, uint32_t raw){ (void)ee; LOG("vabs",raw); }
void ee_cop2_vmulai(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmulai",raw); }
void ee_cop2_vclip(ee_state *ee, uint32_t raw){ (void)ee; LOG("vclip",raw); }
void ee_cop2_vaddaq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vaddaq",raw); }
void ee_cop2_vmaddaq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmaddaq",raw); }
void ee_cop2_vaddai(ee_state *ee, uint32_t raw){ (void)ee; LOG("vaddai",raw); }
void ee_cop2_vmaddai(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmaddai",raw); }
void ee_cop2_vsubaq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsubaq",raw); }
void ee_cop2_vmsubaq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmsubaq",raw); }
void ee_cop2_vsubai(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsubai",raw); }
void ee_cop2_vmsubai(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmsubai",raw); }
void ee_cop2_vadda(ee_state *ee, uint32_t raw){ (void)ee; LOG("vadda",raw); }
void ee_cop2_vmadda(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmadda",raw); }
void ee_cop2_vmula(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmula",raw); }
void ee_cop2_vsuba(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsuba",raw); }
void ee_cop2_vmsuba(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmsuba",raw); }
void ee_cop2_vopmula(ee_state *ee, uint32_t raw){ (void)ee; LOG("vopmula",raw); }
void ee_cop2_vnop(ee_state *ee, uint32_t raw){ (void)ee; LOG("vnop",raw); }
void ee_cop2_vmove(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmove",raw); }
void ee_cop2_vmr32(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmr32",raw); }
void ee_cop2_vlqi(ee_state *ee, uint32_t raw){ (void)ee; LOG("vlqi",raw); }
void ee_cop2_vsqi(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsqi",raw); }
void ee_cop2_vlqd(ee_state *ee, uint32_t raw){ (void)ee; LOG("vlqd",raw); }
void ee_cop2_vsqd(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsqd",raw); }
void ee_cop2_vdiv(ee_state *ee, uint32_t raw){ (void)ee; LOG("vdiv",raw); }
void ee_cop2_vsqrt(ee_state *ee, uint32_t raw){ (void)ee; LOG("vsqrt",raw); }
void ee_cop2_vrsqrt(ee_state *ee, uint32_t raw){ (void)ee; LOG("vrsqrt",raw); }
void ee_cop2_vwaitq(ee_state *ee, uint32_t raw){ (void)ee; LOG("vwaitq",raw); }
void ee_cop2_vmtir(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmtir",raw); }
void ee_cop2_vmfir(ee_state *ee, uint32_t raw){ (void)ee; LOG("vmfir",raw); }
void ee_cop2_vilwr(ee_state *ee, uint32_t raw){ (void)ee; LOG("vilwr",raw); }
void ee_cop2_viswr(ee_state *ee, uint32_t raw){ (void)ee; LOG("viswr",raw); }
void ee_cop2_vrnext(ee_state *ee, uint32_t raw){ (void)ee; LOG("vrnext",raw); }
void ee_cop2_vrget(ee_state *ee, uint32_t raw){ (void)ee; LOG("vrget",raw); }
void ee_cop2_vrinit(ee_state *ee, uint32_t raw){ (void)ee; LOG("vrinit",raw); }
void ee_cop2_vrxor(ee_state *ee, uint32_t raw){ (void)ee; LOG("vrxor",raw); }
uint32_t ee_cop2_cfc2(ee_state *ee, int creg){ (void)ee; (void)creg; return 0; }
void ee_cop2_ctc2(ee_state *ee, int creg, uint32_t val){ (void)ee;(void)creg;(void)val; }

/* SIF/IOP HLE routing stubs -- standalone test link only. The integrated
 * build links the real sif/ implementations (WS-K). */
void sif_call_rpc(ee_state *ee){ (void)ee; }
void sif_bind_server(ee_state *ee){ (void)ee; }
void sif_bind_rpc(ee_state *ee){ (void)ee; }
void sif_reboot_iop(ee_state *ee){ (void)ee; }
void sif_sync_iop(ee_state *ee){ (void)ee; }
void sif_writeback_dcache(ee_state *ee){ (void)ee; }
