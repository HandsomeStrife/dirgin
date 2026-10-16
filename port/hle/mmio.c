/* =====================================================================
 * mmio.c  --  EE MMIO trap dispatch (ee_runtime.h §5) + the HLE DMAC
 * engine.  Recompiled stores whose canonical address is in 0x10000000..
 * 0x1FFFFFFF arrive here; we decode the register bank and forward:
 *   GS priv (0x12xx)      -> gs_write_display() / gs_flip()
 *   DMAC VIF1 ch (0x9xxx) -> assemble ring DMAtag chain -> VIF1 sink (vu/)
 *   DMAC GIF  ch (0xAxxx) -> gs_submit_packet / gs_submit_dma_chain (gs/)
 *   DMAC common/INTC      -> shadow + interrupt model
 *   SIF SBUS (0xF2xx)     -> sif/ (IOP-ready handshake)
 *
 * The DMA model is SYNCHRONOUS: a CHCR write that sets STR runs the transfer
 * immediately and clears STR in the shadow, so the engine's busy-poll loops
 * (FUN_0030b868 spins while CHCR&0x100) terminate.  Evidence for the ring/
 * channel scheme: FUN_0030b768 / FUN_0030b670 / FUN_0030b868 / FUN_00442100.
 * ===================================================================== */
#define _GNU_SOURCE
#include "hle_priv.h"
#include "../gs/gs.h"
#include <stdio.h>
#include <string.h>

/* sif hooks (weakly used; sif/ provides them, fallbacks here if unlinked) */
uint64_t sif_sbus_load (struct sif_dev *s, uint32_t addr) __attribute__((weak));
void     sif_sbus_store(struct sif_dev *s, uint32_t addr, uint64_t v) __attribute__((weak));

/* ---- DMAC channel shadow access ------------------------------------- */
static uint32_t *chan_reg(hle_ctx *h, uint32_t base, uint32_t off) {
    uint32_t idx = (base + off - 0x10008000u) / 4u;
    if (idx >= sizeof(h->dmac_chan)/sizeof(h->dmac_chan[0])) return NULL;
    return &h->dmac_chan[idx];
}
static uint32_t chan_get(hle_ctx *h, uint32_t base, uint32_t off) {
    uint32_t *p = chan_reg(h, base, off); return p ? *p : 0;
}
static void chan_set(hle_ctx *h, uint32_t base, uint32_t off, uint32_t v) {
    uint32_t *p = chan_reg(h, base, off); if (p) *p = v;
}

/* ---- generic source-chain DMAtag walker (for the VIF1 path) ---------
 * Walks tags starting at `addr` in guest RAM, invoking cb() with the
 * referenced qwords of each tag.  Implements id cnt/next/ref/refe/end.
 * Returns number of tags processed.  gs_submit_dma_chain() handles the GIF
 * path itself, so this exists only for VIF1 -> vu/. */
static int walk_dmatag_chain(hle_ctx *h, uint32_t addr,
                             void (*cb)(void*, const uint8_t*, size_t), void *user) {
    ee_state *ee = h->ee;
    int tags = 0;
    for (int guard = 0; guard < 100000; guard++) {
        uint32_t a = ee_canon(addr);
        const uint8_t *tag = ee_host_ptr(ee, a);
        uint16_t qwc = (uint16_t)(tag[0] | (tag[1] << 8));
        uint32_t id  = (tag[3] >> 4) & 0x7;          /* bits 28..30        */
        uint32_t paddr; memcpy(&paddr, tag + 4, 4);
        paddr = ee_canon(paddr);
        tags++;
        switch (id) {
        case 1: /* cnt : data follows the tag, next tag after the data */
            if (qwc && cb) cb(user, ee_host_ptr(ee, a + 16), (size_t)qwc * 16);
            addr = a + 16 + (uint32_t)qwc * 16;
            break;
        case 2: /* next: data follows the tag, next tag = ADDR field */
            if (qwc && cb) cb(user, ee_host_ptr(ee, a + 16), (size_t)qwc * 16);
            addr = paddr;
            break;
        case 0: /* refe: data at ADDR, then END */
        case 3: /* ref : data at ADDR, next tag after this 1-qword tag */
        case 4: /* refs */
            if (qwc && cb) cb(user, ee_host_ptr(ee, paddr), (size_t)qwc * 16);
            if (id == 0) return tags;                /* refe ends          */
            addr = a + 16;
            break;
        case 7: /* end : data follows, then stop */
            if (qwc && cb) cb(user, ee_host_ptr(ee, a + 16), (size_t)qwc * 16);
            return tags;
        default:                                     /* call/ret unused    */
            return tags;
        }
    }
    return tags;
}

/* ---- run a channel whose CHCR just had STR set ---------------------- */
static void dmac_run_channel(hle_ctx *h, uint32_t base, uint32_t chcr) {
    uint32_t madr = chan_get(h, base, DMAC_REG_MADR);
    uint32_t tadr = chan_get(h, base, DMAC_REG_TADR);
    uint32_t qwc  = chan_get(h, base, DMAC_REG_QWC);
    int chain = (chcr & CHCR_MOD_MASK) == CHCR_MOD_CHAIN;

    if (base == DMAC_CH_GIF) {
        if (chain) {
            if (h->gs) gs_submit_dma_chain(h->gs, h->ee->ram, EE_RAM_SIZE, ee_canon(tadr));
        } else {
            /* normal mode (FUN_00442100: CHCR=0x101, MADR+QWC) */
            if (h->gs && qwc) gs_submit_packet(h->gs, ee_host_ptr(h->ee, ee_canon(madr)), qwc);
        }
    } else if (base == DMAC_CH_VIF1) {
        if (chain)
            walk_dmatag_chain(h, ee_canon(tadr), h->vif1_cb, h->vif1_user);
        else if (h->vif1_cb && qwc)
            h->vif1_cb(h->vif1_user, ee_host_ptr(h->ee, ee_canon(madr)), (size_t)qwc * 16);
    }
    /* synchronous: transfer is done -> clear STR (busy) so pollers exit */
    chan_set(h, base, DMAC_REG_CHCR, chcr & ~CHCR_STR);
    chan_set(h, base, DMAC_REG_QWC, 0);
    h->intc_stat |= (1u << INTC_VIF1);   /* channel irq latch (model)      */
}

/* ---- GS privileged register write ----------------------------------- */
static void gs_priv_store(hle_ctx *h, uint32_t addr, uint64_t v) {
    switch (addr) {
    case GS_PMODE:    h->gs_pmode    = v; break;
    case GS_SMODE2:   h->gs_smode2   = v; break;
    case GS_DISPFB1:  h->gs_dispfb1  = v; break;
    case GS_DISPLAY1: h->gs_display1 = v; break;
    case GS_DISPFB2:  h->gs_dispfb2  = v; break;
    case GS_DISPLAY2: h->gs_display2 = v; break;
    case GS_BGCOLOR:  h->gs_bgcolor  = v; break;
    case GS_EXTDATA:  h->gs_extdata  = v; break;
    case GS_CSR:      h->gs_csr      = v; break;
    case GS_IMR:      h->gs_imr      = (uint32_t)v; break;
    default: break;
    }
    /* FUN_0040d228 writes PMODE, DISPFB1/2, DISPLAY2, then DISPLAY1 last:
     * present the display state once the sequence completes. */
    if (addr == GS_DISPLAY1 && h->gs) {
        gs_display d; memset(&d, 0, sizeof d);
        d.pmode = h->gs_pmode;   d.smode2  = h->gs_smode2;
        d.dispfb1 = h->gs_dispfb1; d.display1 = h->gs_display1;
        d.dispfb2 = h->gs_dispfb2; d.display2 = h->gs_display2;
        d.bgcolor = h->gs_bgcolor; d.extdata = h->gs_extdata;
        gs_write_display(h->gs, &d);
    }
}

/* ---- common store/load ---------------------------------------------- */
void hle_mmio_store(hle_ctx *h, uint32_t addr, int width, uint64_t v, const ee_reg *q) {
    addr = ee_canon(addr);
    if (width == 16 && q) v = q->ud[0];   /* 128-bit: use low 64           */

    if (mmio_is_gs_priv(addr)) { gs_priv_store(h, addr, v); return; }

    if (mmio_is_dmac_chan(addr)) {
        uint32_t base = addr & DMAC_CHAN_MASK;
        uint32_t off  = addr - base;
        chan_set(h, base, off, (uint32_t)v);
        if (off == DMAC_REG_CHCR && ((uint32_t)v & CHCR_STR))
            dmac_run_channel(h, base, (uint32_t)v);
        return;
    }
    if (mmio_is_dmac_common(addr)) {
        switch (addr) {
        case DMAC_CTRL:  h->dmac_ctrl = (uint32_t)v; break;
        case DMAC_STAT:  h->dmac_stat &= ~(uint32_t)v; break;  /* w1c */
        case DMAC_RBOR:  h->dmac_rbor = (uint32_t)v; break;
        case DMAC_RBSR:  h->dmac_rbsr = (uint32_t)v; break;
        case DMAC_SQWC:  h->dmac_sqwc = (uint32_t)v; break;
        default: break;
        }
        return;
    }
    if (mmio_is_intc(addr)) {
        if (addr == INTC_STAT) h->intc_stat &= ~(uint32_t)v;   /* w1c */
        else if (addr == INTC_MASK) h->intc_mask ^= (uint32_t)v;/* w-toggle */
        return;
    }
    if (mmio_is_sif(addr)) {
        if (sif_sbus_store) sif_sbus_store(h->sif, addr, v);
        return;
    }
    /* VIF0/VIF1/GIF register banks + anything else: track silently for M1. */
}

uint64_t hle_mmio_load(hle_ctx *h, uint32_t addr, int width) {
    (void)width;
    addr = ee_canon(addr);
    if (mmio_is_gs_priv(addr)) {
        switch (addr) {
        case GS_CSR: return h->gs_csr | 0x8;   /* FINISH/field bits benign */
        case GS_IMR: return h->gs_imr;
        default: return 0;
        }
    }
    if (mmio_is_dmac_chan(addr)) {
        uint32_t base = addr & DMAC_CHAN_MASK;
        return chan_get(h, base, addr - base);
    }
    if (mmio_is_dmac_common(addr)) {
        switch (addr) {
        case DMAC_CTRL: return h->dmac_ctrl;
        case DMAC_STAT: return h->dmac_stat;
        case DMAC_RBOR: return h->dmac_rbor;
        case DMAC_RBSR: return h->dmac_rbsr;
        default: return 0;
        }
    }
    if (mmio_is_intc(addr)) {
        if (addr == INTC_STAT) {
            /* Headless cooperative model: some guest code busy-polls INTC_STAT
             * for the VBLANK bit WITHOUT yielding (e.g. the libgraph vsync wait
             * FUN_00450df8: `INTC_STAT=4; while(!(INTC_STAT&4));`).  Such a spin
             * never lets the scheduler inject a synthetic vblank, so we auto-arm
             * the VBLANK_START bit on read (equivalent to a vblank arriving each
             * poll).  The sema-based frame pacing path is unaffected. */
            h->intc_stat |= (1u << INTC_VBLANK_START);
            return h->intc_stat;
        }
        return h->intc_mask;
    }
    if (mmio_is_sif(addr)) {
        if (sif_sbus_load) return sif_sbus_load(h->sif, addr);
        /* default: report IOP ready so the reboot/sync path proceeds */
        if (addr == SIF_SMFLAG) return 0x40000;
        return 0;
    }
    return 0;
}

/* ================================================================== *
 * ee_runtime.h §5 trap targets -> dispatch into the HLE.             *
 * ================================================================== */
void ee_mmio_store32(ee_state *ee, uint32_t addr, uint32_t val) {
    hle_mmio_store((hle_ctx*)ee->host, addr, 32, val, NULL);
}
void ee_mmio_store64(ee_state *ee, uint32_t addr, uint64_t val) {
    hle_mmio_store((hle_ctx*)ee->host, addr, 64, val, NULL);
}
void ee_mmio_store128(ee_state *ee, uint32_t addr, const ee_reg *val) {
    hle_mmio_store((hle_ctx*)ee->host, addr, 128, 0, val);
}
uint32_t ee_mmio_load32(ee_state *ee, uint32_t addr) {
    return (uint32_t)hle_mmio_load((hle_ctx*)ee->host, addr, 32);
}
uint64_t ee_mmio_load64(ee_state *ee, uint32_t addr) {
    return hle_mmio_load((hle_ctx*)ee->host, addr, 64);
}
