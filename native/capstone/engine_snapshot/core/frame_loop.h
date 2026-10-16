/* =====================================================================
 * frame_loop.h  --  Reauthored PER-FRAME MAIN LOOP: the native engine's
 * frame backbone (purist port, P4).
 *
 * This is the integration point the rest of the engine hangs on. The
 * original per-frame loop is FUN_00248110 (engine-map.md §1, "boot ->
 * per-frame loop", agent: spine), a `while(1)` that each frame:
 *
 *   lock frame mutex -> OPEN GS DMA display list -> poll input ->
 *   ~40 subsystem updates -> STATE DISPATCH (FUN_00247508) ->
 *   drive the object list (FUN_00408758) -> KICK GS DMA ->
 *   unlock mutex -> test exit -> VBLANK wait + framebuffer FLIP ->
 *   advance frame
 *
 * It bottoms out at a hard shutdown + a 33-iteration pipeline-drain loop.
 *
 * ---------------------------------------------------------------------
 * PURIST SUBSTITUTION (the one intentional divergence from the oracle):
 *
 * The original brackets the frame body with the PS2 GS DMA transport:
 *   FUN_0030b768(0)         open:  begin a VIF1 DMA display list
 *   [inline GIF/GS packet emission woven through the draw subsystems]
 *   FUN_0030b868(0)         kick:  close the list, start VIF1, wait DMA
 *   FUN_0040ce28(0x4fc7d0,.) vblank-semaphore wait + PCRTC framebuffer flip
 *
 * A PURIST PC PORT emits NO GS DMA packets. We keep the loop's ORDER and
 * control structure byte-faithful, but replace that transport bracket
 * with a clean NATIVE-RENDERER interface (DocRenderer below):
 *   FUN_0030b768  ->  renderer_begin_frame
 *   inline emit   ->  renderer_submit_scene(render_queue)   (see note)
 *   FUN_0030b868  ->  renderer_end_frame
 *   FUN_0040ce28  ->  present(mode)
 * The original guest addresses are kept in comments so the mapping stays
 * auditable (DECOMP-METHOD.md §4: "When you see inline packet/DMA/GIF
 * emission, that is below the seam -- stub it"). renderer_submit_scene is
 * placed at the object-list draw point: in the original, walking the
 * object list (FUN_00408758) is what flags the render-queue contexts
 * (0x4ee2e0 family, our DocRenderQueue) whose bits the inline packet
 * emission then consumes. Submitting that populated queue to the native
 * renderer is the faithful native expression of "walk scene -> emit".
 *
 * ---------------------------------------------------------------------
 * CLEAN vs HOOKED:
 *   CLEAN  (real ported code, invoked here):
 *     - the top-level state machine   -> engine_state_dispatch (core/)
 *     - the scene object-list walk     -> doc_scene_object_list_update (scene/)
 *     - frame counter / mode-flag / scene-state primitives (core/)
 *   HOOKED (below-seam, each tagged with its original FUN_ address):
 *     - native renderer transport      -> DocRenderer (purist substitution)
 *     - input poll/latch, frame mutex, vblank-present
 *     - the ~40 per-subsystem per-frame updates -> DocFrameHooks.subsystem,
 *       each an explicit, addressed EngineHooks-style tag (NOT a silent
 *       no-op): every one of them is a subsystem pending its own port.
 *
 * Reauthored from (analysis/decomp/, read-only oracle):
 *   FUN_00248110  per-frame main loop            -> doc_engine_run / _frame
 * Uses the already-ported:
 *   FUN_00247508  state dispatch   -> engine_state_dispatch (engine_state.h)
 *   FUN_00408758  object-list walk -> doc_scene_object_list_update (object_list.h)
 * ===================================================================== */
#ifndef DOC_FRAME_LOOP_H
#define DOC_FRAME_LOOP_H

#include <stdint.h>
#include "engine_state.h"
#include "scene_graph.h"   /* DocRenderQueue (the 0x4ee2e0 render context) */

#ifdef __cplusplus
extern "C" {
#endif

struct DocEngine;   /* fwd: the top-level native engine state (below) */

/* --------------------------------------------------------------------- *
 * Per-frame below-seam subsystem updates, enumerated for audit.
 *
 * Every plain per-subsystem update the loop calls (no control-flow-
 * relevant return) is routed through DocFrameHooks::subsystem(ctx, tag,
 * arg). Tags carry the ORIGINAL guest address in the name so the mapping
 * to FUN_00248110 stays traceable and a byte-accurate transcription can be
 * diffed against it (tests/test_frame_loop.c). These are HOOKS, not
 * reimplementations: each names a subsystem that is pending its own port.
 * The ORDER of these values matches their call order inside the loop.
 * --------------------------------------------------------------------- */
typedef enum DocFrameSubsys {
    /* ---- early per-frame updates (after input poll, before dispatch) --- */
    DOC_FSUB_UPDATE_00257670,         /* FUN_00257670 */
    DOC_FSUB_UPDATE_003EBF18,         /* FUN_003ebf18 */
    DOC_FSUB_UPDATE_002EFC78,         /* FUN_002efc78 */
    DOC_FSUB_UPDATE_002EFE00,         /* FUN_002efe00 */
    DOC_FSUB_RENDER_SETUP_00441E50,   /* FUN_00441e50(0xfe00)  libgraph frame setup */
    DOC_FSUB_UPDATE_00264F68,         /* FUN_00264f68 */
    DOC_FSUB_UPDATE_00267970,         /* FUN_00267970 */
    DOC_FSUB_UPDATE_0026A0C0,         /* FUN_0026a0c0 */
    DOC_FSUB_RENDER_OVERLAY_00312D10, /* FUN_00312d10 (gated by DAT_0046dd6c) */
    /* ---- after state dispatch: scene build + draw submission ----------- */
    DOC_FSUB_UPDATE_00404068,         /* FUN_00404068 */
    DOC_FSUB_UPDATE_004084F8,         /* FUN_004084f8 */
    DOC_FSUB_SCENE_DRAW_005A8E28,     /* FUN_005a8e28  (scene draw) */
    DOC_FSUB_UPDATE_00265240,         /* FUN_00265240 */
    DOC_FSUB_RENDER_CLEAR_0040CDC0,   /* FUN_0040cdc0(0x4fc7d0,0x80808080) clear */
    DOC_FSUB_UPDATE_00245A08,         /* FUN_00245a08 */
    DOC_FSUB_UPDATE_002EFE98,         /* FUN_002efe98 */
    DOC_FSUB_UPDATE_00247428,         /* FUN_00247428 */
    DOC_FSUB_UPDATE_00408CB8,         /* FUN_00408cb8 */
    DOC_FSUB_UPDATE_003EDAB8,         /* FUN_003edab8 */
    DOC_FSUB_UPDATE_003DCE38,         /* FUN_003dce38 */
    DOC_FSUB_UPDATE_00271C00,         /* FUN_00271c00 */
    DOC_FSUB_UPDATE_00323258,         /* FUN_00323258(DAT_0046e290) */
    DOC_FSUB_UPDATE_003052F8,         /* FUN_003052f8 */
    DOC_FSUB_SCENE_FLUSH_002E29F0,    /* FUN_002e29f0  (scene flush; reused in drain) */
    DOC_FSUB_UPDATE_002EFF60,         /* FUN_002eff60 */
    DOC_FSUB_SCENE_DRAW_005A8EC0,     /* FUN_005a8ec0  (scene draw) */
    DOC_FSUB_RENDER_MISC_0040CDE8,    /* FUN_0040cde8(0x4fc7d0) */
    DOC_FSUB_UPDATE_002F00C0,         /* FUN_002f00c0 */
    DOC_FSUB_RENDER_SYNC_003DCF58,    /* FUN_003dcf58(arg) (gated by FUN_00242928) */
    DOC_FSUB_UPDATE_00242208,         /* FUN_00242208 */
    DOC_FSUB_UPDATE_002E2A20,         /* FUN_002e2a20 */
    DOC_FSUB_UPDATE_002F0250,         /* FUN_002f0250 */
    DOC_FSUB_RENDER_SYNC_003DD3F0,    /* FUN_003dd3f0 (second gated block) */
    DOC_FSUB_UPDATE_00265350,         /* FUN_00265350 */
    DOC_FSUB_PER_FRAME_HOOK,          /* FUN_002413f0 returns a code ptr, invoked */
    DOC_FSUB_UPDATE_002F0428,         /* FUN_002f0428 */
    /* ---- after kick + mutex unlock (still inside the frame) ------------ */
    DOC_FSUB_UPDATE_00253B78,         /* FUN_00253b78 */
    DOC_FSUB_UPDATE_002F0508,         /* FUN_002f0508 */
    /* ---- shutdown / teardown (after the loop breaks) ------------------- */
    DOC_FSUB_SHUTDOWN_0041AF80,       /* FUN_0041af80(0x14,0) (gated by DAT_0047d720) */
    DOC_FSUB_SHUTDOWN_00403578,       /* FUN_00403578 */
    DOC_FSUB_RENDER_TEARDOWN_0030B478 /* FUN_0030b478(1) */
} DocFrameSubsys;

/* Mutex ids (which frame mutex is taken; the arg to FUN_0042d228/0042d390).
 * The main frame mutex and a separate shutdown mutex are distinct guest
 * objects; kept distinct so lock/unlock pairing stays auditable. */
#define DOC_MUTEX_FRAME     0x004bcda8   /* main per-frame mutex */
#define DOC_MUTEX_SHUTDOWN  0x0050b2e8   /* shutdown-only mutex  */

/* --------------------------------------------------------------------- *
 * The native renderer interface -- the PURIST substitution for the PS2
 * GS DMA transport bracket. See the file header. Each pointer records the
 * original guest transport address it replaces.
 * --------------------------------------------------------------------- */
typedef struct DocRenderer {
    void *ctx;
    void (*begin_frame)   (void *ctx);                       /* <- FUN_0030b768(0) open */
    void (*submit_scene)  (void *ctx, DocRenderQueue *q);    /* <- inline GIF/GS emit  */
    void (*end_frame)     (void *ctx);                       /* <- FUN_0030b868(0) kick */
    void (*present)       (void *ctx, int mode);             /* <- FUN_0040ce28(.,mode) */
    int  (*pending_query) (void *ctx);                       /* <- FUN_0030b610 (!=0 => kick) */
} DocRenderer;

/* --------------------------------------------------------------------- *
 * The frame-loop seam. Every below-seam call that is NOT the native
 * renderer and NOT one of the ported subsystems goes through here. The
 * dispatch assumes every pointer is non-NULL (use doc_frame_hooks_default).
 * --------------------------------------------------------------------- */
typedef struct DocFrameHooks {
    void *ctx;

    /* ---- the two PORTED subsystems the loop drives (delegated so the
     *      integration point is named; the default impls call the real
     *      ported code in engine_state.c / object_list.c). ---- */
    void (*run_state_dispatch)(struct DocEngine *e);  /* <- FUN_00247508 */
    void (*run_object_list)   (struct DocEngine *e);  /* <- FUN_00408758 */

    /* ---- input / frame mutex / frame pacing ---- */
    void (*input_poll)   (void *ctx);           /* <- FUN_002548d8 */
    void (*input_latch)  (void *ctx);           /* <- FUN_002548f8 */
    void (*frame_lock)   (void *ctx, int mutex);/* <- FUN_0042d228(mutex) */
    void (*frame_unlock) (void *ctx, int mutex);/* <- FUN_0042d390(mutex) */
    void (*frame_advance)(void *ctx);           /* <- FUN_00241a20(0) */

    /* ---- below-seam per-subsystem update sink ---- */
    void (*subsystem)    (void *ctx, int tag, int arg); /* tag = DocFrameSubsys */

    /* ---- control-flow queries (guest FUN_ in comment) ---- */
    int (*boot_cond_query) (void *ctx);  /* FUN_00241e28  (preamble: >0 && flag7 => set flag9) */
    int (*render_sync_query)(void *ctx); /* FUN_00242928  (!=0 => the two 003dcf58 blocks) */
    int (*per_frame_hook)  (void *ctx);  /* FUN_002413f0  (!=0 => invoke hook subsystem) */
    int (*exit_query)      (void *ctx);  /* FUN_00241868  (break condition) */
} DocFrameHooks;

/* --------------------------------------------------------------------- *
 * DocEngine -- the top-level native engine state. Ties the ported
 * subsystems together: the spine state machine, the render queue, the
 * frame counter, and the frame-loop's own module state (the DAT_ gates
 * the loop reads). This is the context threaded through every frame.
 * --------------------------------------------------------------------- */
typedef struct DocEngine {
    EngineState         state;          /* the spine state machine (engine_state.h) */
    const EngineHooks  *dispatch_hooks; /* passed to engine_state_dispatch each frame */

    const DocRenderer  *renderer;       /* native renderer (purist GS DMA replacement) */
    const DocFrameHooks*hooks;          /* below-seam frame hooks */
    DocRenderQueue     *render_queue;    /* the scene render context submitted each frame */

    int32_t frame_counter;              /* <- FUN_00241a20 top-level frame pacing counter */

    /* ---- frame-loop module state (replaces the DAT_ globals it reads) --- */
    int render_overlay_enabled;         /* <- DAT_0046dd6c (gates FUN_00312d10) */
    int audio_teardown_enabled;         /* <- DAT_0047d720 (gates FUN_0041af80 on shutdown) */
    int misc_update_arg;                /* <- DAT_0046e290 (arg to FUN_00323258) */

    /* ---- shutdown markers written by the teardown path (DAT_ stores) ---- */
    int32_t shutdown_marker;            /* <- DAT_004fcaa0 = 0x70 on exit */
    int32_t shutdown_clear;             /* <- DAT_004fcf74 = 0   on exit */
} DocEngine;

/* Result of one frame: whether the loop's exit condition fired. */
typedef enum DocFrameResult {
    DOC_FRAME_CONTINUE = 0,  /* exit condition false: presented + advanced */
    DOC_FRAME_STOP     = 1   /* exit condition true: broke BEFORE present/advance */
} DocFrameResult;

/* --------------------------------------------------------------------- *
 * API
 * --------------------------------------------------------------------- */

/* Zero the engine and wire the three hook tables. renderer/hooks may be
 * NULL to accept the all-safe defaults (see the _default makers). */
void doc_engine_init(DocEngine *e,
                     const EngineHooks  *dispatch_hooks,
                     const DocRenderer  *renderer,
                     const DocFrameHooks*hooks,
                     DocRenderQueue     *render_queue);

/* All-safe baselines: no-op side effects, queries return 0. The frame
 * hooks' run_state_dispatch / run_object_list defaults invoke the REAL
 * ported subsystems (engine_state_dispatch / doc_scene_object_list_update). */
DocRenderer   doc_renderer_default(void);
DocFrameHooks doc_frame_hooks_default(void);

/* One iteration of the FUN_00248110 `while(1)` body, in faithful order:
 *   frame-mutex lock -> renderer begin -> input poll -> early updates ->
 *   STATE DISPATCH -> scene build/draw + OBJECT LIST + renderer submit ->
 *   late updates -> input latch -> renderer end (kick) -> mutex unlock ->
 *   tail updates -> EXIT TEST.
 * If the exit condition fires, returns DOC_FRAME_STOP WITHOUT presenting
 * or advancing (faithful: the break precedes FUN_0040ce28/FUN_00241a20).
 * Otherwise presents (vblank+flip) and advances the frame counter, then
 * returns DOC_FRAME_CONTINUE. */
DocFrameResult doc_engine_frame(DocEngine *e);

/* The preamble before the loop (FUN_00248110 lines 8-12): present(mode 1),
 * then if boot_cond_query() && mode-flag 7 set, set mode-flag 9. Uses the
 * ported engine_state mode-flag primitives. */
void doc_engine_frame_preamble(DocEngine *e);

/* The post-loop pipeline drain (FUN_00248110 lines 90-100): a fixed
 * 33-iteration loop that locks, opens, flushes the scene (FUN_002e29f0),
 * kicks, unlocks and presents, to flush the final frames on shutdown. */
void doc_engine_shutdown_drain(DocEngine *e);

/* The whole FUN_00248110: preamble, then frame loop until an exit, then
 * the shutdown stores + conditional audio teardown + final kick + drain.
 * Returns the number of frames that ran before the exit fired. */
int doc_engine_run(DocEngine *e);

#ifdef __cplusplus
}
#endif

#endif /* DOC_FRAME_LOOP_H */
