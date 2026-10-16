/* =====================================================================
 * sif_glue.c  --  bridge the recompiler's EE-level SIF hooks to the sif/
 * device API.  The recompiler aliases the guest SIF library functions to
 * these hooks (ee_runtime.h §: sif_call_rpc/bind/reboot/sync/writeback, all
 * void(ee_state*)); the real work lives in sif/sif.c behind a cleaner
 * device-level API (renamed sif_dev_* to avoid the name clash).  This file
 * reads the EABI argument registers the guest set up and forwards.
 *
 * EABI arg mapping (verified at the recompiled call sites, e.g. FUN_00446af0
 * for the CD read): args 1..8 in $a0..$a3,$t0..$t3; arg 9 at $sp+0.
 * sceSifCallRpc(client, fno, mode, sendbuf, ssize, recvbuf, rsize, endfunc,
 *               endpara).
 * ===================================================================== */
#include "sif.h"
#include "ee_runtime.h"
#include <stdio.h>
#include <stdlib.h>

/* set by boot/main.c at init */
sif_dev *g_sif = NULL;

static int   g_trace;        /* DIRGE_TRACE                     */
static int   g_no_rpc_cb;    /* DIRGE_NO_RPC_CB: skip end_func   */
static int   g_glue_inited;
static void glue_init(void){
    if (g_glue_inited) return;
    g_trace     = getenv("DIRGE_TRACE")    != NULL;
    g_no_rpc_cb = getenv("DIRGE_NO_RPC_CB") != NULL;
    g_glue_inited = 1;
}

/* ---------------------------------------------------------------------
 * FUN_004535d8 was routed to this hook, but it is NOT a SIF library call:
 * the decomp shows a pure guest-memory RPC *packet allocator*
 *   (param: {counter@0, base@+4, count@+8}; packets of 0x40 bytes; mark a
 *    free one with (counter<<16|5), assign id, return its address, else 0).
 * Routing it to a no-op would break sceSifCallRpc (which calls it to get a
 * packet).  We reproduce its exact behaviour here on guest memory.
 * (Documented as a WS-R mis-routing; faithful re-implementation.)
 * --------------------------------------------------------------------- */
void sif_bind_rpc(ee_state *ee){
    uint32_t desc = EE_GET32(ee, R_A0);
    int32_t  count = (int32_t)ee_lw(ee, desc + 0x08);
    uint32_t base  = ee_lw(ee, desc + 0x04);
    int32_t  i = 0;
    uint32_t pkt = base;
    for (; i < count; i++, pkt += 0x40){
        uint32_t flags = ee_lw(ee, pkt + 0x10);
        if ((flags & 1u) == 0){
            uint32_t counter = ee_lw(ee, desc + 0x00);
            ee_sw(ee, pkt + 0x10, (counter << 16) | 5u);
            uint32_t id = counter + 1;
            ee_sw(ee, desc + 0x00, counter + 1);
            if (id == 1){ id = 1; ee_sw(ee, desc + 0x00, counter + 2); }
            ee_sw(ee, pkt + 0x14, pkt);
            ee_sw(ee, pkt + 0x18, id);
            EE_SET32(ee, R_V0, pkt);
            return;
        }
    }
    EE_SET32(ee, R_V0, 0);
}

/* ---------------------------------------------------------------------
 * sceSifCallRpc (FUN_00453d20).  We replace the whole send-to-IOP + async
 * wait with a synchronous local service against KEL.DAT / host fs, then
 * (optionally) invoke the user end_func callback to mirror completion, and
 * return 0 (success).
 * --------------------------------------------------------------------- */
void sif_call_rpc(ee_state *ee){
    glue_init();
    uint32_t client  = EE_GET32(ee, R_A0);
    int32_t  fno     = (int32_t)EE_GET32(ee, R_A1);
    uint32_t mode    = EE_GET32(ee, R_A2);
    uint32_t sendbuf = EE_GET32(ee, R_A3);
    uint32_t ssize   = EE_GET32(ee, R_T0);
    uint32_t recvbuf = EE_GET32(ee, R_T1);
    uint32_t rsize   = EE_GET32(ee, R_T2);
    uint32_t endfunc = EE_GET32(ee, R_T3);
    uint32_t sp      = EE_GET32(ee, R_SP);
    uint32_t endpara = ee_lw(ee, sp + 0);

    if (g_trace)
        fprintf(stderr, "[sif] CallRpc client=%08x fno=%d mode=%u send=%08x "
                        "ssize=%u recv=%08x endfunc=%08x\n",
                client, fno, mode, sendbuf, ssize, recvbuf, endfunc);

    int rc = -1;
    if (g_sif)
        rc = sif_dev_call_rpc(g_sif, client, fno, sendbuf, ssize,
                              recvbuf, rsize, -1);

    /* mirror the async RPC completion: run the registered end_func(endpara)
     * so the engine's file worker sees its "done" flag set. */
    if (rc >= 0 && endfunc && !g_no_rpc_cb){
        ee_func_t cb = ee_lookup(endfunc);
        if (cb){
            uint32_t save_a0 = EE_GET32(ee, R_A0);
            ee->r[R_A0].sd[0] = (int32_t)endpara;
            cb(ee);
            ee->r[R_A0].sd[0] = (int32_t)save_a0;
        } else if (g_trace){
            fprintf(stderr, "[sif] end_func %08x not in table\n", endfunc);
        }
    }
    EE_SET32(ee, R_V0, (uint32_t)rc);
}

/* ---------------------------------------------------------------------
 * sceSifBindRpc (FUN_00453b50): bind an IOP RPC server to a client.  On HW
 * the EE sends a bind request and the IOP replies by DMAing the server's data
 * into the client struct (client->server at +0x00, the "bound" marker at
 * +0x24) and signalling a sema.  We have no IOP, but our sceSifCallRpc is
 * HLE'd by client address, so we just complete the bind synchronously:
 * publish a nonzero server handle + bound marker so the engine's bind-wait
 * (e.g. FUN_004464c8's `while(... ) ; if (client[+0x24]) break;`) proceeds.
 * param: $a0=client, $a1=sid, $a2=mode.  Returns 0 (success).
 * --------------------------------------------------------------------- */
void sif_bind_server(ee_state *ee){
    glue_init();
    uint32_t client = EE_GET32(ee, R_A0);
    uint32_t sid    = EE_GET32(ee, R_A1);
    if (g_trace) fprintf(stderr, "[sif] BindRpc client=%08x sid=%08x\n", client, sid);
    if (client){
        ee_sw(ee, client + 0x00, client);   /* client->server = nonzero handle */
        ee_sw(ee, client + 0x24, client);    /* bound marker (IOP-reply field)  */
    }
    EE_SET32(ee, R_V0, 0);
}

/* RebootIop (FUN_004517f0): mark the HLE IOP ready. */
void sif_reboot_iop(ee_state *ee){
    if (g_sif) sif_dev_reboot_iop(g_sif);
    EE_SET32(ee, R_V0, 1);          /* nonzero = reboot DMA queued ok */
}

/* SyncIop (FUN_00451958): IOP ready -> true. */
void sif_sync_iop(ee_state *ee){
    int ready = g_sif ? sif_iop_ready(g_sif) : 1;
    EE_SET32(ee, R_V0, ready ? 1 : 0);
}

/* WriteBackDCache (FUN_00453360): flat memory model -> no-op. */
void sif_writeback_dcache(ee_state *ee){ (void)ee; }
