/* =====================================================================
 * test_frame_loop.c  --  Unit + oracle-parity test for the reauthored
 * PER-FRAME MAIN LOOP (core/frame_loop.{h,c}).
 *
 * Four things are checked:
 *
 *  1. CALL-SEQUENCE PARITY (the DECOMP-METHOD.md step, made concrete for a
 *     control-flow function). A byte/callseq-accurate transcription of
 *     FUN_00248110 -- ref_frame / ref_preamble / ref_drain / ref_run below
 *     -- replays the ORIGINAL's control structure (which subsystem calls
 *     happen, in what order, under which gate/flag/query conditions) and
 *     emits an ordered call log. The clean doc_engine_* functions emit
 *     their own log through the hook seam. For a matrix of synthetic
 *     environments (gates on/off, render-sync on/off, per-frame-hook
 *     on/off, exit firing / not) the two ordered logs are asserted
 *     IDENTICAL, frame for frame, across N frames and the full run.
 *
 *     The render transport is the one intentional divergence (purist: the
 *     GS DMA bracket becomes native-renderer hooks). Both the clean loop
 *     and the transcription use the SAME native tags at the SAME positions,
 *     so the CONTROL-FLOW parity still holds exactly -- the substitution is
 *     applied identically to both sides by design (see frame_loop.h).
 *
 *  2. EXIT SEMANTICS. The break is BEFORE present + frame-advance: a
 *     stopping frame must neither present nor advance the counter. Checked
 *     against the decomp's condition (!keepalive && (diag || exit_query)).
 *
 *  3. INTEGRATION. With the DEFAULT hooks, the loop drives the REAL ported
 *     subsystems (engine_state_dispatch + doc_scene_object_list_update);
 *     asserted to run cleanly and advance both frame counters.
 *
 *  4. (Mutation is driven externally per DECOMP-METHOD.md: break an op in
 *     frame_loop.c, rebuild, confirm a parity assert goes red, restore.)
 *
 * All frame-loop logic is pure integer/bit/order, so PS2 float quirks are
 * irrelevant and the hand transcription is a faithful oracle.
 * ===================================================================== */
#include "frame_loop.h"
#include "engine_state.h"
#include "object_list.h"
#include "scene_graph.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ===================================================================== *
 * Ordered call log.  Each entry is {event, a, b}.
 * ===================================================================== */
enum Ev {
    EV_FLOCK, EV_FUNLOCK, EV_RBEGIN, EV_RSUBMIT, EV_REND, EV_PRESENT,
    EV_INPUT_POLL, EV_INPUT_LATCH, EV_STATE_DISPATCH, EV_OBJLIST,
    EV_FRAME_ADVANCE, EV_SUB
};
#define LOGMAX 4096
typedef struct Log { int n; int e[LOGMAX][3]; } Log;

static void log_push(Log *L, int ev, int a, int b) {
    if (L->n < LOGMAX) { L->e[L->n][0] = ev; L->e[L->n][1] = a; L->e[L->n][2] = b; }
    L->n++;
}

/* Scripted environment: the control-flow query returns the loop reads.
 * (Gates render_overlay_enabled / audio_teardown_enabled / misc_update_arg
 * live on DocEngine, not here, so the clean loop and the transcription read
 * the SAME engine fields.) */
typedef struct World {
    int boot_cond;       /* FUN_00241e28   */
    int render_sync;     /* FUN_00242928   */
    int per_frame_hook;  /* FUN_002413f0   */
    int exit_query;      /* FUN_00241868   */
    int render_pending;  /* FUN_0030b610   */
} World;

static Log   *g_log;     /* current clean-run log (hooks push here) */
static World *g_world;   /* current scripted environment            */

static void L_(int ev, int a, int b) { log_push(g_log, ev, a, b); }

/* ---- clean-run hook implementations (push to g_log, read g_world) ---- */
static void hk_sub(void *c, int tag, int arg) { (void)c; L_(EV_SUB, tag, arg); }
static void hk_flock(void *c, int m)   { (void)c; L_(EV_FLOCK, m, 0); }
static void hk_funlock(void *c, int m) { (void)c; L_(EV_FUNLOCK, m, 0); }
static void hk_ipoll(void *c)   { (void)c; L_(EV_INPUT_POLL, 0, 0); }
static void hk_ilatch(void *c)  { (void)c; L_(EV_INPUT_LATCH, 0, 0); }
static void hk_fadv(void *c)    { (void)c; L_(EV_FRAME_ADVANCE, 0, 0); }
static void hk_dispatch(DocEngine *e) { (void)e; L_(EV_STATE_DISPATCH, 0, 0); }
static void hk_objlist(DocEngine *e)  { (void)e; L_(EV_OBJLIST, 0, 0); }

static int hk_boot(void *c)   { (void)c; return g_world->boot_cond; }
static int hk_rsync(void *c)  { (void)c; return g_world->render_sync; }
static int hk_perhk(void *c)  { (void)c; return g_world->per_frame_hook; }
static int hk_exit(void *c)   { (void)c; return g_world->exit_query; }

static void rk_begin(void *c)  { (void)c; L_(EV_RBEGIN, 0, 0); }
static void rk_submit(void *c, DocRenderQueue *q) { (void)c; (void)q; L_(EV_RSUBMIT, 0, 0); }
static void rk_end(void *c)    { (void)c; L_(EV_REND, 0, 0); }
static void rk_present(void *c, int m) { (void)c; L_(EV_PRESENT, m, 0); }
static int  rk_pending(void *c) { (void)c; return g_world->render_pending; }

static DocFrameHooks logging_hooks(void) {
    DocFrameHooks h = doc_frame_hooks_default();
    h.ctx = NULL;
    h.run_state_dispatch = hk_dispatch;   /* log-only (parity run is deterministic) */
    h.run_object_list    = hk_objlist;
    h.input_poll    = hk_ipoll;
    h.input_latch   = hk_ilatch;
    h.frame_lock    = hk_flock;
    h.frame_unlock  = hk_funlock;
    h.frame_advance = hk_fadv;
    h.subsystem     = hk_sub;
    h.boot_cond_query   = hk_boot;
    h.render_sync_query = hk_rsync;
    h.per_frame_hook    = hk_perhk;
    h.exit_query        = hk_exit;
    return h;
}
static DocRenderer logging_renderer(void) {
    DocRenderer r = doc_renderer_default();
    r.begin_frame   = rk_begin;
    r.submit_scene  = rk_submit;
    r.end_frame     = rk_end;
    r.present       = rk_present;
    r.pending_query = rk_pending;
    return r;
}

/* ===================================================================== *
 * ORACLE: byte/callseq-accurate transcription of FUN_00248110.
 * Reads the SAME engine gate fields + mode flags and the SAME scripted
 * queries, and pushes the SAME ordered log. Mutates its OWN DocEngine copy
 * exactly as the clean loop mutates the clean copy, so the two engines can
 * be diffed field-for-field afterwards.
 * ===================================================================== */
#define RL(ev, a, b) log_push(L, (ev), (a), (b))
#define RSUB(tag, arg) RL(EV_SUB, (tag), (arg))

static void ref_preamble(DocEngine *e, Log *L) {
    RL(EV_PRESENT, 1, 0);                                   /* FUN_0040ce28(.,1) */
    if (g_world->boot_cond != 0 &&
        engine_test_mode_flag(&e->state, ENGINE_FLAG_FADE_HOLD) != 0) {
        engine_set_mode_flag(&e->state, ENGINE_FLAG_DIAG, 1);
    }
}

static DocFrameResult ref_frame(DocEngine *e, Log *L) {
    int ka;
    RL(EV_FLOCK, DOC_MUTEX_FRAME, 0);
    RL(EV_RBEGIN, 0, 0);
    RL(EV_INPUT_POLL, 0, 0);
    RSUB(DOC_FSUB_UPDATE_00257670, 0);
    RSUB(DOC_FSUB_UPDATE_003EBF18, 0);
    RSUB(DOC_FSUB_UPDATE_002EFC78, 0);
    RSUB(DOC_FSUB_UPDATE_002EFE00, 0);
    RSUB(DOC_FSUB_RENDER_SETUP_00441E50, 0xfe00);
    RSUB(DOC_FSUB_UPDATE_00264F68, 0);
    RSUB(DOC_FSUB_UPDATE_00267970, 0);
    RSUB(DOC_FSUB_UPDATE_0026A0C0, 0);
    if (e->render_overlay_enabled != 0) RSUB(DOC_FSUB_RENDER_OVERLAY_00312D10, 0);

    RL(EV_STATE_DISPATCH, 0, 0);

    RSUB(DOC_FSUB_UPDATE_00404068, 0);
    RSUB(DOC_FSUB_UPDATE_004084F8, 0);
    RSUB(DOC_FSUB_SCENE_DRAW_005A8E28, 0);
    RL(EV_OBJLIST, 0, 0);
    RL(EV_RSUBMIT, 0, 0);
    RSUB(DOC_FSUB_UPDATE_00265240, 0);
    RSUB(DOC_FSUB_RENDER_CLEAR_0040CDC0, (int)0x80808080);
    RSUB(DOC_FSUB_UPDATE_00245A08, 0);
    RSUB(DOC_FSUB_UPDATE_002EFE98, 0);
    RSUB(DOC_FSUB_UPDATE_00247428, 0);
    RSUB(DOC_FSUB_UPDATE_00408CB8, 0);
    RSUB(DOC_FSUB_UPDATE_003EDAB8, 0);
    RSUB(DOC_FSUB_UPDATE_003DCE38, 0);
    RSUB(DOC_FSUB_UPDATE_00271C00, 0);
    RSUB(DOC_FSUB_UPDATE_00323258, e->misc_update_arg);
    RSUB(DOC_FSUB_UPDATE_003052F8, 0);
    RSUB(DOC_FSUB_SCENE_FLUSH_002E29F0, 0);
    RSUB(DOC_FSUB_UPDATE_002EFF60, 0);
    RSUB(DOC_FSUB_SCENE_DRAW_005A8EC0, 0);
    RSUB(DOC_FSUB_RENDER_MISC_0040CDE8, 0);
    RSUB(DOC_FSUB_UPDATE_002F00C0, 0);

    if (g_world->render_sync != 0) RSUB(DOC_FSUB_RENDER_SYNC_003DCF58, 0);
    RSUB(DOC_FSUB_UPDATE_00242208, 0);
    RSUB(DOC_FSUB_UPDATE_002E2A20, 0);
    RSUB(DOC_FSUB_UPDATE_002F0250, 0);
    if (g_world->render_sync != 0) {
        RSUB(DOC_FSUB_RENDER_SYNC_003DCF58, 1);
        RSUB(DOC_FSUB_RENDER_SYNC_003DD3F0, 0);
    }
    RSUB(DOC_FSUB_UPDATE_00265350, 0);

    if (g_world->per_frame_hook != 0) RSUB(DOC_FSUB_PER_FRAME_HOOK, 0);
    RSUB(DOC_FSUB_UPDATE_002F0428, 0);

    RL(EV_INPUT_LATCH, 0, 0);
    RL(EV_REND, 0, 0);
    RL(EV_FUNLOCK, DOC_MUTEX_FRAME, 0);

    RSUB(DOC_FSUB_UPDATE_00253B78, 0);
    RSUB(DOC_FSUB_UPDATE_002F0508, 0);

    ka = engine_test_mode_flag(&e->state, ENGINE_FLAG_LOOP_KEEPALIVE);
    if (ka == 0 &&
        (engine_test_mode_flag(&e->state, ENGINE_FLAG_DIAG) != 0 ||
         g_world->exit_query != 0)) {
        return DOC_FRAME_STOP;
    }

    RL(EV_PRESENT, 0, 0);
    e->frame_counter++;
    RL(EV_FRAME_ADVANCE, 0, 0);
    return DOC_FRAME_CONTINUE;
}

static void ref_drain(DocEngine *e, Log *L) {
    int i = 0x1f;
    (void)e;
    RSUB(DOC_FSUB_RENDER_TEARDOWN_0030B478, 1);
    do {
        i--;
        RL(EV_FLOCK, DOC_MUTEX_FRAME, 0);
        RL(EV_RBEGIN, 0, 0);
        RSUB(DOC_FSUB_SCENE_FLUSH_002E29F0, 0);
        RL(EV_REND, 0, 0);
        RL(EV_FUNLOCK, DOC_MUTEX_FRAME, 0);
        RL(EV_PRESENT, 0, 0);
    } while (-1 < i);
}

static int ref_run(DocEngine *e, Log *L) {
    int frames = 0;
    ref_preamble(e, L);
    while (ref_frame(e, L) == DOC_FRAME_CONTINUE) frames++;
    e->shutdown_marker = 0x70;
    if (e->audio_teardown_enabled != 0) {
        RL(EV_FLOCK, DOC_MUTEX_SHUTDOWN, 0);
        RSUB(DOC_FSUB_SHUTDOWN_0041AF80, 0x14);
        RL(EV_FUNLOCK, DOC_MUTEX_SHUTDOWN, 0);
    }
    e->shutdown_clear = 0;
    RSUB(DOC_FSUB_SHUTDOWN_00403578, 0);
    if (g_world->render_pending != 0) RL(EV_REND, 0, 0);
    ref_drain(e, L);
    return frames;
}

/* ===================================================================== *
 * Diff helpers
 * ===================================================================== */
static const char *evname(int ev) {
    switch (ev) {
    case EV_FLOCK: return "FLOCK"; case EV_FUNLOCK: return "FUNLOCK";
    case EV_RBEGIN: return "RBEGIN"; case EV_RSUBMIT: return "RSUBMIT";
    case EV_REND: return "REND"; case EV_PRESENT: return "PRESENT";
    case EV_INPUT_POLL: return "IPOLL"; case EV_INPUT_LATCH: return "ILATCH";
    case EV_STATE_DISPATCH: return "DISPATCH"; case EV_OBJLIST: return "OBJLIST";
    case EV_FRAME_ADVANCE: return "FADV"; case EV_SUB: return "SUB";
    default: return "?";
    }
}
static void diff_logs(const Log *a, const Log *b, const char *label) {
    int i;
    if (a->n != b->n) {
        CHECK(0, "%s: log length clean=%d raw=%d", label, a->n, b->n);
        return;
    }
    for (i = 0; i < a->n; i++) {
        if (a->e[i][0] != b->e[i][0] || a->e[i][1] != b->e[i][1] || a->e[i][2] != b->e[i][2]) {
            CHECK(0, "%s: log[%d] clean={%s,%d,%d} raw={%s,%d,%d}", label, i,
                  evname(a->e[i][0]), a->e[i][1], a->e[i][2],
                  evname(b->e[i][0]), b->e[i][1], b->e[i][2]);
            return; /* first divergence is enough */
        }
    }
}
static void diff_engine(const DocEngine *c, const DocEngine *r, const char *label) {
    CHECK(c->frame_counter == r->frame_counter, "%s: frame_counter clean=%d raw=%d",
          label, c->frame_counter, r->frame_counter);
    CHECK(c->state.mode_flags == r->state.mode_flags, "%s: mode_flags clean=0x%x raw=0x%x",
          label, c->state.mode_flags, r->state.mode_flags);
    CHECK(c->shutdown_marker == r->shutdown_marker, "%s: shutdown_marker clean=%d raw=%d",
          label, c->shutdown_marker, r->shutdown_marker);
    CHECK(c->shutdown_clear == r->shutdown_clear, "%s: shutdown_clear clean=%d raw=%d",
          label, c->shutdown_clear, r->shutdown_clear);
}

/* ===================================================================== *
 * Parity across a matrix of synthetic environments, N frames each.
 * ===================================================================== */
static DocEngine make_engine(const World *w, DocFrameHooks *h, DocRenderer *r) {
    DocEngine e;
    doc_engine_init(&e, NULL, r, h, NULL);
    /* gates are engine fields; derive a deterministic set from the world */
    e.render_overlay_enabled = (w->boot_cond & 1);
    e.audio_teardown_enabled = (w->render_pending & 1);
    e.misc_update_arg        = 0x1234;
    return e;
}

static void parity_case(World w, int keepalive, int diag, const char *label) {
    DocFrameHooks h = logging_hooks();
    DocRenderer   r = logging_renderer();
    Log clean = {0}, raw = {0};
    DocEngine ce = make_engine(&w, &h, &r);
    DocEngine re = make_engine(&w, &h, &r);
    DocFrameResult rc, rr;
    int frame;

    if (keepalive) {
        engine_set_mode_flag(&ce.state, ENGINE_FLAG_LOOP_KEEPALIVE, 1);
        engine_set_mode_flag(&re.state, ENGINE_FLAG_LOOP_KEEPALIVE, 1);
    }
    if (diag) {
        engine_set_mode_flag(&ce.state, ENGINE_FLAG_DIAG, 1);
        engine_set_mode_flag(&re.state, ENGINE_FLAG_DIAG, 1);
    }

    g_world = &w;

    /* preamble parity */
    g_log = &clean; doc_engine_frame_preamble(&ce);
    ref_preamble(&re, &raw);
    diff_logs(&clean, &raw, label);

    /* N synthetic frames, diffing the per-frame log each time */
    for (frame = 0; frame < 8; frame++) {
        clean.n = 0; raw.n = 0;
        g_log = &clean;
        rc = doc_engine_frame(&ce);
        rr = ref_frame(&re, &raw);
        CHECK(rc == rr, "%s frame %d: result clean=%d raw=%d", label, frame, rc, rr);
        diff_logs(&clean, &raw, label);
        if (rc == DOC_FRAME_STOP) break;
    }
    diff_engine(&ce, &re, label);
}

static void test_parity_matrix(void) {
    World w;
    /* baseline: everything off -> runs forever (we only do 8 frames) */
    memset(&w, 0, sizeof(w));
    parity_case(w, 0, 0, "baseline");

    /* render-sync on (exercises both 003dcf58 blocks) */
    memset(&w, 0, sizeof(w)); w.render_sync = 1;
    parity_case(w, 0, 0, "rsync");

    /* per-frame hook present */
    memset(&w, 0, sizeof(w)); w.per_frame_hook = 1;
    parity_case(w, 0, 0, "perhook");

    /* render-overlay gate on (boot_cond odd -> render_overlay_enabled) */
    memset(&w, 0, sizeof(w)); w.boot_cond = 1;
    parity_case(w, 0, 0, "overlay");

    /* exit query fires but keepalive holds the loop -> never stops */
    memset(&w, 0, sizeof(w)); w.exit_query = 1;
    parity_case(w, /*keepalive*/1, 0, "exit+keepalive");

    /* exit query fires, no keepalive -> STOP on frame 0 */
    memset(&w, 0, sizeof(w)); w.exit_query = 1;
    parity_case(w, 0, 0, "exit");

    /* diag flag set, no keepalive -> STOP on frame 0 (exit_query irrelevant) */
    memset(&w, 0, sizeof(w));
    parity_case(w, 0, /*diag*/1, "diag-stop");

    /* everything on */
    memset(&w, 0, sizeof(w));
    w.render_sync = 1; w.per_frame_hook = 1; w.boot_cond = 1; w.render_pending = 1;
    parity_case(w, 0, 0, "all-on");
}

/* ===================================================================== *
 * Full-run parity (preamble + loop-to-exit + shutdown + drain).
 * ===================================================================== */
static void test_run_parity(void) {
    DocFrameHooks h = logging_hooks();
    DocRenderer   r = logging_renderer();
    World w;
    Log clean = {0}, raw = {0};
    DocEngine ce, re;
    int cf, rf;

    /* Exit fires only after the engine accrues frames: emulate by setting a
     * keepalive-free engine whose exit_query is already 1 BUT letting a few
     * frames pass first via keepalive, then clearing it. Simpler: run with
     * exit_query=1 and keepalive=0 so it stops on frame 0, exercising the
     * shutdown + drain path (that is where the long drain log lives). */
    memset(&w, 0, sizeof(w));
    w.exit_query = 1; w.render_pending = 1;

    ce = make_engine(&w, &h, &r);
    re = make_engine(&w, &h, &r);
    g_world = &w;

    g_log = &clean;
    cf = doc_engine_run(&ce);
    rf = ref_run(&re, &raw);

    CHECK(cf == rf, "run: frames clean=%d raw=%d", cf, rf);
    diff_logs(&clean, &raw, "run");
    diff_engine(&ce, &re, "run");
    CHECK(ce.shutdown_marker == 0x70, "run: shutdown_marker=0x%x", ce.shutdown_marker);
    /* The drain is exactly 32 iterations (do{i--;...}while(-1<i), i=0x1f).
     * Frame 0 STOPs (exit_query=1, no keepalive) so it emits no present; the
     * preamble's only present is mode 1. So every mode-0 present in the whole
     * clean run log comes from the drain: assert there are exactly 32. */
    {
        int i, p0 = 0;
        for (i = 0; i < clean.n; i++)
            if (clean.e[i][0] == EV_PRESENT && clean.e[i][1] == 0) p0++;
        CHECK(p0 == 32, "run: drain present(0) count=%d (expected 32)", p0);
    }
}

/* ===================================================================== *
 * Exit semantics: a stopping frame neither presents nor advances.
 * ===================================================================== */
static void test_exit_semantics(void) {
    DocFrameHooks h = logging_hooks();
    DocRenderer   r = logging_renderer();
    World w;
    Log lg = {0};
    DocEngine e;
    DocFrameResult rc;
    int i, present = 0, advance = 0;

    memset(&w, 0, sizeof(w)); w.exit_query = 1;
    e = make_engine(&w, &h, &r);
    g_world = &w; g_log = &lg;

    rc = doc_engine_frame(&e);
    CHECK(rc == DOC_FRAME_STOP, "exit: expected STOP");
    CHECK(e.frame_counter == 0, "exit: frame_counter must stay 0, got %d", e.frame_counter);
    for (i = 0; i < lg.n; i++) {
        if (lg.e[i][0] == EV_PRESENT) present++;
        if (lg.e[i][0] == EV_FRAME_ADVANCE) advance++;
    }
    CHECK(present == 0, "exit: no present on a stopping frame, got %d", present);
    CHECK(advance == 0, "exit: no advance on a stopping frame, got %d", advance);

    /* A continuing frame DOES present + advance exactly once. */
    memset(&w, 0, sizeof(w));
    e = make_engine(&w, &h, &r);
    g_world = &w; lg.n = 0; g_log = &lg;
    rc = doc_engine_frame(&e);
    CHECK(rc == DOC_FRAME_CONTINUE, "cont: expected CONTINUE");
    CHECK(e.frame_counter == 1, "cont: frame_counter=1, got %d", e.frame_counter);
    present = advance = 0;
    for (i = 0; i < lg.n; i++) {
        if (lg.e[i][0] == EV_PRESENT) present++;
        if (lg.e[i][0] == EV_FRAME_ADVANCE) advance++;
    }
    CHECK(present == 1, "cont: exactly one present, got %d", present);
    CHECK(advance == 1, "cont: exactly one advance, got %d", advance);
}

/* ===================================================================== *
 * Integration: default hooks drive the REAL ported subsystems.
 * ===================================================================== */
static void test_integration(void) {
    DocFrameHooks h = doc_frame_hooks_default();  /* real dispatch + object list */
    DocRenderer   r = doc_renderer_default();
    EngineHooks   dh = engine_hooks_default();
    DocEngine e;
    int i;

    doc_engine_init(&e, &dh, &r, &h, NULL);
    e.state.scene_state = 0;                      /* dispatch default case: no-op */
    doc_scene_object_list_set_enabled(0);         /* object list: nothing to walk */

    for (i = 0; i < 5; i++) {
        DocFrameResult rc = doc_engine_frame(&e);
        CHECK(rc == DOC_FRAME_CONTINUE, "integration: frame %d should continue", i);
    }
    /* The loop's own pacing counter advanced once per CONTINUE... */
    CHECK(e.frame_counter == 5, "integration: loop frame_counter=%d (expected 5)", e.frame_counter);
    /* ...and the ported state dispatch ran once per frame (its own counter). */
    CHECK(e.state.frame_counter == 5, "integration: dispatch frame_counter=%d (expected 5)",
          e.state.frame_counter);
    CHECK(e.state.scene_state == 0, "integration: scene_state stable at 0, got %d",
          e.state.scene_state);
}

int main(void) {
    test_parity_matrix();
    test_run_parity();
    test_exit_semantics();
    test_integration();

    if (g_fail) { printf("frame_loop: %d FAILURE(S)\n", g_fail); return 1; }
    printf("frame_loop: all checks passed (call-sequence parity, exit semantics, "
           "full-run parity, integration)\n");
    return 0;
}
