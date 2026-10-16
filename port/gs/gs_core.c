/* gs_core.c -- GIF packet interpreter + GS register state machine.
 *
 * Pure logic, no GPU.  Parses GIFtags (PACKED / REGLIST / IMAGE), unpacks
 * register writes, maintains GS context state, assembles vertices and emits
 * decoded primitives + VRAM transfers to a gs_backend.
 *
 * Layouts follow the PS2 GS User's Manual; the subset implemented is the one
 * Dirge of Cerberus actually emits (see gs_regs.h evidence annotations).
 */
#include "gs.h"
#include <stdlib.h>
#include <string.h>

/* ---- little helpers ------------------------------------------------ */
static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
/* extract bits [lo..hi] inclusive from a 64-bit word */
static uint64_t bits(uint64_t v, int lo, int hi) {
    uint64_t mask = (hi - lo >= 63) ? ~0ULL : (((1ULL << (hi - lo + 1)) - 1));
    return (v >> lo) & mask;
}

/* ---- full GS context state ---------------------------------------- */
struct gs_state {
    const gs_backend *be;
    int width, height;

    /* current primitive */
    enum gs_prim_type prim;
    unsigned iip, tme, fge, abe, aa1, fst, ctxt, fix;

    /* latched vertex attributes */
    uint8_t r, g, b, a;
    float   q, s, t;
    uint16_t u, v;
    uint8_t  fog;

    /* XYOFFSET (12.4) for the active context */
    int32_t ofx, ofy;

    /* decoded context regs */
    gs_tex0  tex0;
    gs_alpha alpha;
    gs_test  test;
    gs_frame frame;
    gs_zbuf  zbuf;

    /* transfer latches (BITBLTBUF/TRXPOS/TRXREG), fired on TRXDIR */
    gs_tex_transfer xfer;

    /* vertex assembly fifo (max needed = 3 for tri; sprite uses 2) */
    gs_vertex vtx[3];
    int       vcount;

    unsigned draws;
};

/* ---- register decoders -------------------------------------------- */
static void apply_prim(gs_state *g, uint64_t d) {
    g->prim = (enum gs_prim_type)bits(d, 0, 2);
    g->iip  = bits(d, 3, 3);
    g->tme  = bits(d, 4, 4);
    g->fge  = bits(d, 5, 5);
    g->abe  = bits(d, 6, 6);
    g->aa1  = bits(d, 7, 7);
    g->fst  = bits(d, 8, 8);
    g->ctxt = bits(d, 9, 9);
    g->fix  = bits(d, 10, 10);
    g->vcount = 0; /* a PRIM write restarts primitive assembly */
}

static void apply_reg(gs_state *g, unsigned reg, uint64_t d);

/* push the latched attributes + a position as a vertex; kick if complete */
static void vertex_kick(gs_state *g, int32_t fx, int32_t fy, uint32_t z,
                        uint8_t fg, int adc) {
    if (g->vcount >= 3) g->vcount = 0; /* defensive */
    gs_vertex *v = &g->vtx[g->vcount];
    v->fx = fx; v->fy = fy;
    v->x  = (fx - g->ofx) / 16.0f;
    v->y  = (fy - g->ofy) / 16.0f;
    v->z  = z;  v->fog = fg; v->adc = adc;
    v->r = g->r; v->g = g->g; v->b = g->b; v->a = g->a;
    v->q = g->q; v->s = g->s; v->t = g->t;
    v->u = g->u; v->v = g->v; v->uv_mode = g->fst;
    g->vcount++;

    if (adc) return; /* XYZ*3 / ADC=1: accumulate only, no draw */

    int need;
    switch (g->prim) {
        case GS_PRIM_POINT:      need = 1; break;
        case GS_PRIM_LINE:       need = 2; break;
        case GS_PRIM_LINE_STRIP: need = 2; break;
        case GS_PRIM_TRI:        need = 3; break;
        case GS_PRIM_TRI_STRIP:  need = 3; break;
        case GS_PRIM_TRI_FAN:    need = 3; break;
        case GS_PRIM_SPRITE:     need = 2; break;
        default:                 need = 0; break;
    }
    if (need == 0 || g->vcount < need) return;

    gs_draw dc;
    dc.prim = g->prim;
    dc.iip = g->iip; dc.tme = g->tme; dc.fge = g->fge; dc.abe = g->abe;
    dc.aa1 = g->aa1; dc.fst = g->fst; dc.ctxt = g->ctxt; dc.fix = g->fix;
    dc.verts = g->vtx; dc.nverts = need; dc.st = g;
    if (g->be && g->be->draw) g->be->draw(g->be->self, &dc);
    g->draws++;

    /* reset / roll the fifo for strips & fans */
    switch (g->prim) {
        case GS_PRIM_TRI_STRIP:      /* keep last 2 verts */
            g->vtx[0] = g->vtx[1];
            g->vtx[1] = g->vtx[2];
            g->vcount = 2;
            break;
        case GS_PRIM_TRI_FAN:        /* keep first + last */
            g->vtx[1] = g->vtx[2];
            g->vcount = 2;
            break;
        case GS_PRIM_LINE_STRIP:
            g->vtx[0] = g->vtx[1];
            g->vcount = 1;
            break;
        default:
            g->vcount = 0;           /* independent prims */
            break;
    }
}

static void fire_transfer(gs_state *g) {
    /* called on TRXDIR write; image payload (if any) is attached later by the
     * IMAGE giftag handler, so for direct-register path we deliver what we
     * have.  host->local with no data yet is completed by the IMAGE packet. */
    if (g->xfer.dir == 1 /*local->host readback*/ ||
        g->xfer.data != NULL) {
        if (g->be && g->be->transfer) g->be->transfer(g->be->self, &g->xfer);
    }
}

static void apply_reg(gs_state *g, unsigned reg, uint64_t d) {
    switch (reg) {
    case GS_PRIM:  apply_prim(g, d); break;
    case GS_RGBAQ:
        g->r = bits(d,0,7); g->g = bits(d,8,15);
        g->b = bits(d,16,23); g->a = bits(d,24,31);
        { uint32_t qi = (uint32_t)bits(d,32,63); memcpy(&g->q, &qi, 4); }
        break;
    case GS_ST: {
        uint32_t si = (uint32_t)bits(d,0,31), ti = (uint32_t)bits(d,32,63);
        memcpy(&g->s, &si, 4); memcpy(&g->t, &ti, 4);
        break; }
    case GS_UV:
        g->u = bits(d,0,13); g->v = bits(d,16,29); break;
    case GS_FOG:
        g->fog = bits(d,56,63); break;
    case GS_XYZ2:
        vertex_kick(g, bits(d,0,15), bits(d,16,31), bits(d,32,63), g->fog, 0);
        break;
    case GS_XYZF2:
        vertex_kick(g, bits(d,0,15), bits(d,16,31), bits(d,32,55),
                    bits(d,56,63), 0);
        break;
    case GS_XYZ3:
        vertex_kick(g, bits(d,0,15), bits(d,16,31), bits(d,32,63), g->fog, 1);
        break;
    case GS_XYZF3:
        vertex_kick(g, bits(d,0,15), bits(d,16,31), bits(d,32,55),
                    bits(d,56,63), 1);
        break;
    case GS_TEX0_1: case GS_TEX0_2:
        g->tex0.tbp0 = bits(d,0,13);  g->tex0.tbw = bits(d,14,19);
        g->tex0.psm  = bits(d,20,25); g->tex0.tw  = bits(d,26,29);
        g->tex0.th   = bits(d,30,33); g->tex0.tcc = bits(d,34,34);
        g->tex0.tfx  = bits(d,35,36); g->tex0.cbp = bits(d,37,50);
        g->tex0.cpsm = bits(d,51,54); g->tex0.csm = bits(d,55,55);
        g->tex0.csa  = bits(d,56,60); g->tex0.cld = bits(d,61,63);
        break;
    case GS_ALPHA_1: case GS_ALPHA_2:
        g->alpha.A = bits(d,0,1); g->alpha.B = bits(d,2,3);
        g->alpha.C = bits(d,4,5); g->alpha.D = bits(d,6,7);
        g->alpha.FIX = bits(d,32,39);
        break;
    case GS_TEST_1: case GS_TEST_2:
        g->test.ate  = bits(d,0,0);  g->test.atst = bits(d,1,3);
        g->test.aref = bits(d,4,11); g->test.afail= bits(d,12,13);
        g->test.date = bits(d,14,14);g->test.datm = bits(d,15,15);
        g->test.zte  = bits(d,16,16);g->test.ztst = bits(d,17,18);
        break;
    case GS_FRAME_1: case GS_FRAME_2:
        g->frame.fbp = bits(d,0,8);   g->frame.fbw = bits(d,16,21);
        g->frame.psm = bits(d,24,29); g->frame.fbmsk = (uint32_t)bits(d,32,63);
        break;
    case GS_ZBUF_1: case GS_ZBUF_2:
        g->zbuf.zbp = bits(d,0,8); g->zbuf.psm = bits(d,24,27);
        g->zbuf.zmsk = bits(d,32,32);
        break;
    case GS_XYOFFSET_1: case GS_XYOFFSET_2:
        g->ofx = (int32_t)bits(d,0,15); g->ofy = (int32_t)bits(d,32,47);
        break;
    case GS_BITBLTBUF:
        g->xfer.sbp = bits(d,0,13);  g->xfer.sbw = bits(d,16,21);
        g->xfer.spsm= bits(d,24,29);
        g->xfer.dbp = bits(d,32,45); g->xfer.dbw = bits(d,48,53);
        g->xfer.dpsm= bits(d,56,61);
        break;
    case GS_TRXPOS:
        g->xfer.ssax = bits(d,0,10);  g->xfer.ssay = bits(d,16,26);
        g->xfer.dsax = bits(d,32,42); g->xfer.dsay = bits(d,48,58);
        break;
    case GS_TRXREG:
        g->xfer.rrw = bits(d,0,11); g->xfer.rrh = bits(d,32,43);
        break;
    case GS_TRXDIR:
        g->xfer.dir = bits(d,0,1);
        fire_transfer(g);
        break;
    /* env regs we track by presence but don't yet model in the stub */
    case GS_PRMODECONT: case GS_PRMODE:   case GS_SCISSOR_1: case GS_SCISSOR_2:
    case GS_TEXA:       case GS_TEX1_1:   case GS_TEX1_2:    case GS_CLAMP_1:
    case GS_CLAMP_2:    case GS_COLCLAMP: case GS_DTHE:      case GS_FOGCOL:
    case GS_PABE:       case GS_FBA_1:    case GS_FBA_2:     case GS_DIMX:
    case GS_FINISH:     case GS_SIGNAL:   case GS_LABEL:     case GS_TEXFLUSH:
    case GS_NOP:
        break;
    default:
        break;
    }
}

/* ---- PACKED-mode per-descriptor unpack ---------------------------- *
 * Each PACKED entry is one 128-bit qword (4 x 32-bit lanes).          */
static void packed_entry(gs_state *g, unsigned desc, const uint8_t *q) {
    uint64_t lo = rd64(q), hi = rd64(q + 8);
    switch (desc) {
    case GIF_PD_PRIM:  apply_prim(g, bits(lo,0,10)); break;
    case GIF_PD_RGBAQ:
        g->r = bits(lo,0,7);
        g->g = bits(lo,32,39);
        g->b = bits(hi,0,7);    /* lane2 */
        g->a = bits(hi,32,39);  /* lane3 */
        break;
    case GIF_PD_ST: {
        uint32_t si = (uint32_t)bits(lo,0,31), ti = (uint32_t)bits(lo,32,63);
        uint32_t qi = (uint32_t)bits(hi,0,31);   /* lane2 = Q */
        memcpy(&g->s,&si,4); memcpy(&g->t,&ti,4); memcpy(&g->q,&qi,4);
        break; }
    case GIF_PD_UV:
        g->u = bits(lo,0,13); g->v = bits(lo,32,45); break;
    case GIF_PD_XYZF2: {
        int adc = bits(hi,47,47);
        vertex_kick(g, bits(lo,0,15), bits(lo,32,47),
                    bits(hi,4,27), bits(hi,36,43), adc);
        break; }
    case GIF_PD_XYZ2: {
        int adc = bits(hi,47,47);
        vertex_kick(g, bits(lo,0,15), bits(lo,32,47), bits(hi,0,31), g->fog, adc);
        break; }
    case GIF_PD_XYZF3:
        vertex_kick(g, bits(lo,0,15), bits(lo,32,47),
                    bits(hi,4,27), bits(hi,36,43), 1);
        break;
    case GIF_PD_XYZ3:
        vertex_kick(g, bits(lo,0,15), bits(lo,32,47), bits(hi,0,31), g->fog, 1);
        break;
    case GIF_PD_FOG:
        g->fog = bits(hi,36,43); break;
    case GIF_PD_AD: {                 /* DATA = [0:63], ADDR = [64:71] */
        unsigned reg = (unsigned)bits(hi,0,7);
        apply_reg(g, reg, lo);
        break; }
    case GIF_PD_NOP:
    default:
        break;
    }
}

/* ---- GIFtag parse -------------------------------------------------- */
size_t gs_submit_giftag(gs_state *g, const uint8_t *buf, size_t avail) {
    if (avail < 1) return 0;
    uint64_t lo = rd64(buf), hi = rd64(buf + 8);

    unsigned nloop = (unsigned)bits(lo, 0, 14);
    unsigned eop   = (unsigned)bits(lo, 15, 15);
    unsigned pre   = (unsigned)bits(lo, 46, 46);
    uint64_t prim  = bits(lo, 47, 57);
    unsigned flg   = (unsigned)bits(lo, 58, 59);
    unsigned nreg  = (unsigned)bits(lo, 60, 63);
    if (nreg == 0) nreg = 16;
    (void)eop;

    if (pre) apply_prim(g, prim);

    size_t qw = 1; /* the tag itself */

    switch (flg) {
    case GIF_FLG_PACKED: {
        for (unsigned l = 0; l < nloop; l++) {
            for (unsigned r = 0; r < nreg; r++) {
                if (qw >= avail) return qw;
                unsigned desc = (unsigned)bits(hi, r * 4, r * 4 + 3);
                packed_entry(g, desc, buf + qw * 16);
                qw++;
            }
        }
        break; }
    case GIF_FLG_REGLIST: {
        /* NLOOP*NREG registers, 2 x 64-bit packed per qword, raw reg data */
        unsigned total = nloop * nreg, i = 0;
        while (i < total) {
            if (qw >= avail) return qw;
            const uint8_t *p = buf + qw * 16;
            uint64_t d0 = rd64(p), d1 = rd64(p + 8);
            unsigned r0 = (unsigned)bits(hi, (i % nreg) * 4, (i % nreg) * 4 + 3);
            apply_reg(g, r0, d0); i++;
            if (i < total) {
                unsigned r1 = (unsigned)bits(hi, (i % nreg) * 4,
                                             (i % nreg) * 4 + 3);
                apply_reg(g, r1, d1); i++;
            }
            qw++;
        }
        break; }
    case GIF_FLG_IMAGE: {
        /* NLOOP qwords of raw VRAM payload -> complete pending transfer */
        if (qw + nloop > avail) nloop = (unsigned)(avail - qw);
        g->xfer.data = buf + qw * 16;
        g->xfer.data_len = (size_t)nloop * 16;
        if (g->be && g->be->transfer) g->be->transfer(g->be->self, &g->xfer);
        g->xfer.data = NULL; g->xfer.data_len = 0;
        qw += nloop;
        break; }
    case GIF_FLG_DISABLE:
    default:
        break;
    }
    return qw;
}

size_t gs_submit_packet(gs_state *g, const uint8_t *buf, size_t nqwords) {
    size_t qw = 0;
    while (qw < nqwords) {
        size_t used = gs_submit_giftag(g, buf + qw * 16, nqwords - qw);
        if (used == 0) break;
        qw += used;
    }
    return qw;
}

/* ---- DMAtag source-chain walker (GIF-direct path) ------------------ */
int gs_submit_dma_chain(gs_state *g, const uint8_t *mem, size_t mem_size,
                        uint32_t addr) {
    int tags = 0;
    uint32_t a = addr & 0x0fffffff;
    for (int guard = 0; guard < 0x10000; guard++) {
        if ((size_t)a + 16 > mem_size) break;
        uint64_t tag = rd64(mem + a);
        unsigned qwc  = (unsigned)bits(tag, 0, 15);
        unsigned id   = (unsigned)bits(tag, 28, 30);
        uint32_t addr_field = (uint32_t)bits(tag, 32, 62) & 0x0fffffff;
        const uint8_t *data = mem + a + 16;  /* qwords follow the tag  */
        if ((size_t)(a + 16) + (size_t)qwc * 16 <= mem_size && qwc)
            gs_submit_packet(g, data, qwc);
        tags++;

        /* DMAtag id: 0=refe(end),1=cnt,2=next,3=ref,7=end */
        if (id == 0 || id == 7) break;            /* refe / end */
        if (id == 1)      a = a + 16 + qwc * 16;   /* cnt: inline continues */
        else if (id == 2) a = addr_field;          /* next */
        else if (id == 3) a = a + 16 + qwc * 16;   /* ref: tag advances past inline */
        else              a = a + 16 + qwc * 16;
    }
    return tags;
}

/* ---- direct MMIO / helper entry points ----------------------------- */
void gs_write_display(gs_state *g, const gs_display *disp) {
    if (g->be && g->be->display) g->be->display(g->be->self, disp);
}
void gs_do_transfer(gs_state *g, const gs_tex_transfer *t) {
    g->xfer = *t;
    if (g->be && g->be->transfer) g->be->transfer(g->be->self, &g->xfer);
}
void gs_flip(gs_state *g) {
    if (g->be && g->be->flip) g->be->flip(g->be->self);
}

/* ---- lifecycle + introspection ------------------------------------- */
gs_state *gs_create(const gs_backend *be, int w, int h) {
    gs_state *g = (gs_state *)calloc(1, sizeof *g);
    if (!g) return NULL;
    g->be = be; g->width = w; g->height = h;
    g->r = g->g = g->b = g->a = 0x80; g->q = 1.0f;
    g->prim = GS_PRIM_INVALID;
    g->ofx = 0x8000; g->ofy = 0x8000;  /* GS default window centre (2048.0) */
    if (be && be->init) {
        if (be->init(be->self, w, h) != 0) { free(g); return NULL; }
    }
    return g;
}
void gs_destroy(gs_state *g) {
    if (!g) return;
    if (g->be && g->be->shutdown) g->be->shutdown(g->be->self);
    free(g);
}
const gs_tex0  *gs_cur_tex0 (const gs_state *g){ return &g->tex0;  }
const gs_alpha *gs_cur_alpha(const gs_state *g){ return &g->alpha; }
const gs_test  *gs_cur_test (const gs_state *g){ return &g->test;  }
const gs_frame *gs_cur_frame(const gs_state *g){ return &g->frame; }
unsigned        gs_draw_count(const gs_state *g){ return g->draws; }
