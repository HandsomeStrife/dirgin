/* =====================================================================
 * frame_loop.c  --  Reauthored PER-FRAME MAIN LOOP (see frame_loop.h).
 *
 * A faithful reauthor of FUN_00248110: the order and control structure of
 * the original `while(1)` are preserved exactly; the PS2 GS DMA transport
 * bracket is the one intentional substitution (native DocRenderer hooks,
 * documented in frame_loop.h). Every below-seam call is an explicit,
 * addressed hook; no silent no-ops stand in for real behaviour.
 *
 * The decomp this traces to (analysis/decomp/00248110_FUN_00248110.c) is
 * quoted inline against each clause so the mapping stays auditable.
 * ===================================================================== */
#include "frame_loop.h"
#include "object_list.h"   /* doc_scene_object_list_update (FUN_00408758) */
#include <string.h>

/* --------------------------------------------------------------------- *
 * Default hook tables
 * --------------------------------------------------------------------- */

/* The two ported subsystems: the defaults invoke the REAL reauthored code.
 * This is what makes the frame loop the integration point -- it names
 * WHERE each ported subsystem runs, and the default wiring runs it. */
static void def_run_state_dispatch(DocEngine *e) {
    /* FUN_00247508: the top-level state machine, already ported. */
    engine_state_dispatch(&e->state, e->dispatch_hooks);
}
static void def_run_object_list(DocEngine *e) {
    /* FUN_00408758: the scene object-list walk, already ported. It reads
     * its head/enable from object_list.c module state; the frame loop just
     * drives it at the right point. */
    (void)e;
    doc_scene_object_list_update();
}

static void def_v0(void *c)            { (void)c; }
static void def_v1(void *c, int a)     { (void)c; (void)a; }
static void def_sub(void *c, int t, int a) { (void)c; (void)t; (void)a; }
static int  def_q0(void *c)            { (void)c; return 0; }

static void def_r_begin(void *c)                  { (void)c; }
static void def_r_submit(void *c, DocRenderQueue *q){ (void)c; (void)q; }
static void def_r_end(void *c)                    { (void)c; }
static void def_r_present(void *c, int m)         { (void)c; (void)m; }
static int  def_r_pending(void *c)                { (void)c; return 0; }

DocRenderer doc_renderer_default(void) {
    DocRenderer r;
    memset(&r, 0, sizeof(r));
    r.ctx = NULL;
    r.begin_frame   = def_r_begin;
    r.submit_scene  = def_r_submit;
    r.end_frame     = def_r_end;
    r.present       = def_r_present;
    r.pending_query = def_r_pending;
    return r;
}

DocFrameHooks doc_frame_hooks_default(void) {
    DocFrameHooks h;
    memset(&h, 0, sizeof(h));
    h.ctx = NULL;
    h.run_state_dispatch = def_run_state_dispatch;
    h.run_object_list    = def_run_object_list;
    h.input_poll    = def_v0;
    h.input_latch   = def_v0;
    h.frame_lock    = def_v1;
    h.frame_unlock  = def_v1;
    h.frame_advance = def_v0;
    h.subsystem     = def_sub;
    h.boot_cond_query   = def_q0;
    h.render_sync_query = def_q0;
    h.per_frame_hook    = def_q0;
    h.exit_query        = def_q0;
    return h;
}

void doc_engine_init(DocEngine *e,
                     const EngineHooks  *dispatch_hooks,
                     const DocRenderer  *renderer,
                     const DocFrameHooks*hooks,
                     DocRenderQueue     *render_queue) {
    memset(e, 0, sizeof(*e));
    engine_state_init(&e->state);
    e->dispatch_hooks = dispatch_hooks;
    e->renderer       = renderer;
    e->hooks          = hooks;
    e->render_queue   = render_queue;
}

/* Short-hands for the three wired tables (all assumed non-NULL). */
#define H  (e->hooks)
#define R  (e->renderer)
#define SUB(tag, arg) H->subsystem(H->ctx, (tag), (arg))

/* --------------------------------------------------------------------- *
 * Preamble  (decomp lines 8-12, before the while):
 *   FUN_0040ce28(0x4fc7d0,1);                         present(mode 1)
 *   lVar1 = FUN_00241e28();
 *   if ((lVar1 != 0) && (FUN_002413c0(7) != 0)) FUN_00241378(9,1);
 * FUN_002413c0 / FUN_00241378 ARE the ported mode-flag primitives.
 * --------------------------------------------------------------------- */
void doc_engine_frame_preamble(DocEngine *e) {
    R->present(R->ctx, 1);                              /* FUN_0040ce28(0x4fc7d0,1) */
    if (H->boot_cond_query(H->ctx) != 0 &&              /* FUN_00241e28 */
        engine_test_mode_flag(&e->state, ENGINE_FLAG_FADE_HOLD) != 0) {  /* FUN_002413c0(7) */
        engine_set_mode_flag(&e->state, ENGINE_FLAG_DIAG, 1);            /* FUN_00241378(9,1) */
    }
}

/* --------------------------------------------------------------------- *
 * One frame  (decomp lines 14-76, the body of the while(1)).
 * --------------------------------------------------------------------- */
DocFrameResult doc_engine_frame(DocEngine *e) {
    int keepalive;

    /* --- frame open: mutex lock + renderer begin (GS DMA open) --- */
    H->frame_lock(H->ctx, DOC_MUTEX_FRAME);     /* FUN_0042d228(0x4bcda8) */
    R->begin_frame(R->ctx);                     /* FUN_0030b768(0)  [purist: open] */

    /* --- input poll --- */
    H->input_poll(H->ctx);                      /* FUN_002548d8 */

    /* --- early per-frame subsystem updates --- */
    SUB(DOC_FSUB_UPDATE_00257670, 0);
    SUB(DOC_FSUB_UPDATE_003EBF18, 0);
    SUB(DOC_FSUB_UPDATE_002EFC78, 0);
    SUB(DOC_FSUB_UPDATE_002EFE00, 0);
    SUB(DOC_FSUB_RENDER_SETUP_00441E50, 0xfe00);    /* FUN_00441e50(0xfe00) */
    SUB(DOC_FSUB_UPDATE_00264F68, 0);
    SUB(DOC_FSUB_UPDATE_00267970, 0);
    SUB(DOC_FSUB_UPDATE_0026A0C0, 0);
    if (e->render_overlay_enabled != 0) {           /* if (DAT_0046dd6c != 0) */
        SUB(DOC_FSUB_RENDER_OVERLAY_00312D10, 0);   /*     FUN_00312d10(); */
    }

    /* --- STATE DISPATCH (ported) --- */
    H->run_state_dispatch(e);                       /* FUN_00247508 */

    /* --- scene build + draw-submission cluster --- */
    SUB(DOC_FSUB_UPDATE_00404068, 0);
    SUB(DOC_FSUB_UPDATE_004084F8, 0);
    SUB(DOC_FSUB_SCENE_DRAW_005A8E28, 0);           /* FUN_005a8e28 */
    /* OBJECT LIST walk (ported): flags the render-queue contexts... */
    H->run_object_list(e);                          /* FUN_00408758 */
    /* ...then submit that populated queue to the native renderer. This is
     * the purist stand-in for the inline GIF/GS emission the original does
     * while walking (see frame_loop.h). */
    R->submit_scene(R->ctx, e->render_queue);
    SUB(DOC_FSUB_UPDATE_00265240, 0);
    SUB(DOC_FSUB_RENDER_CLEAR_0040CDC0, (int)0x80808080); /* FUN_0040cdc0(0x4fc7d0,0x80808080) */
    SUB(DOC_FSUB_UPDATE_00245A08, 0);
    SUB(DOC_FSUB_UPDATE_002EFE98, 0);
    SUB(DOC_FSUB_UPDATE_00247428, 0);
    SUB(DOC_FSUB_UPDATE_00408CB8, 0);
    SUB(DOC_FSUB_UPDATE_003EDAB8, 0);
    SUB(DOC_FSUB_UPDATE_003DCE38, 0);
    SUB(DOC_FSUB_UPDATE_00271C00, 0);
    SUB(DOC_FSUB_UPDATE_00323258, e->misc_update_arg);  /* FUN_00323258(DAT_0046e290) */
    SUB(DOC_FSUB_UPDATE_003052F8, 0);
    SUB(DOC_FSUB_SCENE_FLUSH_002E29F0, 0);          /* FUN_002e29f0 */
    SUB(DOC_FSUB_UPDATE_002EFF60, 0);
    SUB(DOC_FSUB_SCENE_DRAW_005A8EC0, 0);           /* FUN_005a8ec0 */
    SUB(DOC_FSUB_RENDER_MISC_0040CDE8, 0);          /* FUN_0040cde8(0x4fc7d0) */
    SUB(DOC_FSUB_UPDATE_002F00C0, 0);

    /* first render-sync block:  if (FUN_00242928()) FUN_003dcf58(0); */
    if (H->render_sync_query(H->ctx) != 0) {
        SUB(DOC_FSUB_RENDER_SYNC_003DCF58, 0);
    }
    SUB(DOC_FSUB_UPDATE_00242208, 0);
    SUB(DOC_FSUB_UPDATE_002E2A20, 0);
    SUB(DOC_FSUB_UPDATE_002F0250, 0);
    /* second render-sync block:  if (FUN_00242928()) { FUN_003dcf58(1); FUN_003dd3f0(); } */
    if (H->render_sync_query(H->ctx) != 0) {
        SUB(DOC_FSUB_RENDER_SYNC_003DCF58, 1);
        SUB(DOC_FSUB_RENDER_SYNC_003DD3F0, 0);
    }
    SUB(DOC_FSUB_UPDATE_00265350, 0);

    /* optional per-frame hook:  lVar1 = FUN_002413f0(); if (lVar1) (*lVar1)(); */
    if (H->per_frame_hook(H->ctx) != 0) {
        SUB(DOC_FSUB_PER_FRAME_HOOK, 0);
    }
    SUB(DOC_FSUB_UPDATE_002F0428, 0);

    /* --- frame close: input latch, renderer kick, mutex unlock --- */
    H->input_latch(H->ctx);                     /* FUN_002548f8 */
    R->end_frame(R->ctx);                       /* FUN_0030b868(0)  [purist: kick] */
    H->frame_unlock(H->ctx, DOC_MUTEX_FRAME);   /* FUN_0042d390(0x4bcda8) */

    /* --- tail updates --- */
    SUB(DOC_FSUB_UPDATE_00253B78, 0);
    SUB(DOC_FSUB_UPDATE_002F0508, 0);

    /* --- EXIT TEST (decomp lines 72-74) ---
     *   lVar1 = FUN_002413c0(0x1a);
     *   if ((lVar1 == 0) && (FUN_002413c0(9) != 0 || FUN_00241868() != 0)) break;
     * i.e. break when NOT loop-keepalive AND (diag flag OR exit query).
     * FUN_002413c0 is the ported mode-flag test. The break happens BEFORE
     * present + frame-advance, so a stopping frame does neither. */
    keepalive = engine_test_mode_flag(&e->state, ENGINE_FLAG_LOOP_KEEPALIVE); /* FUN_002413c0(0x1a) */
    if (keepalive == 0 &&
        (engine_test_mode_flag(&e->state, ENGINE_FLAG_DIAG) != 0 ||  /* FUN_002413c0(9) */
         H->exit_query(H->ctx) != 0)) {                              /* FUN_00241868 */
        return DOC_FRAME_STOP;
    }

    /* --- present + advance (decomp lines 75-76) --- */
    R->present(R->ctx, 0);                      /* FUN_0040ce28(0x4fc7d0,0) vblank+flip */
    e->frame_counter++;                         /* FUN_00241a20(0) advances the frame */
    H->frame_advance(H->ctx);
    return DOC_FRAME_CONTINUE;
}

/* --------------------------------------------------------------------- *
 * Shutdown drain  (decomp lines 90-100):
 *   iVar2 = 0x1f; FUN_0030b478(1);
 *   do { iVar2--; lock; open; FUN_002e29f0(); kick; unlock; present(0); }
 *   while (-1 < iVar2);
 * iVar2 is decremented FIRST each pass, so the body runs for iVar2 =
 * 30,29,...,0,-1 -> 32 iterations (the guard -1<iVar2 fails once iVar2
 * reaches -1). Flushes the final frames out of the pipeline.
 * --------------------------------------------------------------------- */
void doc_engine_shutdown_drain(DocEngine *e) {
    int i = 0x1f;
    SUB(DOC_FSUB_RENDER_TEARDOWN_0030B478, 1);  /* FUN_0030b478(1) */
    do {
        i--;
        H->frame_lock(H->ctx, DOC_MUTEX_FRAME);     /* FUN_0042d228(0x4bcda8) */
        R->begin_frame(R->ctx);                     /* FUN_0030b768(0) */
        SUB(DOC_FSUB_SCENE_FLUSH_002E29F0, 0);      /* FUN_002e29f0 */
        R->end_frame(R->ctx);                       /* FUN_0030b868(0) */
        H->frame_unlock(H->ctx, DOC_MUTEX_FRAME);   /* FUN_0042d390(0x4bcda8) */
        R->present(R->ctx, 0);                      /* FUN_0040ce28(0x4fc7d0,0) */
    } while (-1 < i);
}

/* --------------------------------------------------------------------- *
 * The whole FUN_00248110.
 * --------------------------------------------------------------------- */
int doc_engine_run(DocEngine *e) {
    int frames = 0;

    doc_engine_frame_preamble(e);

    /* while(1) { ... if (exit) break; present; advance; } */
    while (doc_engine_frame(e) == DOC_FRAME_CONTINUE) {
        frames++;
    }

    /* --- post-loop shutdown (decomp lines 78-89) --- */
    e->shutdown_marker = 0x70;                       /* DAT_004fcaa0 = 0x70 */
    if (e->audio_teardown_enabled != 0) {            /* if (DAT_0047d720 != '\0') */
        H->frame_lock(H->ctx, DOC_MUTEX_SHUTDOWN);   /*   FUN_0042d228(0x50b2e8) */
        SUB(DOC_FSUB_SHUTDOWN_0041AF80, 0x14);       /*   FUN_0041af80(0x14,0) */
        H->frame_unlock(H->ctx, DOC_MUTEX_SHUTDOWN); /*   FUN_0042d390(0x50b2e8) */
    }
    e->shutdown_clear = 0;                           /* DAT_004fcf74 = 0 */
    SUB(DOC_FSUB_SHUTDOWN_00403578, 0);              /* FUN_00403578() */
    if (R->pending_query(R->ctx) != 0) {             /* if (FUN_0030b610() != 0) */
        R->end_frame(R->ctx);                        /*   FUN_0030b868(0) */
    }

    doc_engine_shutdown_drain(e);
    return frames;
}
