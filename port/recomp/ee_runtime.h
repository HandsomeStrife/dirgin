/* =====================================================================
 * ee_runtime.h  --  Dirge of Cerberus native port: EE recompiler contract
 * ---------------------------------------------------------------------
 * WS-R owns this file. It is THE integration seam that every other
 * workstream (WS-K kernel/SIF, WS-G GS, WS-V VU, WS-I input, WS-AUD)
 * builds against. Statically-recompiled EE functions include ONLY this
 * header; the rest of the port implements the hooks declared at the
 * bottom.  Keep it C11, freestanding-friendly, and ABI-stable.
 *
 * Scope of the contract:
 *   1. Guest memory model (single 32 MB mmap; EE addr == offset)
 *   2. CPU register file (R5900: 128-bit GPRs, HI/LO pairs, FPU)
 *   3. Recompiled-function signature + calling convention
 *   4. Load / store / unaligned-access helpers
 *   5. MMIO trap-out (REG_GS_* / REG_DMAC_* / REG_VIF1_* -> hle/gs/vu)
 *   6. Syscall trap-out (SYSCALL instr -> hle/)
 *   7. COP2 / VU0 macro-mode representation
 *   8. Branch-delay-slot handling convention (documented, not code)
 * =====================================================================*/
#ifndef EE_RUNTIME_H
#define EE_RUNTIME_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * 1. GUEST MEMORY MODEL
 * ---------------------------------------------------------------------
 * EE main RAM is a single contiguous 32 MB block, mmap'd once at startup.
 * A guest EE *physical/kuseg* address is just an offset into that block:
 *     host_ptr = ee->ram + (guest_addr & EE_RAM_MASK)
 * The game's ELF loads at guest VA 0x00100000 (inside the first 32 MB),
 * runs in kuseg, and never enables the TLB for anything we care about, so
 * VA==PA for all game accesses after masking the kseg0/kseg1 region bits.
 *
 * Address map (top nibble of a 32-bit EE address):
 *   0x0.......  main RAM (0..32MB-1 after masking)       -> ee->ram
 *   0x1.......  MMIO:  GS priv, DMAC, VIF, GIF, INTC, SIF -> ee_mmio_*()
 *   0x7.......  scratchpad (SPR) 16 KB                    -> ee->spr
 *   0x8.../0x9.../0xA... kseg0/kseg1 of the above (mask off bit 29/31)
 * Everything the recompiled code touches resolves through the helpers in
 * section 4 so this policy lives in exactly one place.
 */
#define EE_RAM_SIZE   (32u * 1024u * 1024u)   /* 0x02000000 */
#define EE_RAM_MASK   (EE_RAM_SIZE - 1u)       /* 0x01FFFFFF */
#define EE_SPR_SIZE   (16u * 1024u)            /* scratchpad */
#define EE_SPR_BASE   0x70000000u
#define EE_MMIO_BASE  0x10000000u              /* .. 0x1FFFFFFF */

/* Collapse kseg0/kseg1 mirrors + segment bits down to a canonical address.
 * (EE uses 0x0/0x8/0xA aliases of the same physical space.)              */
static inline uint32_t ee_canon(uint32_t a) { return a & 0x1FFFFFFFu; }

/* ---------------------------------------------------------------------
 * 2. CPU REGISTER FILE
 * ---------------------------------------------------------------------
 * R5900 GPRs are 128 bits. We store them as a little-endian lane union so
 * integer code uses .ud[0]/.sw[0] while MMI (pcpyh, pextlw, ...) uses the
 * wider lanes. Host is assumed little-endian (x86-64 / aarch64); all lane
 * indices below match the EE's own little-endian lane numbering.
 *
 * 32-bit ALU results are SIGN-EXTENDED into the full 64-bit low half, per
 * the MIPS convention the R5900 follows (addu/or/sllv/... write sw[0] then
 * sign-extend into ud[0]). The recompiler emits that sign extension; the
 * helper EE_SET32() below is the single source of truth for it.
 */
typedef union {
    uint64_t ud[2];   /* [0]=low 64, [1]=high 64            */
    int64_t  sd[2];
    uint32_t uw[4];
    int32_t  sw[4];
    uint16_t uh[8];
    int16_t  sh[8];
    uint8_t  ub[16];
    int8_t   sb[16];
} ee_reg;             /* 16 bytes, one 128-bit GPR          */

typedef struct ee_state {
    ee_reg   r[32];       /* GPRs; r[0] ($zero) must stay 0        */
    ee_reg   lo, hi;      /* HI/LO are 128-bit on R5900 (LO1/HI1 in [1]) */
    uint32_t sa;          /* shift-amount reg (for QFSRV)          */

    /* FPU (COP1): 32 single-precision regs kept as raw bit patterns so we
     * can honour the EE FPU's non-IEEE quirks (no NaN/Inf, denorm flush)
     * in the helpers rather than relying on host float semantics.        */
    uint32_t f[32];
    uint32_t fcr31;       /* FPU control/status (C bit, rounding)  */
    uint32_t fcr0;        /* FPU implementation/revision           */
    uint32_t facc;        /* COP1 accumulator (ADDA/MADD/MSUB.S)   */

    /* COP0 (System Control) -- kernel state. The recompiler never touches
     * these directly; MFC0/MTC0/EI/DI/SYNC trap out to the hooks in section
     * 6 so WS-K owns the interrupt/exception model. They live inline here so
     * the HLE has one place to keep them. cop0_status's spin bit (0x10000)
     * MUST read back CLEAR once the HLE has nothing pending, or the boot
     * frame-advance loop (FUN_00241478) never terminates. */
    uint32_t cop0_status; /* COP0 r12 Status                        */
    uint32_t cop0_cause;  /* COP0 r13 Cause                         */
    uint32_t cop0_count;  /* COP0 r9  Count (free-running timer)    */
    uint32_t cop0_epc;    /* COP0 r14 EPC                           */

    uint32_t pc;          /* current guest PC (debug / exceptions) */

    uint8_t *ram;         /* 32 MB main RAM base                   */
    uint8_t *spr;         /* 16 KB scratchpad base                 */
    void    *host;        /* opaque -> hle/ services (kernel, sif) */
} ee_state;

/* Convenience register indices (O32/EABI names). */
enum { R_ZERO=0, R_AT, R_V0, R_V1, R_A0, R_A1, R_A2, R_A3,
       R_T0, R_T1, R_T2, R_T3, R_T4, R_T5, R_T6, R_T7,
       R_S0, R_S1, R_S2, R_S3, R_S4, R_S5, R_S6, R_S7,
       R_T8, R_T9, R_K0, R_K1, R_GP, R_SP, R_FP, R_RA };

/* Write a 32-bit ALU result into a GPR with R5900 sign-extension. */
static inline void EE_SET32(ee_state *ee, int reg, uint32_t v) {
    if (reg) ee->r[reg].sd[0] = (int64_t)(int32_t)v;
}
/* Write a 64-bit (doubleword) result. */
static inline void EE_SET64(ee_state *ee, int reg, uint64_t v) {
    if (reg) { ee->r[reg].ud[0] = v; /* high 64 untouched for d-ops */ }
}
#define EE_GET32(ee,reg)  ((uint32_t)(ee)->r[(reg)].uw[0])
#define EE_GETS32(ee,reg) ((int32_t)(ee)->r[(reg)].sw[0])
#define EE_GET64(ee,reg)  ((uint64_t)(ee)->r[(reg)].ud[0])
#define EE_GETS64(ee,reg) ((int64_t)(ee)->r[(reg)].sd[0])

/* ---------------------------------------------------------------------
 * 3. RECOMPILED-FUNCTION SIGNATURE + CALLING CONVENTION
 * ---------------------------------------------------------------------
 * Every EE function at guest address 0xADDR is emitted as:
 *
 *     void func_00ADDR(ee_state *ee);
 *
 * It reads its arguments from $a0..$a3 (r[R_A0..R_A3]) and from the guest
 * stack (r[R_SP]) exactly as the original did, and leaves its return value
 * in $v0 (/$v1 for 64-bit) -- NOTHING is passed or returned in C. Callers
 * (recompiled or host) set up the GPRs, invoke func_*, then read $v0.
 *
 * Intra-ELF calls (JAL/JALR to a known function start) become a direct C
 * call `func_TARGET(ee);`. Calls through a register / unresolved target go
 * through ee_call_indirect() which looks the address up in the generated
 * function table. This keeps each guest function a normal C function with
 * no per-guest-thread host stack -- a thread switch simply unwinds to the
 * scheduler in hle/ and re-enters at the saved guest function.
 *
 * $ra (return address) is NOT simulated as a real address for ordinary
 * calls: the C return edge replaces it. ee_call_indirect / the function
 * table exist for computed calls, vtables, and callbacks handed to the HLE.
 */
typedef void (*ee_func_t)(ee_state *ee);

/* Provided by the generated function table (generated/function_table.c). */
ee_func_t ee_lookup(uint32_t guest_addr);          /* NULL if unknown      */
void      ee_call_indirect(ee_state *ee, uint32_t guest_addr);

/* ---------------------------------------------------------------------
 * 4. LOAD / STORE / UNALIGNED ACCESS
 * ---------------------------------------------------------------------
 * All guest memory access in recompiled code goes through these. They
 * centralise the address map (section 1): RAM is a direct host access
 * (host is LE, so no byte-swap); the MMIO window traps out to section 5;
 * scratchpad routes to ee->spr. Keeping every access here means the memory
 * policy can change (guard pages, dirty tracking, watchpoints) without
 * touching a single generated file.
 */
static inline int ee_is_mmio(uint32_t a) { return (a & 0xF0000000u) == EE_MMIO_BASE; }
static inline int ee_is_spr (uint32_t a) { return (a & 0xFC000000u) == EE_SPR_BASE;  }

/* MMIO hooks (section 5) -- implemented by hle/ + gs/ + vu/. */
uint32_t ee_mmio_load32 (ee_state *ee, uint32_t addr);
uint64_t ee_mmio_load64 (ee_state *ee, uint32_t addr);
void     ee_mmio_store32(ee_state *ee, uint32_t addr, uint32_t val);
void     ee_mmio_store64(ee_state *ee, uint32_t addr, uint64_t val);
void     ee_mmio_store128(ee_state *ee, uint32_t addr, const ee_reg *val);

static inline uint8_t *ee_host_ptr(ee_state *ee, uint32_t a) {
    a = ee_canon(a);
    if (ee_is_spr(a)) return ee->spr + (a & (EE_SPR_SIZE - 1u));
    return ee->ram + (a & EE_RAM_MASK);
}

/* --- aligned loads --- */
static inline uint8_t  ee_lbu(ee_state *ee, uint32_t a){ return *ee_host_ptr(ee,a); }
static inline int8_t   ee_lb (ee_state *ee, uint32_t a){ return (int8_t)*ee_host_ptr(ee,a); }
static inline uint16_t ee_lhu(ee_state *ee, uint32_t a){ uint16_t v; memcpy(&v, ee_host_ptr(ee,a),2); return v; }
static inline int16_t  ee_lh (ee_state *ee, uint32_t a){ return (int16_t)ee_lhu(ee,a); }
static inline uint32_t ee_lw (ee_state *ee, uint32_t a){
    a = ee_canon(a);
    if (ee_is_mmio(a)) return ee_mmio_load32(ee,a);
    uint32_t v; memcpy(&v, ee_host_ptr(ee,a),4); return v;
}
static inline uint64_t ee_ld (ee_state *ee, uint32_t a){
    a = ee_canon(a);
    if (ee_is_mmio(a)) return ee_mmio_load64(ee,a);
    uint64_t v; memcpy(&v, ee_host_ptr(ee,a),8); return v;
}
static inline void ee_lq(ee_state *ee, uint32_t a, ee_reg *out){ /* 128-bit, 16-aligned */
    memcpy(out, ee_host_ptr(ee, a & ~0xFu), 16);
}

/* --- aligned stores --- */
static inline void ee_sb(ee_state *ee, uint32_t a, uint8_t v){ *ee_host_ptr(ee,a)=v; }
static inline void ee_sh(ee_state *ee, uint32_t a, uint16_t v){ memcpy(ee_host_ptr(ee,a),&v,2); }
static inline void ee_sw(ee_state *ee, uint32_t a, uint32_t v){
    a = ee_canon(a);
    if (ee_is_mmio(a)) { ee_mmio_store32(ee,a,v); return; }
    memcpy(ee_host_ptr(ee,a),&v,4);
}
static inline void ee_sd(ee_state *ee, uint32_t a, uint64_t v){
    a = ee_canon(a);
    if (ee_is_mmio(a)) { ee_mmio_store64(ee,a,v); return; }
    memcpy(ee_host_ptr(ee,a),&v,8);
}
static inline void ee_sq(ee_state *ee, uint32_t a, const ee_reg *v){
    a = ee_canon(a);
    if (ee_is_mmio(a)) { ee_mmio_store128(ee,a,v); return; }
    memcpy(ee_host_ptr(ee, a & ~0xFu), v, 16);
}

/* --- unaligned loads/stores (LWL/LWR/SWL/SWR) ---
 * Implemented with the classic MIPS merge. The recompiler emits a pair
 * (LWL then LWR into the same reg); each helper takes the current reg value
 * and returns the merged result so order-independence holds for LE hosts. */
static inline uint32_t ee_lwl(ee_state *ee, uint32_t a, uint32_t cur){
    uint32_t aa=ee_canon(a); int sh=(aa&3)*8; uint32_t mask=0xFFFFFFFFu<<(24-sh);
    uint32_t w; memcpy(&w, ee_host_ptr(ee, aa&~3u),4);
    return (cur & ~mask) | ((w << (24-sh)) & mask);
}
static inline uint32_t ee_lwr(ee_state *ee, uint32_t a, uint32_t cur){
    uint32_t aa=ee_canon(a); int sh=(aa&3)*8; uint32_t mask=0xFFFFFFFFu>>sh;
    uint32_t w; memcpy(&w, ee_host_ptr(ee, aa&~3u),4);
    return (cur & ~mask) | ((w >> sh) & mask);
}
static inline void ee_swl(ee_state *ee, uint32_t a, uint32_t val){
    uint32_t aa=ee_canon(a); int sh=(aa&3)*8; uint8_t *p=ee_host_ptr(ee,aa&~3u);
    uint32_t w; memcpy(&w,p,4); uint32_t mask=0xFFFFFFFFu>>(24-sh);
    w=(w&~mask)|((val>>(24-sh))&mask); memcpy(p,&w,4);
}
static inline void ee_swr(ee_state *ee, uint32_t a, uint32_t val){
    uint32_t aa=ee_canon(a); int sh=(aa&3)*8; uint8_t *p=ee_host_ptr(ee,aa&~3u);
    uint32_t w; memcpy(&w,p,4); uint32_t mask=0xFFFFFFFFu<<sh;
    w=(w&~mask)|((val<<sh)&mask); memcpy(p,&w,4);
}
/* 64-bit unaligned (LDL/LDR/SDL/SDR): same merge, doubleword granularity. */
static inline uint64_t ee_ldl(ee_state *ee, uint32_t a, uint64_t cur){
    uint32_t aa=ee_canon(a); int sh=(aa&7)*8; uint64_t mask=~0ull<<(56-sh);
    uint64_t w; memcpy(&w, ee_host_ptr(ee, aa&~7u),8);
    return (cur & ~mask) | ((w << (56-sh)) & mask);
}
static inline uint64_t ee_ldr(ee_state *ee, uint32_t a, uint64_t cur){
    uint32_t aa=ee_canon(a); int sh=(aa&7)*8; uint64_t mask=~0ull>>sh;
    uint64_t w; memcpy(&w, ee_host_ptr(ee, aa&~7u),8);
    return (cur & ~mask) | ((w >> sh) & mask);
}
static inline void ee_sdl(ee_state *ee, uint32_t a, uint64_t val){
    uint32_t aa=ee_canon(a); int sh=(aa&7)*8; uint8_t *p=ee_host_ptr(ee,aa&~7u);
    uint64_t w; memcpy(&w,p,8); uint64_t mask=~0ull>>(56-sh);
    w=(w&~mask)|((val>>(56-sh))&mask); memcpy(p,&w,8);
}
static inline void ee_sdr(ee_state *ee, uint32_t a, uint64_t val){
    uint32_t aa=ee_canon(a); int sh=(aa&7)*8; uint8_t *p=ee_host_ptr(ee,aa&~7u);
    uint64_t w; memcpy(&w,p,8); uint64_t mask=~0ull<<sh;
    w=(w&~mask)|((val<<sh)&mask); memcpy(p,&w,8);
}

/* ---------------------------------------------------------------------
 * 5. MMIO TRAP-OUT
 * ---------------------------------------------------------------------
 * Any store whose canonical address lands in 0x10000000-0x1FFFFFFF is a
 * hardware register write and is dispatched to the HLE by ee_mmio_store*()
 * above (declared in section 4 so the inline load/store helpers can call
 * them). The implementation in hle/ decodes the register bank and forwards:
 *
 *   0x10003800-0x10003FFF  VIF0/VIF1   REG_VIF1_*   -> vu/  + gs/ transport
 *   0x10006000-0x10006FFF  GIF         REG_GIF_*    -> gs/
 *   0x1000A000-0x1000FFFF  DMAC chans  REG_DMAC_*   -> hle/ DMA engine
 *   0x1000F000-0x1000F2FF  INTC/SIF    REG_INTC_*   -> hle/ interrupts
 *   0x12000000-0x12001FFF  GS priv     REG_GS_*     -> gs/ (PMODE/DISPFB..)
 *
 * The canonical register constants live in hle/ee_mmio_regs.h (WS-K owns
 * the enum); recompiled code never names a register -- it only ever emits
 * ee_sw(ee, addr, v), and the address carries the identity. That is the
 * whole trap contract: address-in, HLE decodes.
 */

/* ---------------------------------------------------------------------
 * 6. SYSCALL TRAP-OUT
 * ---------------------------------------------------------------------
 * A MIPS SYSCALL instruction is emitted as a call to ee_syscall(code), where
 * `code` is the instruction's 20-bit code field (bits 6..25) -- NOT $v1.
 *
 * SYSCALL CONVENTION (locked for WS-K):
 *   The recompiler ALWAYS passes the instruction code field as `code`. The EE
 *   libkernel wrappers use the pattern `li $v1, n; syscall 0`, so for them
 *   `code == 0` and the real BIOS syscall number is in $v1. Therefore the HLE
 *   rule is: if (code == 0) number = (int32_t)$v1; else number = code;
 *   (a negative $v1 selects the "alternate" form, as on the BIOS). $a0..$a3
 *   carry args; the result goes back in $v0. This matches every libkernel
 *   wrapper and the handful of inline `syscall <imm>` sites.
 *
 * SYNC / EI / DI / COP0 are kernel-visible too, so they trap out the same way
 * (see the COP0 hooks below) rather than being faked in the generated code.
 */
void ee_syscall(ee_state *ee, uint32_t code);
void ee_break  (ee_state *ee, uint32_t code);
void ee_trap_unimpl(ee_state *ee, uint32_t pc, uint32_t raw, const char *mnem);

/* ---- COP0 (System Control) + barrier trap-outs (WS-K owns) ----------
 * MFC0/MTC0 read/write a COP0 register by number; EI/DI toggle the global
 * interrupt enable; SYNC is a barrier/yield point the HLE may use to pump
 * pending interrupts/vblank so spin loops on Status make progress. The
 * recompiler emits calls to these for COP0 instructions and the SYNC op; the
 * standalone test build stubs them (mfc0 -> ee->cop0_* so the spin bit reads
 * clear), the integrated build links the real WS-K implementations. */
uint32_t ee_cop0_mfc0(ee_state *ee, int reg);
void     ee_cop0_mtc0(ee_state *ee, int reg, uint32_t val);
void     ee_ei(ee_state *ee);
void     ee_di(ee_state *ee);
void     ee_eret(ee_state *ee);
void     ee_sync(ee_state *ee, uint32_t stype);

/* ---- SIF / IOP library functions routed to the sif/ HLE --------------
 * These guest functions are NOT recompiled: the recompiler aliases
 * func_<addr> to these hooks (see tools/recompile.py HLE_ROUTES) so both
 * direct JAL calls and the indirect function table reach the HLE. WS-K/sif
 * implements them against this contract. Guest addresses are fixed by the
 * ELF and listed here so the mapping is discoverable in one place:
 *   0x00453d20  sceSifCallRpc     -> sif_call_rpc
 *   0x004535d8  sceSifBindRpc     -> sif_bind_rpc
 *   0x004517f0  RebootIop         -> sif_reboot_iop
 *   0x00451958  SyncIop           -> sif_sync_iop
 *   0x00453360  WriteBackDCache   -> sif_writeback_dcache
 */
void sif_call_rpc(ee_state *ee);
void sif_bind_rpc(ee_state *ee);
void sif_reboot_iop(ee_state *ee);
void sif_sync_iop(ee_state *ee);
void sif_writeback_dcache(ee_state *ee);

/* ---------------------------------------------------------------------
 * 7. COP2 / VU0 MACRO MODE
 * ---------------------------------------------------------------------
 * VU0 in macro mode is driven from EE COP2 instructions (QMFC2/QMTC2,
 * LQC2/SQC2, CFC2/CTC2, and the VU macro ops VADD/VMUL/VMADD/...). The EE
 * state carries the VU0 register file inline so macro ops operate directly
 * on it without a context switch:
 */
typedef struct ee_vu0 {
    float    vf[32][4];   /* 32 quad float regs (x,y,z,w)  */
    uint16_t vi[16];      /* 16 integer regs               */
    float    acc[4];      /* accumulator                   */
    uint32_t q, p;        /* Q (div) and P (EFU) regs      */
    uint32_t mac, status, clip; /* flag regs               */
} ee_vu0;

/* The recompiler emits VU0 macro ops as calls into these intrinsics, which
 * WS-V implements (lifting PCSX2/ps2 semantics -- EE float rules, flag
 * pipeline). COP2 is thus represented as explicit intrinsic calls on a VU0
 * state block, NOT as inline SIMD the C compiler must pattern-match. This
 * keeps VU0 correctness (flags, clamping) in one audited place.           */
ee_vu0 *ee_vu0_state(ee_state *ee);
void ee_cop2_qmtc2(ee_state *ee, int vf, int lane, uint32_t bits);
uint32_t ee_cop2_qmfc2(ee_state *ee, int vf, int lane);
/* --- Full VU0 macro-op surface (WS-R declares; WS-V defines in vu/). ---
 * The recompiler decodes each COP2 macro instruction to one of these
 * named intrinsics and passes the raw instruction word so WS-V retains
 * full field decode (dest mask, broadcast lane, fsf/ftf, registers). */
/* VU0 macro compute ops: recompiler passes the raw 32-bit COP2
 * instruction; WS-V decodes dest mask / bc / registers and executes
 * on ee_vu0 state. (Definitions: vu/; logging stubs: ee_stubs.c.)   */
void ee_cop2_vaddbc(ee_state *ee, uint32_t raw);
void ee_cop2_vsubbc(ee_state *ee, uint32_t raw);
void ee_cop2_vmaddbc(ee_state *ee, uint32_t raw);
void ee_cop2_vmsubbc(ee_state *ee, uint32_t raw);
void ee_cop2_vmaxbc(ee_state *ee, uint32_t raw);
void ee_cop2_vminibc(ee_state *ee, uint32_t raw);
void ee_cop2_vmulbc(ee_state *ee, uint32_t raw);
void ee_cop2_vmulq(ee_state *ee, uint32_t raw);
void ee_cop2_vmaxi(ee_state *ee, uint32_t raw);
void ee_cop2_vmuli(ee_state *ee, uint32_t raw);
void ee_cop2_vminii(ee_state *ee, uint32_t raw);
void ee_cop2_vaddq(ee_state *ee, uint32_t raw);
void ee_cop2_vmaddq(ee_state *ee, uint32_t raw);
void ee_cop2_vaddi(ee_state *ee, uint32_t raw);
void ee_cop2_vmaddi(ee_state *ee, uint32_t raw);
void ee_cop2_vsubq(ee_state *ee, uint32_t raw);
void ee_cop2_vmsubq(ee_state *ee, uint32_t raw);
void ee_cop2_vsubi(ee_state *ee, uint32_t raw);
void ee_cop2_vmsubi(ee_state *ee, uint32_t raw);
void ee_cop2_vadd(ee_state *ee, uint32_t raw);
void ee_cop2_vmadd(ee_state *ee, uint32_t raw);
void ee_cop2_vmul(ee_state *ee, uint32_t raw);
void ee_cop2_vmax(ee_state *ee, uint32_t raw);
void ee_cop2_vsub(ee_state *ee, uint32_t raw);
void ee_cop2_vmsub(ee_state *ee, uint32_t raw);
void ee_cop2_vopmsub(ee_state *ee, uint32_t raw);
void ee_cop2_vmini(ee_state *ee, uint32_t raw);
void ee_cop2_viadd(ee_state *ee, uint32_t raw);
void ee_cop2_visub(ee_state *ee, uint32_t raw);
void ee_cop2_viaddi(ee_state *ee, uint32_t raw);
void ee_cop2_viand(ee_state *ee, uint32_t raw);
void ee_cop2_vior(ee_state *ee, uint32_t raw);
void ee_cop2_vcallms(ee_state *ee, uint32_t raw);
void ee_cop2_vcallmsr(ee_state *ee, uint32_t raw);
void ee_cop2_vaddabc(ee_state *ee, uint32_t raw);
void ee_cop2_vsubabc(ee_state *ee, uint32_t raw);
void ee_cop2_vmaddabc(ee_state *ee, uint32_t raw);
void ee_cop2_vmsubabc(ee_state *ee, uint32_t raw);
void ee_cop2_vitof(ee_state *ee, uint32_t raw);
void ee_cop2_vftoi(ee_state *ee, uint32_t raw);
void ee_cop2_vmulabc(ee_state *ee, uint32_t raw);
void ee_cop2_vmulaq(ee_state *ee, uint32_t raw);
void ee_cop2_vabs(ee_state *ee, uint32_t raw);
void ee_cop2_vmulai(ee_state *ee, uint32_t raw);
void ee_cop2_vclip(ee_state *ee, uint32_t raw);
void ee_cop2_vaddaq(ee_state *ee, uint32_t raw);
void ee_cop2_vmaddaq(ee_state *ee, uint32_t raw);
void ee_cop2_vaddai(ee_state *ee, uint32_t raw);
void ee_cop2_vmaddai(ee_state *ee, uint32_t raw);
void ee_cop2_vsubaq(ee_state *ee, uint32_t raw);
void ee_cop2_vmsubaq(ee_state *ee, uint32_t raw);
void ee_cop2_vsubai(ee_state *ee, uint32_t raw);
void ee_cop2_vmsubai(ee_state *ee, uint32_t raw);
void ee_cop2_vadda(ee_state *ee, uint32_t raw);
void ee_cop2_vmadda(ee_state *ee, uint32_t raw);
void ee_cop2_vmula(ee_state *ee, uint32_t raw);
void ee_cop2_vsuba(ee_state *ee, uint32_t raw);
void ee_cop2_vmsuba(ee_state *ee, uint32_t raw);
void ee_cop2_vopmula(ee_state *ee, uint32_t raw);
void ee_cop2_vnop(ee_state *ee, uint32_t raw);
void ee_cop2_vmove(ee_state *ee, uint32_t raw);
void ee_cop2_vmr32(ee_state *ee, uint32_t raw);
void ee_cop2_vlqi(ee_state *ee, uint32_t raw);
void ee_cop2_vsqi(ee_state *ee, uint32_t raw);
void ee_cop2_vlqd(ee_state *ee, uint32_t raw);
void ee_cop2_vsqd(ee_state *ee, uint32_t raw);
void ee_cop2_vdiv(ee_state *ee, uint32_t raw);
void ee_cop2_vsqrt(ee_state *ee, uint32_t raw);
void ee_cop2_vrsqrt(ee_state *ee, uint32_t raw);
void ee_cop2_vwaitq(ee_state *ee, uint32_t raw);
void ee_cop2_vmtir(ee_state *ee, uint32_t raw);
void ee_cop2_vmfir(ee_state *ee, uint32_t raw);
void ee_cop2_vilwr(ee_state *ee, uint32_t raw);
void ee_cop2_viswr(ee_state *ee, uint32_t raw);
void ee_cop2_vrnext(ee_state *ee, uint32_t raw);
void ee_cop2_vrget(ee_state *ee, uint32_t raw);
void ee_cop2_vrinit(ee_state *ee, uint32_t raw);
void ee_cop2_vrxor(ee_state *ee, uint32_t raw);
/* VU0 COP2 control-register moves (CFC2/CTC2). */
uint32_t ee_cop2_cfc2(ee_state *ee, int creg);
void     ee_cop2_ctc2(ee_state *ee, int creg, uint32_t val);


/* ---------------------------------------------------------------------
 * 8. BRANCH-DELAY-SLOT CONVENTION  (documentation; no code)
 * ---------------------------------------------------------------------
 * Delay slots are resolved at recompile time, NOT at runtime -- the
 * generated C contains no notion of "delay". The recompiler linearises
 * each function into labelled statements (L_<addr>:) and translates control
 * flow so that observable order matches the hardware:
 *
 *   Ordinary branch (BEQ/BNE/Jxx, condition uses PRE-slot reg values):
 *       uint32_t _c = <condition>;     // snapshot regs before the slot
 *       <delay-slot instruction>;      // always executes
 *       if (_c) goto L_target;
 *
 *   Branch-likely (BEQL/BNEL/BLEZL/BGTZL): delay slot executes ONLY when
 *   the branch is taken:
 *       if (<condition>) { <delay-slot instruction>; goto L_target; }
 *
 *   JR $ra (return) / JALR: snapshot nothing, emit the delay slot, then
 *   `return;` (or the indirect-call sequence) so the slot runs first.
 *
 * The condition snapshot into a temp is mandatory because the delay-slot
 * instruction may overwrite a register the condition reads (common on the
 * R5900). This is the single rule the whole recompiler relies on; see
 * tools/recompile.py:emit_branch().
 */

/* =====================================================================
 * 9. COP1 / FPU  --  EE single-precision rules (no NaN/Inf, denorm flush)
 * ---------------------------------------------------------------------
 * The EE FPU is NOT IEEE-754: there are no NaNs or infinities, overflow
 * saturates to +-FLT_MAX (bit pattern 0x7FFFFFFF / 0xFFFFFFFF), and
 * denormals are flushed to a signed zero. These helpers reproduce that,
 * lifted from PS2Recomp's verified ps2_float_semantics implementation and
 * ported to C11.  f[] registers are stored as raw uint32_t bit patterns;
 * every helper takes and returns bit patterns so the quirks live here.
 * The recompiler emits e.g.  ee->f[fd] = ee_fadd(ee->f[fs], ee->f[ft]);
 * ===================================================================== */
#include <math.h>

#define EE_F_MAXBITS 0x7FFFFFFFu
#define EE_F_EXPMASK 0x7F800000u
#define EE_F_SIGN    0x80000000u

static inline float    ee_b2f(uint32_t b){ float f; memcpy(&f,&b,4); return f; }
static inline uint32_t ee_f2b(float f){ uint32_t b; memcpy(&b,&f,4); return b; }

static inline int ee_fspecial(float x){
    uint32_t t = ee_f2b(x) & 0x7FFFFFFFu;
    return t != 0u && (t - 0x00800000u) >= 0x7F000000u;
}
static inline double ee_to_d(float x){
    uint32_t b = ee_f2b(x), e = b & EE_F_EXPMASK;
    if (e == 0u)          return (double)ee_b2f(b & EE_F_SIGN);
    if (e == EE_F_EXPMASK) return (double)ee_b2f(b - 0x00800000u) * 2.0;
    return (double)x;
}
static inline float ee_from_d(double d){
    double m = fabs(d); uint32_t s = signbit(d) ? EE_F_SIGN : 0u;
    if (m >= 0x1p129) return ee_b2f(s | EE_F_MAXBITS);
    if (m <  0x1p-126) return ee_b2f(s);
    if (m >= 0x1p128){
        uint32_t b = ee_f2b((float)(d * 0.5));
        if ((b & EE_F_EXPMASK) == EE_F_EXPMASK) return ee_b2f(s | EE_F_MAXBITS);
        return ee_b2f(b + 0x00800000u);
    }
    return (float)d;
}
static inline int ee_fnormal(float r){
    return (ee_f2b(r) * 2u + 0x01000000u) >= 0x02000000u;
}
static inline int ee_fall_normal(float a, float b, float r){
    uint32_t sa = ee_f2b(a)*2u + 0x01000000u;
    uint32_t sb = ee_f2b(b)*2u + 0x01000000u;
    uint32_t sr = ee_f2b(r)*2u + 0x01000000u;
    uint32_t lo = sa < sb ? sa : sb; lo = lo < sr ? lo : sr;
    return lo >= 0x02000000u;
}
static inline uint32_t ee_fadd(uint32_t ab, uint32_t bb){
    float a=ee_b2f(ab), b=ee_b2f(bb), r=a+b;
    if (ee_fnormal(r)) return ee_f2b(r);
    if (!(ee_fspecial(a)|ee_fspecial(b)|ee_fspecial(r))) return ee_f2b(r);
    return ee_f2b(ee_from_d(ee_to_d(a)+ee_to_d(b)));
}
static inline uint32_t ee_fsub(uint32_t ab, uint32_t bb){
    float a=ee_b2f(ab), b=ee_b2f(bb), r=a-b;
    if (ee_fnormal(r)) return ee_f2b(r);
    if (!(ee_fspecial(a)|ee_fspecial(b)|ee_fspecial(r))) return ee_f2b(r);
    return ee_f2b(ee_from_d(ee_to_d(a)-ee_to_d(b)));
}
static inline uint32_t ee_fmul(uint32_t ab, uint32_t bb){
    float a=ee_b2f(ab), b=ee_b2f(bb), r=a*b;
    if (ee_fnormal(r)) return ee_f2b(r);
    if (!(ee_fspecial(a)|ee_fspecial(b)|ee_fspecial(r))) return ee_f2b(r);
    return ee_f2b(ee_from_d(ee_to_d(a)*ee_to_d(b)));
}
static inline uint32_t ee_fdiv(uint32_t ab, uint32_t bb){
    float a=ee_b2f(ab), b=ee_b2f(bb), r=a/b;
    if (ee_fall_normal(a,b,r)) return ee_f2b(r);
    if ((bb & EE_F_EXPMASK) == 0u) return ((ab^bb)&EE_F_SIGN) | EE_F_MAXBITS;
    if (!(ee_fspecial(a)|ee_fspecial(b)|ee_fspecial(r))) return ee_f2b(r);
    return ee_f2b(ee_from_d(ee_to_d(a)/ee_to_d(b)));
}
static inline uint32_t ee_fsqrt(uint32_t ab){
    float a=ee_b2f(ab), r=sqrtf(a);
    if (ee_fall_normal(a,a,r)) return ee_f2b(r);
    if ((ab & EE_F_EXPMASK) == 0u) return 0u;
    return ee_f2b(ee_from_d(sqrt(fabs(ee_to_d(a)))));
}
static inline uint32_t ee_frsqrt(uint32_t ab, uint32_t bb){
    float a=ee_b2f(ab), b=ee_b2f(bb), r=a/sqrtf(b);
    if (ee_fall_normal(a,b,r)) return ee_f2b(r);
    if ((bb & EE_F_EXPMASK) == 0u) return (ab & EE_F_SIGN) | EE_F_MAXBITS;
    float root = ee_from_d(sqrt(fabs(ee_to_d(b))));
    return ee_f2b(ee_from_d(ee_to_d(a)/(double)root));
}
static inline uint32_t ee_fabs(uint32_t ab){ return ab & 0x7FFFFFFFu; }
static inline uint32_t ee_fneg(uint32_t ab){ return ab ^ EE_F_SIGN; }
static inline uint32_t ee_fmax(uint32_t ab, uint32_t bb){ float a=ee_b2f(ab),b=ee_b2f(bb); return ee_f2b(a>b?a:b); }
static inline uint32_t ee_fmin(uint32_t ab, uint32_t bb){ float a=ee_b2f(ab),b=ee_b2f(bb); return ee_f2b(a<b?a:b); }
/* saturating float->int32 (EE clamps out-of-range to 0x7FFFFFFF/0x80000000) */
static inline uint32_t ee_f2i_sat(double v){
    if (v >=  2147483647.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint32_t ee_cvt_w_s   (uint32_t ab){ return ee_f2i_sat((double)nearbyintf(ee_b2f(ab))); }
static inline uint32_t ee_trunc_w_s (uint32_t ab){ return ee_f2i_sat(truncf(ee_b2f(ab))); }
static inline uint32_t ee_round_w_s (uint32_t ab){ return ee_f2i_sat(roundf(ee_b2f(ab))); }
static inline uint32_t ee_ceil_w_s  (uint32_t ab){ return ee_f2i_sat(ceilf(ee_b2f(ab))); }
static inline uint32_t ee_floor_w_s (uint32_t ab){ return ee_f2i_sat(floorf(ee_b2f(ab))); }
static inline uint32_t ee_cvt_s_w   (uint32_t wb){ int32_t w; memcpy(&w,&wb,4); return ee_f2b((float)w); }
/* FPU condition compares: return 1 if the EE C bit should be SET. No NaN/Inf,
 * so ordered/unordered variants collapse to the same numeric comparison. */
static inline int ee_fc_eq(uint32_t ab,uint32_t bb){ return ee_b2f(ab)==ee_b2f(bb); }
static inline int ee_fc_lt(uint32_t ab,uint32_t bb){ return ee_b2f(ab)< ee_b2f(bb); }
static inline int ee_fc_le(uint32_t ab,uint32_t bb){ return ee_b2f(ab)<=ee_b2f(bb); }
#define EE_FCR31_C 0x00800000u
static inline void ee_fset_c(ee_state *ee, int set){
    if (set) ee->fcr31 |= EE_FCR31_C; else ee->fcr31 &= ~EE_FCR31_C;
}

/* =====================================================================
 * 10. MMI  --  128-bit parallel integer ops (R5900-specific)
 * ---------------------------------------------------------------------
 * GPRs are the ee_reg lane union, so MMI ops are expressed as portable
 * scalar lane loops (no host SIMD dependency). Each helper writes a temp
 * then copies out, so an in-place rd==rs / rd==rt is safe. Semantics per
 * the EE Core User's Manual; the pack/interleave lane order follows the
 * standard PEXTL/PPAC/PCPY conventions. Correctness of PCPYH/PCPYLD (used
 * by the engine memset FUN_00447c30) and the add/sub/compare families is
 * covered by the differential tests; the long-tail ops are implemented to
 * the documented spec but are not each individually hardware-verified.
 * ===================================================================== */
static inline int16_t ee_sat_s16(int32_t v){ return v>32767?32767:(v<-32768?-32768:(int16_t)v); }
static inline int8_t  ee_sat_s8 (int32_t v){ return v>127?127:(v<-128?-128:(int8_t)v); }
static inline int32_t ee_sat_s32(int64_t v){ return v>2147483647LL?2147483647:(v<(-2147483647LL-1)?(-2147483647-1):(int32_t)v); }
static inline uint32_t ee_sat_u32(int64_t v){ return v>0xFFFFFFFFLL?0xFFFFFFFFu:(v<0?0u:(uint32_t)v); }
static inline uint16_t ee_sat_u16(int32_t v){ return v>0xFFFF?0xFFFF:(v<0?0:(uint16_t)v); }
static inline uint8_t  ee_sat_u8 (int32_t v){ return v>0xFF?0xFF:(v<0?0:(uint8_t)v); }

/* ---- MMI0 (parallel add/sub/cmp/max + saturating + pack/extend low) --- */
static inline void ee_mmi_paddw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.sw[i]=s->sw[i]+t->sw[i]; *d=o; }
static inline void ee_mmi_psubw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.sw[i]=s->sw[i]-t->sw[i]; *d=o; }
static inline void ee_mmi_pcgtw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.uw[i]=s->sw[i]>t->sw[i]?0xFFFFFFFFu:0; *d=o; }
static inline void ee_mmi_pmaxw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.sw[i]=s->sw[i]>t->sw[i]?s->sw[i]:t->sw[i]; *d=o; }
static inline void ee_mmi_paddh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.sh[i]=(int16_t)(s->sh[i]+t->sh[i]); *d=o; }
static inline void ee_mmi_psubh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.sh[i]=(int16_t)(s->sh[i]-t->sh[i]); *d=o; }
static inline void ee_mmi_pcgth(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.uh[i]=s->sh[i]>t->sh[i]?0xFFFFu:0; *d=o; }
static inline void ee_mmi_pmaxh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.sh[i]=s->sh[i]>t->sh[i]?s->sh[i]:t->sh[i]; *d=o; }
static inline void ee_mmi_paddb(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<16;i++) o.sb[i]=(int8_t)(s->sb[i]+t->sb[i]); *d=o; }
static inline void ee_mmi_psubb(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<16;i++) o.sb[i]=(int8_t)(s->sb[i]-t->sb[i]); *d=o; }
static inline void ee_mmi_pcgtb(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<16;i++) o.ub[i]=s->sb[i]>t->sb[i]?0xFFu:0; *d=o; }
static inline void ee_mmi_paddsw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.sw[i]=ee_sat_s32((int64_t)s->sw[i]+t->sw[i]); *d=o; }
static inline void ee_mmi_psubsw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.sw[i]=ee_sat_s32((int64_t)s->sw[i]-t->sw[i]); *d=o; }
static inline void ee_mmi_paddsh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.sh[i]=ee_sat_s16((int32_t)s->sh[i]+t->sh[i]); *d=o; }
static inline void ee_mmi_psubsh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.sh[i]=ee_sat_s16((int32_t)s->sh[i]-t->sh[i]); *d=o; }
static inline void ee_mmi_paddsb(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<16;i++) o.sb[i]=ee_sat_s8((int32_t)s->sb[i]+t->sb[i]); *d=o; }
static inline void ee_mmi_psubsb(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<16;i++) o.sb[i]=ee_sat_s8((int32_t)s->sb[i]-t->sb[i]); *d=o; }
static inline void ee_mmi_pextlw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.uw[0]=t->uw[0];o.uw[1]=s->uw[0];o.uw[2]=t->uw[1];o.uw[3]=s->uw[1]; *d=o; }
static inline void ee_mmi_pextlh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++){o.uh[2*i]=t->uh[i];o.uh[2*i+1]=s->uh[i];} *d=o; }
static inline void ee_mmi_pextlb(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++){o.ub[2*i]=t->ub[i];o.ub[2*i+1]=s->ub[i];} *d=o; }
static inline void ee_mmi_ppacw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.uw[0]=t->uw[0];o.uw[1]=t->uw[2];o.uw[2]=s->uw[0];o.uw[3]=s->uw[2]; *d=o; }
static inline void ee_mmi_ppach(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++){o.uh[i]=t->uh[2*i];o.uh[i+4]=s->uh[2*i];} *d=o; }
static inline void ee_mmi_ppacb(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++){o.ub[i]=t->ub[2*i];o.ub[i+8]=s->ub[2*i];} *d=o; }

/* ---- MMI1 (abs/cmp-eq/min + unsigned sat + extend-high + qfsrv) ------- */
static inline void ee_mmi_pabsw(ee_reg*d,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++){int32_t v=t->sw[i]; o.sw[i]=(v==INT32_MIN)?INT32_MAX:(v<0?-v:v);} *d=o; }
static inline void ee_mmi_pabsh(ee_reg*d,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++){int16_t v=t->sh[i]; o.sh[i]=(v==INT16_MIN)?INT16_MAX:(v<0?(int16_t)-v:v);} *d=o; }
static inline void ee_mmi_pceqw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.uw[i]=s->uw[i]==t->uw[i]?0xFFFFFFFFu:0; *d=o; }
static inline void ee_mmi_pceqh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.uh[i]=s->uh[i]==t->uh[i]?0xFFFFu:0; *d=o; }
static inline void ee_mmi_pceqb(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<16;i++) o.ub[i]=s->ub[i]==t->ub[i]?0xFFu:0; *d=o; }
static inline void ee_mmi_pminw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.sw[i]=s->sw[i]<t->sw[i]?s->sw[i]:t->sw[i]; *d=o; }
static inline void ee_mmi_pminh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.sh[i]=s->sh[i]<t->sh[i]?s->sh[i]:t->sh[i]; *d=o; }
static inline void ee_mmi_padduw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.uw[i]=ee_sat_u32((int64_t)s->uw[i]+t->uw[i]); *d=o; }
static inline void ee_mmi_psubuw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.uw[i]=ee_sat_u32((int64_t)s->uw[i]-t->uw[i]); *d=o; }
static inline void ee_mmi_padduh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.uh[i]=ee_sat_u16((int32_t)s->uh[i]+t->uh[i]); *d=o; }
static inline void ee_mmi_psubuh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++) o.uh[i]=ee_sat_u16((int32_t)s->uh[i]-t->uh[i]); *d=o; }
static inline void ee_mmi_paddub(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<16;i++) o.ub[i]=ee_sat_u8((int32_t)s->ub[i]+t->ub[i]); *d=o; }
static inline void ee_mmi_psubub(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<16;i++) o.ub[i]=ee_sat_u8((int32_t)s->ub[i]-t->ub[i]); *d=o; }
static inline void ee_mmi_pextuw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.uw[0]=t->uw[2];o.uw[1]=s->uw[2];o.uw[2]=t->uw[3];o.uw[3]=s->uw[3]; *d=o; }
static inline void ee_mmi_pextuh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++){o.uh[2*i]=t->uh[i+4];o.uh[2*i+1]=s->uh[i+4];} *d=o; }
static inline void ee_mmi_pextub(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<8;i++){o.ub[2*i]=t->ub[i+8];o.ub[2*i+1]=s->ub[i+8];} *d=o; }
static inline void ee_mmi_padsbh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.sh[i]=(int16_t)(s->sh[i]-t->sh[i]); for(int i=4;i<8;i++) o.sh[i]=(int16_t)(s->sh[i]+t->sh[i]); *d=o; }
/* QFSRV: funnel-shift right of {rs:rt} by ee->sa bits (byte-granular). */
static inline void ee_mmi_qfsrv(ee_reg*d,const ee_reg*s,const ee_reg*t,uint32_t sa){
    unsigned char buf[32]; memcpy(buf,t->ub,16); memcpy(buf+16,s->ub,16);
    unsigned sh=(sa&0x7F)>>3; ee_reg o; for(int i=0;i<16;i++) o.ub[i]=buf[i+sh]; *d=o;
}

/* ---- MMI2 (pmfhi/lo, interleave, pand/pxor, copy-low-dword, exew/rot3) - */
static inline void ee_mmi_pand(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.ud[0]=s->ud[0]&t->ud[0]; o.ud[1]=s->ud[1]&t->ud[1]; *d=o; }
static inline void ee_mmi_pxor(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.ud[0]=s->ud[0]^t->ud[0]; o.ud[1]=s->ud[1]^t->ud[1]; *d=o; }
static inline void ee_mmi_por (ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.ud[0]=s->ud[0]|t->ud[0]; o.ud[1]=s->ud[1]|t->ud[1]; *d=o; }
static inline void ee_mmi_pnor(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.ud[0]=~(s->ud[0]|t->ud[0]); o.ud[1]=~(s->ud[1]|t->ud[1]); *d=o; }
static inline void ee_mmi_pcpyld(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.ud[0]=t->ud[0]; o.ud[1]=s->ud[0]; *d=o; }
static inline void ee_mmi_pcpyud(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.ud[0]=s->ud[1]; o.ud[1]=t->ud[1]; *d=o; }
static inline void ee_mmi_pcpyh (ee_reg*d,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.uh[i]=t->uh[0]; for(int i=4;i<8;i++) o.uh[i]=t->uh[4]; *d=o; }
static inline void ee_mmi_pinth (ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++){o.uh[2*i]=t->uh[i];o.uh[2*i+1]=s->uh[i+4];} *d=o; }
static inline void ee_mmi_pinteh(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++){o.uh[2*i]=t->uh[2*i];o.uh[2*i+1]=s->uh[2*i];} *d=o; }
static inline void ee_mmi_pexeh (ee_reg*d,const ee_reg*t){ ee_reg o=*t; uint16_t a; a=o.uh[0];o.uh[0]=o.uh[2];o.uh[2]=a; a=o.uh[4];o.uh[4]=o.uh[6];o.uh[6]=a; *d=o; }
static inline void ee_mmi_pexew (ee_reg*d,const ee_reg*t){ ee_reg o=*t; uint32_t a=o.uw[0]; o.uw[0]=o.uw[2]; o.uw[2]=a; *d=o; }
static inline void ee_mmi_prevh (ee_reg*d,const ee_reg*t){ ee_reg o; for(int i=0;i<4;i++) o.uh[i]=t->uh[3-i]; for(int i=0;i<4;i++) o.uh[i+4]=t->uh[7-i]; *d=o; }
static inline void ee_mmi_prot3w(ee_reg*d,const ee_reg*t){ ee_reg o; o.uw[0]=t->uw[1]; o.uw[1]=t->uw[2]; o.uw[2]=t->uw[0]; o.uw[3]=t->uw[3]; *d=o; }
static inline void ee_mmi_pexcw (ee_reg*d,const ee_reg*t){ ee_reg o; o.uw[0]=t->uw[0]; o.uw[1]=t->uw[2]; o.uw[2]=t->uw[1]; o.uw[3]=t->uw[3]; *d=o; }
static inline void ee_mmi_pexch (ee_reg*d,const ee_reg*t){ ee_reg o=*t; uint16_t a; a=o.uh[1];o.uh[1]=o.uh[2];o.uh[2]=a; a=o.uh[5];o.uh[5]=o.uh[6];o.uh[6]=a; *d=o; }
/* parallel variable word shift: lanes 0,2 shifted by rs lanes 0,2; sign/zero
 * extended into the 64-bit halves (approximation of PSLLVW/PSRLVW/PSRAVW). */
static inline void ee_mmi_psllvw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.sd[0]=(int32_t)(t->uw[0]<<(s->uw[0]&31)); o.sd[1]=(int32_t)(t->uw[2]<<(s->uw[2]&31)); *d=o; }
static inline void ee_mmi_psrlvw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.sd[0]=(int32_t)(t->uw[0]>>(s->uw[0]&31)); o.sd[1]=(int32_t)(t->uw[2]>>(s->uw[2]&31)); *d=o; }
static inline void ee_mmi_psravw(ee_reg*d,const ee_reg*s,const ee_reg*t){ ee_reg o; o.sd[0]=(int32_t)(t->sw[0]>>(s->uw[0]&31)); o.sd[1]=(int32_t)(t->sw[2]>>(s->uw[2]&31)); *d=o; }

/* ---- parallel immediate shifts (sa) ---------------------------------- */
static inline void ee_mmi_psllh(ee_reg*d,const ee_reg*t,uint32_t sa){ ee_reg o; for(int i=0;i<8;i++) o.uh[i]=(uint16_t)(t->uh[i]<<(sa&15)); *d=o; }
static inline void ee_mmi_psrlh(ee_reg*d,const ee_reg*t,uint32_t sa){ ee_reg o; for(int i=0;i<8;i++) o.uh[i]=(uint16_t)(t->uh[i]>>(sa&15)); *d=o; }
static inline void ee_mmi_psrah(ee_reg*d,const ee_reg*t,uint32_t sa){ ee_reg o; for(int i=0;i<8;i++) o.sh[i]=(int16_t)(t->sh[i]>>(sa&15)); *d=o; }
static inline void ee_mmi_psllw(ee_reg*d,const ee_reg*t,uint32_t sa){ ee_reg o; for(int i=0;i<4;i++) o.sw[i]=(int32_t)(t->uw[i]<<(sa&31)); *d=o; }
static inline void ee_mmi_psrlw(ee_reg*d,const ee_reg*t,uint32_t sa){ ee_reg o; for(int i=0;i<4;i++) o.sw[i]=(int32_t)(t->uw[i]>>(sa&31)); *d=o; }
static inline void ee_mmi_psraw(ee_reg*d,const ee_reg*t,uint32_t sa){ ee_reg o; for(int i=0;i<4;i++) o.sw[i]=(int32_t)(t->sw[i]>>(sa&31)); *d=o; }

/* ---- PLZCW (parallel leading sign-bit count, per 32-bit word) -------- */
static inline uint32_t ee_plzcw32(uint32_t x){ uint32_t s=x>>31; x=s?~x:x; uint32_t n=0; for(int b=30;b>=0;b--){ if(x&(1u<<b)) break; n++; } return n; }

/* ---- PMFHL / PMTHL (parallel move from/to HI/LO) --------------------- */
static inline void ee_mmi_pmfhl(ee_state*ee,int rd,uint32_t sa){
    if(!rd) return;
    ee_reg o; const ee_reg*hi=&ee->hi,*lo=&ee->lo;
    switch(sa){
    case 0x00: /* LW */ o.uw[0]=lo->uw[0];o.uw[1]=hi->uw[0];o.uw[2]=lo->uw[2];o.uw[3]=hi->uw[2]; break;
    case 0x01: /* UW */ o.uw[0]=lo->uw[1];o.uw[1]=hi->uw[1];o.uw[2]=lo->uw[3];o.uw[3]=hi->uw[3]; break;
    case 0x02: /* SLW: saturated 64-bit of {hi0:lo0} and {hi2:lo2} */ {
        int64_t a=((int64_t)hi->sw[0]<<32)|lo->uw[0]; int64_t b=((int64_t)hi->sw[2]<<32)|lo->uw[2];
        o.sd[0]=ee_sat_s32(a); o.sd[1]=ee_sat_s32(b); break; }
    case 0x03: /* LH */ o.uh[0]=lo->uh[0];o.uh[1]=lo->uh[2];o.uh[2]=hi->uh[0];o.uh[3]=hi->uh[2];
                        o.uh[4]=lo->uh[4];o.uh[5]=lo->uh[6];o.uh[6]=hi->uh[4];o.uh[7]=hi->uh[6]; break;
    default: /* 0x04 SH: saturate each of the 4 words of LO/HI to s16 */
        o.sh[0]=ee_sat_s16(lo->sw[0]);o.sh[1]=ee_sat_s16(lo->sw[1]);o.sh[2]=ee_sat_s16(hi->sw[0]);o.sh[3]=ee_sat_s16(hi->sw[1]);
        o.sh[4]=ee_sat_s16(lo->sw[2]);o.sh[5]=ee_sat_s16(lo->sw[3]);o.sh[6]=ee_sat_s16(hi->sw[2]);o.sh[7]=ee_sat_s16(hi->sw[3]); break;
    }
    ee->r[rd]=o;
}
static inline void ee_mmi_pmthl_lw(ee_state*ee,int rs){ ee->lo.uw[0]=ee->r[rs].uw[0]; ee->hi.uw[0]=ee->r[rs].uw[1]; ee->lo.uw[2]=ee->r[rs].uw[2]; ee->hi.uw[2]=ee->r[rs].uw[3]; }

/* ---- parallel multiply/divide into HI/LO ----------------------------------
 * These mirror the ran-j/PS2Recomp reference (the named semantics oracle) and
 * are best-effort: faithful lifts, NOT independently hardware-verified. Of the
 * family only PMADDH/PMULTH/PMULTW/PDIVBW actually occur in this ELF. */
#define EE__HILO(ee) ((int64_t)(((uint64_t)(ee)->hi.uw[0]<<32)|(ee)->lo.uw[0]))
static inline int64_t ee__madd16(const ee_reg*a,const ee_reg*b,int j){
    return (int64_t)((int32_t)a->sh[2*j]*(int32_t)b->sh[2*j] + (int32_t)a->sh[2*j+1]*(int32_t)b->sh[2*j+1]); }
static inline void ee__setlo32(ee_state*ee,int64_t acc){ ee->lo.sd[0]=(int32_t)(uint32_t)acc; ee->hi.sd[0]=(int32_t)(uint32_t)((uint64_t)acc>>32); }
static inline void ee_mmi_pmaddh(ee_state*ee,int rd,int rs,int rt){ const ee_reg*s=&ee->r[rs],*t=&ee->r[rt];
    int64_t acc=EE__HILO(ee)+ee__madd16(s,t,0)+ee__madd16(s,t,1)+ee__madd16(s,t,2)+ee__madd16(s,t,3);
    ee__setlo32(ee,acc); if(rd) ee->r[rd].ud[0]=(uint64_t)acc; }
static inline void ee_mmi_pmsubh(ee_state*ee,int rd,int rs,int rt){ const ee_reg*s=&ee->r[rs],*t=&ee->r[rt];
    int64_t acc=EE__HILO(ee)-(ee__madd16(s,t,0)+ee__madd16(s,t,1)+ee__madd16(s,t,2)+ee__madd16(s,t,3));
    ee__setlo32(ee,acc); if(rd) ee->r[rd].ud[0]=(uint64_t)acc; }
static inline void ee_mmi_pmulth(ee_state*ee,int rd,int rs,int rt){ const ee_reg*s=&ee->r[rs],*t=&ee->r[rt];
    int64_t r=ee__madd16(s,t,0)+ee__madd16(s,t,1)+ee__madd16(s,t,2)+ee__madd16(s,t,3);
    ee__setlo32(ee,r); if(rd) ee->r[rd].ud[0]=(uint64_t)r; }
static inline void ee_mmi_phmadh(ee_state*ee,int rd,int rs,int rt){ const ee_reg*s=&ee->r[rs],*t=&ee->r[rt];
    int64_t acc=EE__HILO(ee); for(int j=0;j<4;j++) acc+=(int64_t)((int32_t)s->sh[2*j]*t->sh[2*j]+(int32_t)s->sh[2*j+1]*t->sh[2*j+1]);
    ee__setlo32(ee,acc); if(rd) ee->r[rd].ud[0]=(uint64_t)acc; }
static inline void ee_mmi_phmsbh(ee_state*ee,int rd,int rs,int rt){ const ee_reg*s=&ee->r[rs],*t=&ee->r[rt];
    int64_t acc=EE__HILO(ee); for(int j=0;j<4;j++) acc-=(int64_t)((int32_t)s->sh[2*j]*t->sh[2*j]+(int32_t)s->sh[2*j+1]*t->sh[2*j+1]);
    ee__setlo32(ee,acc); if(rd) ee->r[rd].ud[0]=(uint64_t)acc; }
static inline void ee__pmacc_w(ee_state*ee,int rd,int rs,int rt,int sub,int useacc){ const ee_reg*s=&ee->r[rs],*t=&ee->r[rt];
    uint64_t p = (uint64_t)s->uw[0]*t->uw[0] + 2ull*((uint64_t)s->uw[2]*t->uw[2]);
    int64_t acc = useacc?EE__HILO(ee):0; acc = sub? acc-(int64_t)p : acc+(int64_t)p;
    ee__setlo32(ee,acc); if(rd) ee->r[rd].ud[0]=(uint64_t)acc; }
static inline void ee_mmi_pmaddw(ee_state*ee,int rd,int rs,int rt){ ee__pmacc_w(ee,rd,rs,rt,0,1); }
static inline void ee_mmi_pmsubw(ee_state*ee,int rd,int rs,int rt){ ee__pmacc_w(ee,rd,rs,rt,1,1); }
static inline void ee_mmi_pmultw(ee_state*ee,int rd,int rs,int rt){ ee__pmacc_w(ee,rd,rs,rt,0,0); }
static inline void ee_mmi_pmadduw(ee_state*ee,int rd,int rs,int rt){ ee__pmacc_w(ee,rd,rs,rt,0,1); }
static inline void ee_mmi_pmultuw(ee_state*ee,int rd,int rs,int rt){ const ee_reg*s=&ee->r[rs],*t=&ee->r[rt];
    uint64_t r0=(uint64_t)s->uw[0]*t->uw[0], r2=(uint64_t)s->uw[2]*t->uw[2];
    ee->lo.sd[0]=(int32_t)(uint32_t)r0; ee->hi.sd[0]=(int32_t)(uint32_t)(r0>>32);
    if(rd){ ee->r[rd].uw[0]=(uint32_t)r0; ee->r[rd].uw[1]=(uint32_t)(r0>>32); ee->r[rd].uw[2]=(uint32_t)r2; ee->r[rd].uw[3]=(uint32_t)(r2>>32);} }
static inline void ee_mmi_pdivw(ee_state*ee,int rd,int rs,int rt){ int32_t a=ee->r[rs].sw[0],b=ee->r[rt].sw[0];
    if(b){ if(b==-1&&a==INT32_MIN){ee->lo.sd[0]=INT32_MIN;ee->hi.sd[0]=0;} else {ee->lo.sd[0]=a/b;ee->hi.sd[0]=a%b;} }
    else { ee->lo.sd[0]=(a<0)?1:-1; ee->hi.sd[0]=a; } if(rd) ee->r[rd].sd[0]=ee->lo.sd[0]; }
static inline void ee_mmi_pdivuw(ee_state*ee,int rd,int rs,int rt){ uint32_t a=ee->r[rs].uw[0],b=ee->r[rt].uw[0];
    if(b){ ee->lo.sd[0]=(int32_t)(a/b); ee->hi.sd[0]=(int32_t)(a%b);} else { ee->lo.sd[0]=-1; ee->hi.sd[0]=(int32_t)a; }
    if(rd) ee->r[rd].sd[0]=ee->lo.sd[0]; }
static inline void ee_mmi_pdivbw(ee_state*ee,int rd,int rs,int rt){ int32_t div=ee->r[rt].sw[0]; ee_reg o; const ee_reg*s=&ee->r[rs];
    if(div){ ee->lo.sd[0]=s->sw[0]/div; ee->hi.sd[0]=s->sw[0]%div; for(int i=0;i<4;i++) o.sw[i]=s->sw[i]/div; }
    else { ee->lo.sd[0]=(s->sw[0]<0)?1:-1; ee->hi.sd[0]=s->sw[0]; for(int i=0;i<4;i++) o.sw[i]=0; }
    if(rd) ee->r[rd]=o; }

#ifdef __cplusplus
}
#endif
#endif /* EE_RUNTIME_H */
