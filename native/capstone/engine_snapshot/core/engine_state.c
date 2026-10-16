/* =====================================================================
 * engine_state.c  --  Reauthored ENGINE SPINE (see engine_state.h).
 *
 * Primitives (FUN_00241d38/d68/41378/413c0) + the per-frame state
 * dispatch (FUN_00247508). The dispatch reproduces the PURE STATE LOGIC
 * verbatim from the decomp; every below-seam call is routed through the
 * EngineHooks seam (queries return control-flow ints; side effects go to
 * the subsystem sink). No below-seam behaviour is faked.
 * ===================================================================== */
#include "engine_state.h"
#include <string.h>

/* --------------------------------------------------------------------- *
 * State / flag primitives
 * --------------------------------------------------------------------- */
void engine_state_init(EngineState *es) {
    memset(es, 0, sizeof(*es));
}

/* FUN_00241d38 */
void engine_set_scene_state(EngineState *es, int32_t next) {
    /* The guard compares the RAW argument (possibly negative) with the
     * current state, so a pop request (next<0) always passes it unless
     * the machine were already in a negative state (it is not). */
    if (next != es->scene_state) {
        if (next < 0) {
            next = es->prev_scene_state;   /* pop to previous (1-deep back-stack) */
        }
        es->prev_scene_state = es->scene_state;
        es->scene_state = next;
    }
}

/* FUN_00241d68 */
int32_t engine_get_scene_state(const EngineState *es) {
    return es->scene_state;
}

/* FUN_00241378 */
void engine_set_mode_flag(EngineState *es, unsigned bit, int on) {
    uint32_t mask = 1u << (bit & 0x1f);
    if (on) {
        es->mode_flags |= mask;
    } else {
        es->mode_flags &= ~mask;
    }
}

/* FUN_002413c0 */
int engine_test_mode_flag(const EngineState *es, unsigned bit) {
    return (es->mode_flags & (1u << (bit & 0x1f))) != 0;
}

/* --------------------------------------------------------------------- *
 * Default (all-safe) hooks
 * --------------------------------------------------------------------- */
static int  noop_q0(void *c)                               { (void)c; return 0; }
static int  noop_q1(void *c, int a)                        { (void)c; (void)a; return 0; }
static int  noop_open(void *c, int a, int b)               { (void)c; (void)a; (void)b; return 0; }
static int  noop_make(void *c, int a, int b, int d, int e) { (void)c; (void)a; (void)b; (void)d; (void)e; return 0; }
static void noop_sub(void *c, int t, int a, int b, int d)  { (void)c; (void)t; (void)a; (void)b; (void)d; }
static void noop_fatal(void *c, int code)                  { (void)c; (void)code; }

EngineHooks engine_hooks_default(void) {
    EngineHooks h;
    memset(&h, 0, sizeof(h));
    h.ctx = NULL;
    h.title_boot_poll     = noop_q0;
    h.clock_now           = noop_q0;
    h.save_slot_check     = noop_q0;
    h.boot_target_resolve = noop_q0;
    h.alt_boot_poll       = noop_q0;
    h.title_update_poll   = noop_q0;
    h.input_test          = noop_q1;
    h.player_ctx_read     = noop_q1;
    h.disc_ctx_read       = noop_q0;
    h.field_boot_mode     = noop_q0;
    h.next_file_code      = noop_q0;
    h.file_open           = noop_open;
    h.file_status         = noop_q1;
    h.fade_poll           = noop_q0;
    h.pause_poll          = noop_q0;
    h.field_enter_poll    = noop_q0;
    h.field_cam           = noop_q1;
    h.res_param           = noop_q1;
    h.field_result_make   = noop_make;
    h.field_run_poll      = noop_q1;
    h.field_exit_poll     = noop_q1;
    h.field_substate      = noop_q0;
    h.region_check        = noop_q0;
    h.prev_state_query    = noop_q0;
    h.event_sync_poll     = noop_q0;
    h.title_return_poll   = noop_q1;
    h.intro_poll          = noop_q0;
    h.intro_fade_poll     = noop_q0;
    h.reload_poll         = noop_q0;
    h.subsystem           = noop_sub;
    h.fatal               = noop_fatal;
    return h;
}

/* --------------------------------------------------------------------- *
 * The per-frame state dispatch (FUN_00247508).
 *
 * Shorthands kept local so the transition logic reads like the decomp
 * switch. SET(x) == engine_set_scene_state; SUB(tag,...) == the below-seam
 * side-effect sink.
 * --------------------------------------------------------------------- */
void engine_state_dispatch(EngineState *es, const EngineHooks *hooks) {
    void *ctx = hooks->ctx;
    int32_t state;

#define SET(x)         engine_set_scene_state(es, (int32_t)(x))
#define SUB(tag, a, b, c) hooks->subsystem(ctx, (int)(tag), (int)(a), (int)(b), (int)(c))

    /* ----- preamble: runs every frame, before the switch ----- */
    state = engine_get_scene_state(es);
    es->frame_counter++;                       /* DAT_004bcd84 + 1 */
    SUB(ENGINE_SUB_PREUPDATE_1, 0, 0, 0);      /* FUN_00246c98 */
    SUB(ENGINE_SUB_PREUPDATE_2, 0, 0, 0);      /* FUN_00247048 */
    SUB(ENGINE_SUB_PREUPDATE_3, 0, 0, 0);      /* FUN_00247318 */
    SUB(ENGINE_SUB_RESET_SCRATCH, 0, 0, 0);    /* DAT_004fc7d8 = 0 */
    SUB(ENGINE_SUB_PREUPDATE_4, 0, 0, 0);      /* FUN_003fdf00 */

    switch (state) {

    case SCENE_STATE_BOOT: /* 1 */
        if (hooks->title_boot_poll(ctx) > 0) {
            SUB(ENGINE_SUB_PLAY_MOVIE, 0xb4, 0, 1);
            SUB(ENGINE_SUB_SCENE_BEGIN, 2, 0, 0);
            SUB(ENGINE_SUB_SCENE_OVERLAY, 0, 0, 0);
            SET(SCENE_STATE_TITLE);
            return;
        }
        break; /* no transition */

    case SCENE_STATE_TITLE: /* 2 */
        es->boot_clock_mark = hooks->clock_now(ctx);
        if (hooks->save_slot_check(ctx) == 0) {
            es->boot_target = hooks->boot_target_resolve(ctx);
            if (es->boot_target != 0) {
                SUB(ENGINE_SUB_FADE, 0x3f, 500, 0);
                es->boot_submode = 1;
                SUB(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
                if (es->boot_target == 9) {
                    SET(SCENE_STATE_TITLE_SUB);
                } else {
                    SET(SCENE_STATE_TITLE_HOLD);
                }
            }
            if (hooks->alt_boot_poll(ctx) == 0) {
                break; /* no transition */
            }
            es->boot_submode = 2;
            SUB(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            SUB(ENGINE_SUB_FADE, 0x3f, 500, 0);
        } else {
            SUB(ENGINE_SUB_FADE, 0x3f, 0, 0);
            es->boot_submode = 0;
            SUB(ENGINE_SUB_MISC, 0x70, 0, 0);      /* DAT_004fcaa0 = 0x70 */
            SUB(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            SUB(ENGINE_SUB_INPUT_MODE, 0, 1, 1);
        }
        SET(SCENE_STATE_TITLE_HOLD);               /* LAB_00247720 */
        break;

    case SCENE_STATE_TITLE_SUB: /* 3 */
        engine_set_mode_flag(es, ENGINE_FLAG_LOOP_KEEPALIVE, 1);
        if (hooks->title_update_poll(ctx) == 0) {
            return;
        }
        SET(SCENE_STATE_TITLE_HOLD);               /* LAB_00247720 */
        break;

    case SCENE_STATE_LOAD_DISPATCH: /* 4 : the load hub */
        if (es->boot_submode == 0) {
            SUB(ENGINE_SUB_FADE, 0, 0, 0);
            SET(SCENE_STATE_BOOT_FINALIZE);        /* LAB_002480b8 */
            break;
        }
        if (es->boot_submode != 1) {
            SUB(ENGINE_SUB_FADE, 0, 0, 0);
            SET(SCENE_STATE_FIELD_SETUP);          /* caseD_1 */
            return;
        }
        /* boot_submode == 1 */
        if (hooks->input_test(ctx, 0x12) == 0) {
            /* FUN_002419d8(0xc) is still polled; both sub-paths play movie 0xb4 */
            (void)hooks->input_test(ctx, 0xc);
            SUB(ENGINE_SUB_PLAY_MOVIE, 0xb4, 0, 1);
        } else {
            SUB(ENGINE_SUB_EVENT_HINT, 3, 0, 0);
            SUB(ENGINE_SUB_PLAY_MOVIE, 0x7a, 0, 1);
        }
        SUB(ENGINE_SUB_FADE, 0, 0, 0);
        switch (es->boot_target) {                 /* DAT_0046b3c0 routing */
        case 3:
            SUB(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            SET(SCENE_STATE_BOOT_FILE_OPEN);       /* 9 */
            break;
        case 4:
            SET(SCENE_STATE_BOOT);                 /* 1 */
            SET(SCENE_STATE_RELOAD);               /* LAB_00247c34: 0xd; return */
            return;
        case 5:
            SET(SCENE_STATE_SYS_FILE_OPEN);        /* caseD_5: 8 */
            break;
        default:
            break;                                 /* caseD_6 */
        case 8:
            if (hooks->input_test(ctx, 3) == 0) {
                if (hooks->input_test(ctx, 5) == 0) {
                    hooks->fatal(ctx, 1);          /* trap(1) */
                } else {
                    SUB(ENGINE_SUB_SAVE_LOAD_BYTE, hooks->player_ctx_read(ctx, 0xc0), 0, 0);
                    SUB(ENGINE_SUB_SAVE_APPLY, 0, 0, 0);
                    SUB(ENGINE_SUB_SAVE_SELECT, 1, 0, 0);
                }
            } else {
                SUB(ENGINE_SUB_SAVE_LOAD_BYTE, hooks->disc_ctx_read(ctx), 0, 0);
                SUB(ENGINE_SUB_SAVE_SELECT, 0, 0, 0);
            }
            SUB(ENGINE_SUB_SCENE_PREP, 0x14, 0, 0);
            SUB(ENGINE_SUB_FRAME_ADVANCE, 0, 0, 0);
            SET(SCENE_STATE_FIELD_SETUP);          /* 0x13 */
            SUB(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            break;
        case 9:
            SUB(ENGINE_SUB_LAYER_ENABLE, 0, 1, 0);
            SUB(ENGINE_SUB_BOOT_SETUP, 1, 0, 0);
            SUB(ENGINE_SUB_FRAME_MUTEX_LOCK, 0, 0, 0);
            SUB(ENGINE_SUB_MISC, 1, 0, 0);         /* DAT_0046b358 = 1 */
            SUB(ENGINE_SUB_RENDER_VTABLE_A, 0, 0, 0);
            SUB(ENGINE_SUB_FRAME_MUTEX_UNLK, 0, 0, 0);
            SUB(ENGINE_SUB_RENDER_MISC_D, 0, 0, 0);
            SUB(ENGINE_SUB_RENDER_VTABLE_B, 0, 0, 0);
            SUB(ENGINE_SUB_PLAY_MOVIE, 0xd9, 0, 1);
            SUB(ENGINE_SUB_RENDER_TOGGLE, 1, 0, 0);
            SUB(ENGINE_SUB_RENDER_FLUSH, 0, 0, 0);
            /* FALLTHROUGH to case 2 (decomp has no break here) */
            /* fall through */
        case 2:
            SUB(ENGINE_SUB_RENDER_MISC_A, 0, 0, 0);
            SUB(ENGINE_SUB_RENDER_MISC_B, 1, 0, 0);
            SUB(ENGINE_SUB_LAYER_ENABLE, 5, 1, 0);
            SUB(ENGINE_SUB_RENDER_MISC_C, 0, 0, 0);
            SUB(ENGINE_SUB_SCENE_PREP, 0x14, 0, 0);
            SET(SCENE_STATE_FIELD_SETUP);          /* 0x13 */
            SUB(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            SUB(ENGINE_SUB_FRAME_ADVANCE, 0, 0, 0);
            if (hooks->field_boot_mode(ctx) == 3) {
                SUB(ENGINE_SUB_FIELD_SPAWN, 0, 0, 0);
            }
            break;
        case 10:
            SET(SCENE_STATE_EVENT_OPEN);           /* caseD_a: 0x18; return */
            return;
        case 0xd:
            SET(SCENE_STATE_EVENT_DONE);           /* 0x1c */
            break;
        case 0xe:
            es->event_file_code = 0x3010071;
            SET(SCENE_STATE_EVENT_OPEN);           /* 0x18 */
            break;
        }
        break;

    case SCENE_STATE_TITLE_HOLD: /* 5 */
        engine_set_mode_flag(es, ENGINE_FLAG_LOOP_KEEPALIVE, 1);
        {
            uint32_t now = (uint32_t)hooks->clock_now(ctx);
            /* unsigned compare, exactly as the decomp (DAT_004bcbcc + 600U) */
            if ((uint32_t)((uint32_t)es->boot_clock_mark + 600u) < now) {
                SUB(ENGINE_SUB_ATTRACT_PREP, 0, 0, 0);
                SET(SCENE_STATE_ATTRACT);          /* 0x20 */
            }
        }
        break;

    case SCENE_STATE_BOOT_FILE_OPEN: /* 9 */
        SUB(ENGINE_SUB_LAYER_ENABLE, 7, 1, 0);
        {
            int code = hooks->next_file_code(ctx);
            es->file_handle = hooks->file_open(ctx, 0x3000011, code);
            SUB(ENGINE_SUB_FILE_KICK, es->file_handle, 0, 0);
        }
        SET(SCENE_STATE_BOOT_FILE_WAIT);           /* 10 */
        break;

    case SCENE_STATE_BOOT_FILE_WAIT: /* 0xa */
        {
            int st = hooks->file_status(ctx, es->file_handle);   /* handle+0x23a */
            if (st != 3) {
                if (st != 4) {
                    return;                        /* still loading */
                }
                /* error path */
                SET(SCENE_STATE_BOOT);             /* 1 */
                SUB(ENGINE_SUB_FILE_CLOSE, es->file_handle, 0, 0);
                es->file_handle = 0;
                SUB(ENGINE_SUB_FILE_FINISH, 1, 0, 0);   /* LAB_00247f3c */
                break;
            }
            /* done */
            SET(SCENE_STATE_BOOT_FADE);            /* 0xb */
            SUB(ENGINE_SUB_FILE_CLOSE, es->file_handle, 0, 0);
            SUB(ENGINE_SUB_FILE_FINISH, 0, 0, 0);
            SUB(ENGINE_SUB_BOOT_COMMIT, 0, 0, 0);  /* FUN_002438c0 */
            es->file_handle = 0;
        }
        break;

    case SCENE_STATE_BOOT_FADE: /* 0xb */
        if (hooks->fade_poll(ctx) == 0) {
            return;
        }
        SUB(ENGINE_SUB_BOOT_COMMIT, 1, 0, 0);      /* FUN_002438e0 */
        if (engine_test_mode_flag(es, ENGINE_FLAG_FADE_HOLD) == 0) {
            SET(SCENE_STATE_BOOT);                 /* 1 */
        } else {
            SET(SCENE_STATE_EVENT_DONE);           /* 0x1c */
            engine_set_mode_flag(es, ENGINE_FLAG_FADE_HOLD, 0);
        }
        SUB(ENGINE_SUB_LAYER_ENABLE, 7, 0, 0);
        SUB(ENGINE_SUB_FILE_FINISH, 1, 0, 0);      /* LAB_00247f3c */
        break;

    case SCENE_STATE_BOOT_FINALIZE: /* 0x10 */
        if (engine_test_mode_flag(es, ENGINE_FLAG_FIELD_READY) == 0 &&
            hooks->pause_poll(ctx) == 0) {
            return;
        }
        SET(SCENE_STATE_BOOT);                     /* caseD_2: 1 */
        break;

    case SCENE_STATE_FIELD_SETUP: /* 0x13 */
        SUB(ENGINE_SUB_RENDER_TOGGLE, 0, 0, 0);
        if (hooks->input_test(ctx, 4) != 0 || hooks->input_test(ctx, 10) != 0) {
            SUB(ENGINE_SUB_RENDER_FLUSH, 0, 0, 0);
            if (hooks->input_test(ctx, 10) != 0) {
                SUB(ENGINE_SUB_RENDER_BIND, 0xe, 0xf, 0);
                SUB(ENGINE_SUB_RENDER_SYNC_A, 0, 0, 0);
            }
            if (hooks->input_test(ctx, 10) != 0) {
                SUB(ENGINE_SUB_RENDER_SYNC_B, 0, 0, 0);
            }
        }
        SUB(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
        SUB(ENGINE_SUB_INPUT_MODE, 0, 0, 0);
        if (es->field_handle != 0) {
            SUB(ENGINE_SUB_FIELD_FREE, es->field_handle, 3, 0);
        }
        if (hooks->field_enter_poll(ctx) >= 0) {
            if (hooks->input_test(ctx, 10) == 0) {
                int a = hooks->res_param(ctx, 0xd0);
                int b = hooks->field_enter_poll(ctx);
                int c = hooks->field_cam(ctx, 0);
                int d = hooks->field_cam(ctx, 1);
                es->field_handle = hooks->field_result_make(ctx, a, b, c, d);
            } else {
                int a = hooks->res_param(ctx, 0xd0);
                es->field_handle = hooks->field_result_make(ctx, a, 0x81, 0, 0);
                SUB(ENGINE_SUB_EVENT_HINT, 1, 0, 0);
            }
            SUB(ENGINE_SUB_FIELD_FINALIZE, 0, 0, 0);
            SET(SCENE_STATE_FIELD_LOAD);           /* 0x14; return */
            return;
        }
        SET(SCENE_STATE_BOOT);                     /* caseD_2: 1 */
        break;

    case SCENE_STATE_FIELD_LOAD: /* 0x14 */
        {
            int r = hooks->field_run_poll(ctx, es->field_handle);
            if (r > 0) {
                SET(SCENE_STATE_FIELD_RUN);        /* 0x15; return */
                return;
            }
            if (r >= 0) {
                return;                            /* still loading */
            }
            if (es->field_handle != 0) {
                SUB(ENGINE_SUB_FIELD_FREE, es->field_handle, 3, 0);
            }
            es->field_handle = 0;
            SET(SCENE_STATE_BOOT);                 /* caseD_2: 1 */
        }
        break;

    case SCENE_STATE_FIELD_RUN: /* 0x15 */
        SUB(ENGINE_SUB_FIELD_PREP, 0, 0, 0);
        {
            int r = hooks->field_exit_poll(ctx, es->field_handle);
            if (r == 0) {
                break;                             /* caseD_6: no transition */
            }
            if (r == 2) {
                SET(SCENE_STATE_RELOAD);           /* LAB_00247c34: 0xd; return */
                return;
            }
            SUB(ENGINE_SUB_FIELD_TEARDOWN, 0, 0, 0);
            if (es->field_handle != 0) {
                SUB(ENGINE_SUB_FIELD_FREE, es->field_handle, 3, 0);
                es->field_handle = 0;
            }
            if (r == 3) {
                SUB(ENGINE_SUB_FIELD_EXTRA, 0, 0, 0);
                SET(SCENE_STATE_SYS_FILE_OPEN);    /* caseD_5: 8 */
            } else if (r == 5) {
                SUB(ENGINE_SUB_FIELD_EXTRA, 0, 0, 0);
                SET(SCENE_STATE_EVENT_OPEN);       /* caseD_a: 0x18; return */
                return;
            } else if (r == 6 || r == 1) {
                if (r == 6) {
                    SUB(ENGINE_SUB_SCRIPT_RESET, -1, -1, 0);   /* FUN_003ea1d0(-1) */
                    SUB(ENGINE_SUB_SCRIPT_RESET, -1, -1, 1);   /* FUN_003e62d0(-1) */
                    if (hooks->input_test(ctx, 10) == 0) {
                        SUB(ENGINE_SUB_LAYER_ENABLE, 0xd, 1, 0);
                    } else {
                        SUB(ENGINE_SUB_RENDER_FLUSH, 0, 0, 0);
                    }
                    if (hooks->field_substate(ctx) == 1) {
                        int keep =
                            (hooks->player_ctx_read(ctx, 0x41881) == 0) ||
                            ((hooks->region_check(ctx) != 0 &&
                              hooks->player_ctx_read(ctx, 0x41850) == 0x3f &&
                              hooks->player_ctx_read(ctx, 0x41881) != 0) ||
                             hooks->input_test(ctx, 0x12) != 0);
                        if (keep) {
                            if (hooks->input_test(ctx, 0xe) != 0) {
                                SUB(ENGINE_SUB_LAYER_ENABLE, 0xd, 0, 0);
                                SUB(ENGINE_SUB_LAYER_ENABLE, 0xe, 0, 0);
                            }
                            SUB(ENGINE_SUB_RENDER_MISC_C, 0, 0, 0);
                            if (hooks->field_boot_mode(ctx) == 3) {
                                SUB(ENGINE_SUB_FIELD_SPAWN, 0, 0, 0);
                            } else {
                                SUB(ENGINE_SUB_RENDER_FLUSH, 0, 0, 0);
                            }
                            SUB(ENGINE_SUB_LAYER_ENABLE, 5, 1, 0);
                            SUB(ENGINE_SUB_LAYER_ENABLE, 0xd, 0, 0);
                            SUB(ENGINE_SUB_LAYER_ENABLE, 0xe, 0, 0);
                        } else {
                            SUB(ENGINE_SUB_SAVE_APPLY, 0, 0, 0);
                            SUB(ENGINE_SUB_LAYER_ENABLE, 3, 1, 0);
                        }
                    } else if (hooks->field_substate(ctx) == 0) {
                        SUB(ENGINE_SUB_LAYER_ENABLE, 3, 1, 0);
                    }
                }
                SET(SCENE_STATE_FIELD_SETUP);      /* caseD_1: 0x13; return */
                return;
            } else {
                SET(SCENE_STATE_BOOT);             /* caseD_2: 1 */
            }
        }
        break;

    case SCENE_STATE_FIELD_RESUME: /* 0x17 */
        if (hooks->prev_state_query(ctx) == 0x15) {
            SUB(ENGINE_SUB_FIELD_PREP, 0, 0, 0);
        }
        SUB(ENGINE_SUB_RESUME_DRAW, 0, 0, 0);      /* FUN_00379908(DAT_0046f850) */
        break;

    case SCENE_STATE_EVENT_OPEN: /* 0x18 */
        {
            int code = hooks->next_file_code(ctx);
            es->file_handle = hooks->file_open(ctx, es->event_file_code, code);
            SUB(ENGINE_SUB_FILE_KICK, es->file_handle, 0, 0);
        }
        SET(SCENE_STATE_EVENT_WAIT);               /* 0x19 */
        break;

    case SCENE_STATE_EVENT_WAIT: /* 0x19 */
        if (engine_test_mode_flag(es, ENGINE_FLAG_EVENT_READY) &&
            hooks->event_sync_poll(ctx) == 0 &&
            hooks->file_status(ctx, es->file_handle) == 3) {
            SUB(ENGINE_SUB_FILE_CLOSE, es->file_handle, 0, 0);
            SUB(ENGINE_SUB_INPUT_GATE, 1, 0, 0);
            SUB(ENGINE_SUB_FADE_SET, 0x7f, 0, 0);
            SET(SCENE_STATE_EVENT_RUN);            /* 0x1a */
        }
        break;

    case SCENE_STATE_EVENT_RUN: /* 0x1a */
        {
            int r = hooks->fade_poll(ctx);
            if (r >= 0) {
                return;
            }
            if (r == -2) {
                SET(SCENE_STATE_EVENT_OPEN);       /* caseD_a: 0x18; return */
                return;
            }
            if (hooks->alt_boot_poll(ctx) != 0) {
                engine_set_mode_flag(es, ENGINE_FLAG_EVENT_SKIP, 1);
                SET(SCENE_STATE_FIELD_SETUP);      /* caseD_1: 0x13; return */
                return;
            }
            SET(SCENE_STATE_BOOT);                 /* caseD_2: 1 */
        }
        break;

    case SCENE_STATE_INTRO: /* 0x1d */
        {
            int r = hooks->intro_poll(ctx);
            if (r == 0) {
                return;
            }
            if (r < 1 && r != -2) {
                SUB(ENGINE_SUB_INTRO_PREP, 0, 0, 0);
                SET(SCENE_STATE_INTRO_FADE);       /* 0x1e; return */
                return;
            }
            SUB(ENGINE_SUB_INTRO_COMMIT, 0, 0, 0); /* LAB_002480b0 */
            SET(SCENE_STATE_BOOT_FINALIZE);        /* LAB_002480b8: 0x10 */
        }
        break;

    case SCENE_STATE_INTRO_FADE: /* 0x1e */
        if (hooks->intro_fade_poll(ctx) == 0) {
            return;
        }
        SUB(ENGINE_SUB_INTRO_COMMIT, 0, 0, 0);     /* LAB_002480b0 */
        SET(SCENE_STATE_BOOT_FINALIZE);            /* LAB_002480b8: 0x10 */
        break;

    case SCENE_STATE_RELOAD_WAIT: /* 0x1f */
        if (hooks->reload_poll(ctx) == 0) {
            return;
        }
        SET(SCENE_STATE_BOOT);                     /* 1 */
        break;

    case SCENE_STATE_ATTRACT: /* 0x20 */
        {
            int arg = 0;
            if (es->boot_submode != 0) {
                arg = (hooks->input_test(ctx, 10) == 0) ? 1 : 0;
            }
            if (hooks->title_return_poll(ctx, arg) != 0) {
                engine_set_mode_flag(es, ENGINE_FLAG_LOOP_KEEPALIVE, 0);
                SET(SCENE_STATE_LOAD_DISPATCH);    /* 4 */
            }
        }
        break;

    default:
        break; /* no transition */
    }

#undef SET
#undef SUB
}
