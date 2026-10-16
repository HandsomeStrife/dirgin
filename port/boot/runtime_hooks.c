/* =====================================================================
 * runtime_hooks.c  --  the ee_runtime.h trap-out hooks that the INTEGRATED
 * build still needs but that are NOT owned by hle/ (mmio+syscall), cop2/
 * (VU0), sif/ (RPC) or the generated function table (ee_lookup).
 *
 * WHY THIS FILE EXISTS (seam resolution, documented):
 *   recomp/ee_runtime.c is the standalone PROTOTYPE runtime.  It defines
 *   stub bodies for ee_mmio_* / ee_syscall / ee_break / ee_trap_unimpl
 *   (really owned by hle/), for ee_vu0_state / ee_cop2_qmtc2 / ee_cop2_qmfc2
 *   (really owned by cop2/), and for the COP0 + barrier hooks + the guest
 *   machine mmap helpers.  Linking recomp/ee_runtime.c into the integrated
 *   program would therefore duplicate every symbol hle/ and cop2/ provide.
 *   So we do NOT link ee_runtime.c; instead this file supplies exactly the
 *   handful of hooks that are otherwise unprovided:
 *       ee_cop0_mfc0/mtc0, ee_ei, ee_di, ee_eret, ee_sync   (COP0/barriers)
 *       ee_call_indirect                                    (computed calls)
 *   The COP0 bodies are the contract-mandated behaviour (identical to the
 *   prototype): MFC0 reads back ee->cop0_*, so Status's spin bit (0x10000)
 *   reads CLEAR and the boot frame-advance loop FUN_00241478 terminates.
 * ===================================================================== */
#include "ee_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>
void vu0_macro_dump_stats(FILE *f);   /* cop2/vu0_macro.c */

/* ---- COP0 (System Control) ------------------------------------------ */
uint32_t ee_cop0_mfc0(ee_state *ee, int reg){
    switch (reg){
    case 9:  return ee->cop0_count;
    case 12: return ee->cop0_status;   /* spin bit 0x10000 starts/reads clear */
    case 13: return ee->cop0_cause;
    case 14: return ee->cop0_epc;
    default: return 0;
    }
}
void ee_cop0_mtc0(ee_state *ee, int reg, uint32_t val){
    switch (reg){
    case 9:  ee->cop0_count  = val; break;
    case 12: ee->cop0_status = val; break;
    case 13: ee->cop0_cause  = val; break;
    case 14: ee->cop0_epc    = val; break;
    default: break;
    }
}
/* EI/DI toggle the EE's EIE bit (Status 0x10000) as well as IE (0x1).  Bit
 * 0x10000 is the dynamic "interrupts enabled" state the boot reads BOTH ways:
 *   - FUN_00241478/FUN_00241508 do `DI(); while(Status&0x10000);` to enter a
 *     critical section (needs it CLEAR after DI -> terminates), and
 *   - FUN_00249084 (the file-subsystem yield pump) does
 *     `if((Status&0x10000)==0) return error;` i.e. will only WaitSema/yield
 *     when interrupts are ENABLED (needs it SET in EI state).
 * The prototype hardcoded this bit clear, which satisfied the first but made
 * the second never yield (infinite busy-wait for a worker thread).  Modelling
 * EIE on EI/DI satisfies both.  (Integration bug fix.) */
void ee_ei  (ee_state *ee){ ee->cop0_status |=  (0x1u | 0x10000u); }
void ee_di  (ee_state *ee){ ee->cop0_status &= ~(0x1u | 0x10000u); }
void ee_eret(ee_state *ee){ (void)ee; }                   /* no exception model */

/* SYNC is a barrier/yield point.  The HLE spin loops that matter (vblank
 * WaitSema, DMA STR busy-poll) make progress through the scheduler and the
 * synchronous DMAC, so SYNC needs no extra pumping here. */
void ee_sync(ee_state *ee, uint32_t stype){ (void)ee; (void)stype; }

/* ---- indirect / computed call --------------------------------------- *
 * Resolve through the generated function table.  An unknown target is a
 * real integration blocker (a vtable/callback the recompiler could not
 * enumerate): log it loudly with the call site PC so the report can name
 * it, then abort rather than silently skipping (which would corrupt state).
 */
void ee_call_indirect(ee_state *ee, uint32_t guest_addr){
    if (getenv("DIRGE_WATCH46")){
        static int w46=0;
        if (!w46){ uint32_t v=ee_lw(ee,0x0046ba1c);
            if (v){ w46=1; fprintf(stderr,"[watch46] DAT_0046ba1c=%08x at indirect-call; bt:\n",v);
                void *b[48]; int n=backtrace(b,48); backtrace_symbols_fd(b,n,2);} }
    }
    ee_func_t f = ee_lookup(guest_addr);
    if (!f){
        /* DIAGNOSTIC (DIRGE_SOFT_INDIRECT): an unknown computed-call target is
         * normally a hard error, but to measure how far the MAIN boot thread
         * reaches past a crashing CONCURRENT service thread, this makes it a
         * logged no-op (returns $v0=-1) instead of aborting.  NOT a real fix. */
        static int soft = -1;
        if (soft < 0) soft = getenv("DIRGE_SOFT_INDIRECT") ? 1 : 0;
        fprintf(stderr, "[boot] indirect call to unknown guest addr %08x "
                        "($ra=%08x v0=%08x)%s\n",
                        guest_addr, EE_GET32(ee,R_RA), EE_GET32(ee,R_V0),
                        soft ? " -> soft no-op" : " FATAL");
        if (soft){ EE_SET32(ee,R_V0,(uint32_t)-1); return; }
        void *bt[64]; int n = backtrace(bt, 64);
        backtrace_symbols_fd(bt, n, 2);
        vu0_macro_dump_stats(stderr);
        fflush(stderr);
        abort();
    }
    f(ee);
}
