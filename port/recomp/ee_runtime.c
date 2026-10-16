/* =====================================================================
 * ee_runtime.c -- reference/stub implementation of the WS-R runtime hooks.
 *
 * This is the PROTOTYPE runtime: it provides a working guest-memory model
 * and loud stubs for the trap-out points (MMIO / syscall / VU0 / indirect
 * call). The real port replaces the stub bodies with hle/, gs/, vu/ -- the
 * SIGNATURES here are the frozen contract; the bodies are placeholders.
 * ===================================================================== */
#define _DEFAULT_SOURCE   /* for MAP_ANONYMOUS under -std=c11 */
#include "ee_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

/* ---- guest machine lifecycle (prototype helpers, not part of contract) */
int ee_machine_init(ee_state *ee) {
    memset(ee, 0, sizeof *ee);
    ee->ram = mmap(NULL, EE_RAM_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ee->ram == MAP_FAILED) { perror("mmap ram"); return -1; }
    ee->spr = mmap(NULL, EE_SPR_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ee->spr == MAP_FAILED) { perror("mmap spr"); return -1; }
    return 0;
}
void ee_machine_free(ee_state *ee) {
    if (ee->ram && ee->ram != MAP_FAILED) munmap(ee->ram, EE_RAM_SIZE);
    if (ee->spr && ee->spr != MAP_FAILED) munmap(ee->spr, EE_SPR_SIZE);
    ee->ram = ee->spr = NULL;
}

/* ---- MMIO trap-out (stubs: WS-K/WS-G/WS-V override) ------------------ */
uint32_t ee_mmio_load32(ee_state *ee, uint32_t addr){
    (void)ee; fprintf(stderr,"[mmio] load32  %08x (stub -> 0)\n", addr); return 0;
}
uint64_t ee_mmio_load64(ee_state *ee, uint32_t addr){
    (void)ee; fprintf(stderr,"[mmio] load64  %08x (stub -> 0)\n", addr); return 0;
}
void ee_mmio_store32(ee_state *ee, uint32_t addr, uint32_t val){
    (void)ee; fprintf(stderr,"[mmio] store32 %08x <= %08x (stub)\n", addr, val);
}
void ee_mmio_store64(ee_state *ee, uint32_t addr, uint64_t val){
    (void)ee; fprintf(stderr,"[mmio] store64 %08x <= %016llx (stub)\n",
                      addr,(unsigned long long)val);
}
void ee_mmio_store128(ee_state *ee, uint32_t addr, const ee_reg *val){
    (void)ee; fprintf(stderr,"[mmio] store128 %08x <= %016llx%016llx (stub)\n",
                      addr,(unsigned long long)val->ud[1],(unsigned long long)val->ud[0]);
}

/* ---- syscall / break / unimplemented --------------------------------- */
void ee_syscall(ee_state *ee, uint32_t code){
    fprintf(stderr,"[syscall] code=%u v1=%d a0=%08x (stub)\n",
            code,(int)EE_GETS32(ee,R_V1),EE_GET32(ee,R_A0));
}
void ee_break(ee_state *ee, uint32_t code){
    (void)ee; fprintf(stderr,"[break] code=%u\n",code);
}
void ee_trap_unimpl(ee_state *ee, uint32_t pc, uint32_t raw, const char *mnem){
    (void)ee; fprintf(stderr,"[FATAL] unimplemented op at %08x: raw=%08x (%s)\n",
                      pc,raw,mnem?mnem:"?"); abort();
}

/* ---- COP2 / VU0 (stubs: WS-V overrides) ------------------------------ */
static ee_vu0 g_vu0_stub;
ee_vu0 *ee_vu0_state(ee_state *ee){ (void)ee; return &g_vu0_stub; }
void ee_cop2_qmtc2(ee_state *ee,int vf,int lane,uint32_t bits){
    (void)ee; memcpy(&g_vu0_stub.vf[vf][lane], &bits, 4);
}
uint32_t ee_cop2_qmfc2(ee_state *ee,int vf,int lane){
    (void)ee; uint32_t b; memcpy(&b,&g_vu0_stub.vf[vf][lane],4); return b;
}

/* ---- COP0 / barriers (stubs: WS-K overrides) ------------------------- */
uint32_t ee_cop0_mfc0(ee_state *ee, int reg){
    switch(reg){
        case 9:  return ee->cop0_count;
        case 12: return ee->cop0_status;   /* spin bit (0x10000) starts clear */
        case 13: return ee->cop0_cause;
        case 14: return ee->cop0_epc;
        default: return 0;
    }
}
void ee_cop0_mtc0(ee_state *ee, int reg, uint32_t val){
    switch(reg){
        case 9:  ee->cop0_count=val; break;
        case 12: ee->cop0_status=val; break;
        case 13: ee->cop0_cause=val; break;
        case 14: ee->cop0_epc=val; break;
        default: break;
    }
}
void ee_ei(ee_state *ee){ ee->cop0_status |= 0x1u; }   /* IE */
void ee_di(ee_state *ee){ ee->cop0_status &= ~0x1u; }
void ee_eret(ee_state *ee){ (void)ee; }
void ee_sync(ee_state *ee, uint32_t stype){ (void)ee; (void)stype; }

/* ---- indirect call (prototype: linear table in function_table.c) ----- */
void ee_call_indirect(ee_state *ee, uint32_t guest_addr){
    ee_func_t f = ee_lookup(guest_addr);
    if (!f){ fprintf(stderr,"[FATAL] indirect call to unknown %08x\n",guest_addr); abort(); }
    f(ee);
}
