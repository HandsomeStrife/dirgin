/* gs.h -- public interface to the GS (Graphics Synthesizer) backend for the
 * Dirge of Cerberus PC port (WS-G).
 *
 * This is the seam the rest of the port calls.  Above it: the recompiled EE
 * code (WS-R) + HLE kernel/DMAC (WS-K) build DMA chains in guest memory and
 * "kick" them; WS-V runs VU1 and produces GIF packets.  Below it: a pluggable
 * rasterizer backend (log / software / OpenGL / Vulkan).
 *
 * The caller hands us GIF packets (a flat byte buffer of 128-bit qwords) or a
 * guest-memory DMAtag chain.  We parse GIFtags + GS register writes, run a GS
 * register state machine, and drive the backend with decoded draw calls and
 * VRAM transfers.  No GPU is required to parse: the interpreter half is pure
 * logic and runs headlessly.
 */
#ifndef GS_H
#define GS_H

#include <stdint.h>
#include <stddef.h>
#include "gs_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== *
 * Decoded value types                                                *
 * ================================================================== */

/* A single assembled vertex, already unpacked from PACKED/REGLIST form.
 * Screen coords are the GS 12.4 fixed-point values converted to float
 * pixels (after subtracting XYOFFSET).  Raw fixed values kept too. */
typedef struct {
    float    x, y;          /* screen pixels (XYOFFSET applied)            */
    int32_t  fx, fy;        /* raw 12.4 fixed-point X/Y (pre-offset)       */
    uint32_t z;             /* 24- or 32-bit depth                         */
    uint8_t  fog;
    uint8_t  r, g, b, a;    /* vertex colour                               */
    float    q;             /* RGBAQ.Q / ST.Q                              */
    float    s, t;          /* ST texture coords (perspective)             */
    uint16_t u, v;          /* UV texture coords (10.4 fixed)              */
    int      uv_mode;        /* 1 = UV (FST), 0 = ST                        */
    int      adc;           /* 1 = accumulate, do not draw                 */
} gs_vertex;

/* Decoded TEX0 (sampling source + CLUT). */
typedef struct {
    uint32_t tbp0, tbw, psm, tw, th, tcc, tfx;      /* texture base/format */
    uint32_t cbp, cpsm, csm, csa, cld;              /* CLUT (paletted)     */
} gs_tex0;

/* Decoded ALPHA (per-pixel blend eqn:  (A-B)*C>>7 + D ). */
typedef struct { uint32_t A, B, C, D, FIX; } gs_alpha;

/* Decoded TEST (alpha test + depth test + destination-alpha test). */
typedef struct {
    uint32_t ate, atst, aref, afail;   /* alpha test                       */
    uint32_t date, datm;               /* destination alpha test           */
    uint32_t zte, ztst;                /* depth test                       */
} gs_test;

/* Decoded FRAME (colour buffer) and ZBUF (depth buffer). */
typedef struct { uint32_t fbp, fbw, psm, fbmsk; } gs_frame;
typedef struct { uint32_t zbp, psm, zmsk;        } gs_zbuf;

/* Snapshot of the GS context relevant to one draw. */
typedef struct gs_state gs_state;

/* One decoded draw call delivered to the backend. */
typedef struct {
    enum gs_prim_type prim;
    unsigned iip, tme, fge, abe, aa1, fst, ctxt, fix; /* PRIM flags         */
    const gs_vertex  *verts;
    int               nverts;       /* exactly the count for `prim`         */
    const gs_state   *st;           /* for tex0/alpha/test/frame snapshots  */
} gs_draw;

/* A VRAM<->VRAM / host<->VRAM transfer (BITBLTBUF + TRXPOS/REG + TRXDIR). */
typedef struct {
    uint32_t dbp, dbw, dpsm;        /* dest  buffer base/width/format       */
    uint32_t sbp, sbw, spsm;        /* src   buffer base/width/format       */
    uint32_t dsax, dsay, ssax, ssay;/* TRXPOS dest/src x,y                  */
    uint32_t rrw, rrh;              /* TRXREG transmission area w,h         */
    int      dir;                   /* 0 host->local, 1 local->host, 2 l->l */
    const uint8_t *data;            /* image payload (host->local)          */
    size_t   data_len;              /* bytes available in `data`            */
} gs_tex_transfer;

/* PCRTC / display registers written at flip (FUN_00441f78 / FUN_0040d228). */
typedef struct {
    uint64_t pmode, smode2, dispfb1, display1, dispfb2, display2,
             bgcolor, extdata;
} gs_display;

/* ================================================================== *
 * Backend interface -- the pluggable rasterizer target.              *
 * A backend implements these; `self` is its own instance pointer.    *
 * ================================================================== */
typedef struct gs_backend {
    const char *name;
    void *self;
    int  (*init)     (void *self, int width, int height);
    void (*draw)     (void *self, const gs_draw *d);
    void (*transfer) (void *self, const gs_tex_transfer *t);
    void (*display)  (void *self, const gs_display *disp);
    void (*flip)     (void *self);
    void (*shutdown) (void *self);
} gs_backend;

/* The built-in headless logging backend (backend_log.c). */
const gs_backend *gs_backend_log(void);

/* ================================================================== *
 * GS state machine (opaque; allocate with gs_create).                *
 * ================================================================== */
struct gs_state;

gs_state *gs_create(const gs_backend *backend, int width, int height);
void      gs_destroy(gs_state *gs);

/* Parse a flat buffer of GIF packets (`nqwords` * 16 bytes): one or more
 * GIFtags each followed by their payload.  This is the normal entry once a
 * DMAtag chain has been resolved to contiguous GIF data.  Returns the number
 * of qwords consumed. */
size_t gs_submit_packet(gs_state *gs, const uint8_t *buf, size_t nqwords);

/* Parse exactly one GIFtag (+ its payload) at `buf`.  Returns qwords consumed
 * (0 on malformed input). */
size_t gs_submit_giftag(gs_state *gs, const uint8_t *buf, size_t avail_qwords);

/* Walk a source-chain DMAtag list in guest memory and feed the referenced
 * GIF data through gs_submit_packet.  `mem_base`+`addr` locate the first tag.
 * This mirrors the GIF-direct transport (FUN_0030b768 arg!=0 / FUN_00442100).
 * `mem_size` bounds reads.  Returns number of tags processed. */
int gs_submit_dma_chain(gs_state *gs, const uint8_t *mem_base, size_t mem_size,
                        uint32_t addr);

/* Write a privileged/PCRTC register directly (MMIO path: PMODE/DISPFBn/
 * DISPLAYn/SMODE2/BGCOLOR/EXTDATA/CSR...). */
void gs_write_display(gs_state *gs, const gs_display *disp);

/* Perform a texture/VRAM transfer directly (used by the HLE texture-upload
 * path FUN_004415a0 when it bypasses the draw list). */
void gs_do_transfer(gs_state *gs, const gs_tex_transfer *t);

/* Present the current colour buffer (frame flip). */
void gs_flip(gs_state *gs);

/* Introspection for tests / debuggers. */
const gs_tex0  *gs_cur_tex0 (const gs_state *gs);
const gs_alpha *gs_cur_alpha(const gs_state *gs);
const gs_test  *gs_cur_test (const gs_state *gs);
const gs_frame *gs_cur_frame(const gs_state *gs);
unsigned        gs_draw_count(const gs_state *gs);

#ifdef __cplusplus
}
#endif
#endif /* GS_H */
