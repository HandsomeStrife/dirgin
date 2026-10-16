/* test_triangle.c -- headless proof that the GIF/GS interpreter works.
 *
 * Hand-assembles real GIF packets (qword by qword, exactly as the game does
 * inline, e.g. FUN_004115b8) and feeds them through gs_submit_packet:
 *   1. A PACKED A+D env block        -> FRAME / TEST / ALPHA / TEX0
 *   2. A PACKED drawing packet        -> one textured, gouraud, blended TRIANGLE
 *   3. A PACKED A+D transfer block + IMAGE payload -> an 8x8 PSMCT32 upload
 *   4. A frame flip
 * then asserts exactly one draw was emitted.  No GPU required.
 */
#include "gs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* qword emitter into a growing byte buffer */
typedef struct { uint8_t *p; size_t n, cap; } qbuf;
static void qw(qbuf *b, uint64_t lo, uint64_t hi) {
    if (b->n + 16 > b->cap) { b->cap = b->cap ? b->cap * 2 : 256;
                              b->p = realloc(b->p, b->cap); }
    for (int i = 0; i < 8; i++) b->p[b->n + i]     = (lo >> (8*i)) & 0xff;
    for (int i = 0; i < 8; i++) b->p[b->n + 8 + i] = (hi >> (8*i)) & 0xff;
    b->n += 16;
}

/* GIFtag builders */
static uint64_t giftag_lo(unsigned nloop, unsigned eop, unsigned pre,
                          unsigned prim, unsigned flg, unsigned nreg) {
    return (uint64_t)(nloop & 0x7fff)
         | ((uint64_t)(eop & 1) << 15)
         | ((uint64_t)(pre & 1) << 46)
         | ((uint64_t)(prim & 0x7ff) << 47)
         | ((uint64_t)(flg & 3) << 58)
         | ((uint64_t)(nreg & 0xf) << 60);
}

#define OFX 0x8000  /* default XYOFFSET (2048.0 in 12.4) matches gs_create */
static int32_t fixc(int screen) { return OFX + screen * 16; }

int main(void) {
    gs_state *gs = gs_create(gs_backend_log(), 640, 448);
    if (!gs) { fprintf(stderr, "gs_create failed\n"); return 1; }

    qbuf b = {0};

    /* ---- 1. env A+D block: FRAME, TEST, ALPHA, TEX0 (NREG=1 A+D) ---- */
    qw(&b, giftag_lo(4, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_PD_AD);
    /* FRAME_1: fbw=10(640px), psm=PSMCT32 */
    qw(&b, ((uint64_t)10 << 16), GS_FRAME_1);
    /* TEST_1: ate=1, zte=1, ztst=2(GEQUAL) */
    qw(&b, 1u | (1u << 16) | (2u << 17), GS_TEST_1);
    /* ALPHA_1: (Cs-Cd)*As>>7 + Cd  => A=0,B=1,C=0,D=1 */
    qw(&b, (1u << 2) | (1u << 6), GS_ALPHA_1);
    /* TEX0_1: tbw=4, tw=5(32), th=5(32), tcc=1 (RGBA) */
    qw(&b, ((uint64_t)4 << 14) | ((uint64_t)5 << 26) | ((uint64_t)5 << 30)
           | ((uint64_t)1 << 34), GS_TEX0_1);

    /* ---- 2. drawing packet: TRIANGLE, PRE=1, REGS=[RGBAQ,UV,XYZ2] ---- */
    unsigned prim = GS_PRIM_TRI | (1u<<3) /*iip*/ | (1u<<4) /*tme*/
                  | (1u<<6) /*abe*/ | (1u<<8) /*fst=UV*/;
    uint64_t regs = (uint64_t)GIF_PD_RGBAQ
                  | ((uint64_t)GIF_PD_UV   << 4)
                  | ((uint64_t)GIF_PD_XYZ2 << 8);
    qw(&b, giftag_lo(3, 1, 1, prim, GIF_FLG_PACKED, 3), regs);

    struct { int sx, sy; uint8_t r,g,b,a; uint16_t u,v; } V[3] = {
        { 100, 100, 0xff,0x00,0x00,0xff,   0,   0 },
        { 300, 100, 0x00,0xff,0x00,0xff, 512,   0 },
        { 200, 300, 0x00,0x00,0xff,0xff, 256, 512 },
    };
    for (int i = 0; i < 3; i++) {
        /* RGBAQ packed: R lane0, G lane1, B lane2, A lane3 */
        qw(&b, (uint64_t)V[i].r | ((uint64_t)V[i].g << 32),
               (uint64_t)V[i].b | ((uint64_t)V[i].a << 32));
        /* UV packed: U lane0[0:13], V lane1[0:13] */
        qw(&b, (uint64_t)V[i].u | ((uint64_t)V[i].v << 32), 0);
        /* XYZ2 packed: X lane0[0:15], Y lane1[0:15], Z lane2[0:31] */
        qw(&b, (uint64_t)(uint16_t)fixc(V[i].sx)
               | ((uint64_t)(uint16_t)fixc(V[i].sy) << 32),
               (uint64_t)0x003fffff);
    }

    /* ---- 3. texture upload: A+D transfer regs + IMAGE payload ---- */
    qw(&b, giftag_lo(4, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_PD_AD);
    /* BITBLTBUF: dbp=0x1200, dbw=1(64), dpsm=PSMCT32 */
    qw(&b, ((uint64_t)0x1200 << 32) | ((uint64_t)1 << 48), GS_BITBLTBUF);
    qw(&b, 0, GS_TRXPOS);                              /* dst 0,0 */
    qw(&b, (uint64_t)8 | ((uint64_t)8 << 32), GS_TRXREG); /* 8x8 */
    qw(&b, 0, GS_TRXDIR);                              /* host->local */
    /* IMAGE: 8x8 PSMCT32 = 256 bytes = 16 qwords of pixel data */
    qw(&b, giftag_lo(16, 1, 0, 0, GIF_FLG_IMAGE, 1), 0);
    for (int i = 0; i < 16; i++) qw(&b, 0xA1B2C3D4E5F60718ULL + i, 0);

    /* ---- run it ---- */
    printf("=== feeding %zu qwords through the GIF/GS interpreter ===\n",
           b.n / 16);
    size_t used = gs_submit_packet(gs, b.p, b.n / 16);
    printf("=== parsed %zu / %zu qwords ===\n", used, b.n / 16);

    gs_flip(gs);

    unsigned draws = gs_draw_count(gs);
    printf("=== draws emitted: %u (expected 1) ===\n", draws);

    const gs_frame *fr = gs_cur_frame(gs);
    printf("=== final FRAME: fbw=%u psm=0x%x ===\n", fr->fbw, fr->psm);

    gs_destroy(gs);
    free(b.p);

    if (draws != 1) { fprintf(stderr, "FAIL: expected exactly 1 draw\n"); return 1; }
    if (used != b.n / 16) { fprintf(stderr, "FAIL: did not consume all qwords\n"); return 1; }
    printf("PASS\n");
    return 0;
}
