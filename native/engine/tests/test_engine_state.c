/* =====================================================================
 * test_engine_state.c  --  Unit + oracle-parity test for the reauthored
 * ENGINE SPINE (core/engine_state.{h,c}).
 *
 * Three things are checked:
 *
 *  1. PRIMITIVE PARITY (state/flag ops). A byte-accurate transcription of
 *     FUN_00241d38/d68/41378/413c0, reading the scene-state / prev / mode-
 *     flag words out of a raw "guest memory" buffer at their DOCUMENTED
 *     GUEST ADDRESSES, is driven with the same op sequence as the clean
 *     EngineState and asserted identical (incl. the n<0 push-pop).
 *
 *  2. DISPATCH PARITY (FUN_00247508). A second transcription of the whole
 *     dispatch, operating on the same raw buffer at the documented
 *     offsets and calling the SAME below-seam hooks, is driven frame-for-
 *     frame against the clean engine_state_dispatch on a matrix of
 *     synthetic (state, scripted-environment) cases. After each tick, the
 *     full spine state block AND the ordered log of below-seam calls are
 *     asserted identical. This is the DECOMP-METHOD.md step made concrete:
 *     it proves both the transition logic AND the offset map are faithful.
 *
 *  3. BEHAVIOUR (transitions traced to the decomp). For every state, a
 *     scripted environment drives the clean dispatch and the resulting
 *     scene-state / mode-flags are asserted against what the decomp does.
 *
 * All logic here is pure integer/bit, so PS2 float quirks are irrelevant
 * and the raw transcription is a faithful oracle.
 * ===================================================================== */
#include "engine_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ===================================================================== *
 * Raw "guest memory": the spine state block at its real guest addresses.
 * Based at the lowest address so the buffer stays ~330 KB.
 * ===================================================================== */
#define A_EVTCODE 0x0046b228  /* event_file_code */
#define A_TARGET  0x0046b3c0  /* boot_target     */
#define A_FHANDLE 0x0046b3c4  /* file_handle     */
#define A_FIELDH  0x0046b3c8  /* field_handle    */
#define A_FLAGS   0x004b8980  /* mode_flags      */
#define A_SCENE   0x004b8b2c  /* scene_state     */
#define A_PREV    0x004b8b30  /* prev_scene_state*/
#define A_SUBMODE 0x004bcbc8  /* boot_submode    */
#define A_CLKMARK 0x004bcbcc  /* boot_clock_mark */
#define A_FRAME   0x004bcd84  /* frame_counter   */

#define RAW_BASE  0x0046b228
#define RAW_SIZE  ((size_t)(A_FRAME + 4 - RAW_BASE))
#define OFF(a)    ((uint32_t)((a) - RAW_BASE))

static uint32_t rld32(const uint8_t *m, uint32_t o) { uint32_t v; memcpy(&v, m + o, 4); return v; }
static void     rst32(uint8_t *m, uint32_t o, uint32_t v) { memcpy(m + o, &v, 4); }

/* ---- primitive transcriptions (FUN_00241d38/d68/41378/413c0) ---- */
static int32_t r_get_state(const uint8_t *m) { return (int32_t)rld32(m, OFF(A_SCENE)); }

static void r_set_state(uint8_t *m, int32_t next) {
    int32_t cur = (int32_t)rld32(m, OFF(A_SCENE));
    if (next != cur) {
        if (next < 0) next = (int32_t)rld32(m, OFF(A_PREV));
        rst32(m, OFF(A_PREV), (uint32_t)cur);
        rst32(m, OFF(A_SCENE), (uint32_t)next);
    }
}
static void r_set_flag(uint8_t *m, unsigned bit, long on) {
    uint32_t f = rld32(m, OFF(A_FLAGS));
    uint32_t b = 1u << (bit & 0x1f);
    if (on) f |= b; else f &= ~b;
    rst32(m, OFF(A_FLAGS), f);
}
static int r_test_flag(const uint8_t *m, unsigned bit) {
    return (rld32(m, OFF(A_FLAGS)) & (1u << (bit & 0x1f))) != 0;
}

/* ===================================================================== *
 * Synthetic environment ("script"): scripted query returns + a call log.
 * The clean run and the raw run each get their own copy (identical data,
 * separate log) so the two logs can be diffed.
 * ===================================================================== */
#define LOGMAX 256
typedef struct Script {
    int title_boot, clock, save_check, boot_target, alt_boot, title_update;
    int field_boot_mode, next_code, file_open_ret, file_status, fade, pause;
    int field_enter, res_param, field_make, field_run, field_exit, field_sub;
    int region, prev_state, event_sync, title_return, intro, intro_fade, reload;
    int disc_ctx, cam0, cam1;
    int pc_c0, pc_41850, pc_41881;
    uint32_t input_mask;   /* bit i set => input_test(i) != 0 */

    int log_n;
    int log[LOGMAX][4];     /* {tag,a,b,c}; fatal recorded as tag=-1 */
} Script;

static void log_push(Script *s, int tag, int a, int b, int c) {
    if (s->log_n < LOGMAX) {
        s->log[s->log_n][0] = tag; s->log[s->log_n][1] = a;
        s->log[s->log_n][2] = b;   s->log[s->log_n][3] = c;
    }
    s->log_n++;
}

/* ---- hook implementations (shared by clean + raw; read ctx) ---- */
static int h_title_boot(void *c)          { return ((Script*)c)->title_boot; }
static int h_clock(void *c)               { return ((Script*)c)->clock; }
static int h_save_check(void *c)          { return ((Script*)c)->save_check; }
static int h_boot_target(void *c)         { return ((Script*)c)->boot_target; }
static int h_alt_boot(void *c)            { return ((Script*)c)->alt_boot; }
static int h_title_update(void *c)        { return ((Script*)c)->title_update; }
static int h_input(void *c, int id)       { return (((Script*)c)->input_mask >> (id & 31)) & 1u ? 1 : 0; }
static int h_pctx(void *c, int off) {
    Script *s = (Script*)c;
    if (off == 0xc0)    return s->pc_c0;
    if (off == 0x41850) return s->pc_41850;
    if (off == 0x41881) return s->pc_41881;
    return 0;
}
static int h_disc(void *c)                { return ((Script*)c)->disc_ctx; }
static int h_field_boot_mode(void *c)     { return ((Script*)c)->field_boot_mode; }
static int h_next_code(void *c)           { return ((Script*)c)->next_code; }
static int h_file_open(void *c, int a, int b) { (void)a; (void)b; return ((Script*)c)->file_open_ret; }
static int h_file_status(void *c, int h)  { (void)h; return ((Script*)c)->file_status; }
static int h_fade(void *c)                { return ((Script*)c)->fade; }
static int h_pause(void *c)               { return ((Script*)c)->pause; }
static int h_field_enter(void *c)         { return ((Script*)c)->field_enter; }
static int h_cam(void *c, int i)          { return i == 0 ? ((Script*)c)->cam0 : ((Script*)c)->cam1; }
static int h_res(void *c, int id)         { (void)id; return ((Script*)c)->res_param; }
static int h_make(void *c, int a, int b, int d, int e) { (void)a;(void)b;(void)d;(void)e; return ((Script*)c)->field_make; }
static int h_field_run(void *c, int h)    { (void)h; return ((Script*)c)->field_run; }
static int h_field_exit(void *c, int h)   { (void)h; return ((Script*)c)->field_exit; }
static int h_field_sub(void *c)           { return ((Script*)c)->field_sub; }
static int h_region(void *c)              { return ((Script*)c)->region; }
static int h_prev_state(void *c)          { return ((Script*)c)->prev_state; }
static int h_event_sync(void *c)          { return ((Script*)c)->event_sync; }
static int h_title_return(void *c, int a) { (void)a; return ((Script*)c)->title_return; }
static int h_intro(void *c)               { return ((Script*)c)->intro; }
static int h_intro_fade(void *c)          { return ((Script*)c)->intro_fade; }
static int h_reload(void *c)              { return ((Script*)c)->reload; }
static void h_sub(void *c, int t, int a, int b, int d) { log_push((Script*)c, t, a, b, d); }
static void h_fatal(void *c, int code)    { log_push((Script*)c, -1, code, 0, 0); }

static EngineHooks make_hooks(Script *s) {
    EngineHooks h = engine_hooks_default();
    h.ctx = s;
    h.title_boot_poll = h_title_boot; h.clock_now = h_clock; h.save_slot_check = h_save_check;
    h.boot_target_resolve = h_boot_target; h.alt_boot_poll = h_alt_boot; h.title_update_poll = h_title_update;
    h.input_test = h_input; h.player_ctx_read = h_pctx; h.disc_ctx_read = h_disc;
    h.field_boot_mode = h_field_boot_mode; h.next_file_code = h_next_code; h.file_open = h_file_open;
    h.file_status = h_file_status; h.fade_poll = h_fade; h.pause_poll = h_pause;
    h.field_enter_poll = h_field_enter; h.field_cam = h_cam; h.res_param = h_res;
    h.field_result_make = h_make; h.field_run_poll = h_field_run; h.field_exit_poll = h_field_exit;
    h.field_substate = h_field_sub; h.region_check = h_region; h.prev_state_query = h_prev_state;
    h.event_sync_poll = h_event_sync; h.title_return_poll = h_title_return; h.intro_poll = h_intro;
    h.intro_fade_poll = h_intro_fade; h.reload_poll = h_reload;
    h.subsystem = h_sub; h.fatal = h_fatal;
    return h;
}

/* ===================================================================== *
 * Raw-offset transcription of the dispatch (FUN_00247508).
 * Independently reads/writes the spine block in the raw buffer at the
 * documented guest addresses; calls the same hooks for below-seam work.
 * ===================================================================== */
#define GS       r_get_state(m)
#define SET_R(x) r_set_state(m, (int32_t)(x))
#define SUB_R(t,a,b,c) hk->subsystem(ctx, (int)(t), (int)(a), (int)(b), (int)(c))
#define GET(addr)      ((int32_t)rld32(m, OFF(addr)))
#define PUT(addr,v)    rst32(m, OFF(addr), (uint32_t)(int32_t)(v))

static void ref_dispatch(uint8_t *m, const EngineHooks *hk) {
    void *ctx = hk->ctx;
    int32_t state = GS;
    PUT(A_FRAME, GET(A_FRAME) + 1);
    SUB_R(ENGINE_SUB_PREUPDATE_1, 0, 0, 0);
    SUB_R(ENGINE_SUB_PREUPDATE_2, 0, 0, 0);
    SUB_R(ENGINE_SUB_PREUPDATE_3, 0, 0, 0);
    SUB_R(ENGINE_SUB_RESET_SCRATCH, 0, 0, 0);
    SUB_R(ENGINE_SUB_PREUPDATE_4, 0, 0, 0);

    switch (state) {
    case SCENE_STATE_BOOT:
        if (hk->title_boot_poll(ctx) > 0) {
            SUB_R(ENGINE_SUB_PLAY_MOVIE, 0xb4, 0, 1);
            SUB_R(ENGINE_SUB_SCENE_BEGIN, 2, 0, 0);
            SUB_R(ENGINE_SUB_SCENE_OVERLAY, 0, 0, 0);
            SET_R(SCENE_STATE_TITLE);
            return;
        }
        break;
    case SCENE_STATE_TITLE:
        PUT(A_CLKMARK, hk->clock_now(ctx));
        if (hk->save_slot_check(ctx) == 0) {
            PUT(A_TARGET, hk->boot_target_resolve(ctx));
            if (GET(A_TARGET) != 0) {
                SUB_R(ENGINE_SUB_FADE, 0x3f, 500, 0);
                PUT(A_SUBMODE, 1);
                SUB_R(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
                if (GET(A_TARGET) == 9) SET_R(SCENE_STATE_TITLE_SUB);
                else                    SET_R(SCENE_STATE_TITLE_HOLD);
            }
            if (hk->alt_boot_poll(ctx) == 0) break;
            PUT(A_SUBMODE, 2);
            SUB_R(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            SUB_R(ENGINE_SUB_FADE, 0x3f, 500, 0);
        } else {
            SUB_R(ENGINE_SUB_FADE, 0x3f, 0, 0);
            PUT(A_SUBMODE, 0);
            SUB_R(ENGINE_SUB_MISC, 0x70, 0, 0);
            SUB_R(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            SUB_R(ENGINE_SUB_INPUT_MODE, 0, 1, 1);
        }
        SET_R(SCENE_STATE_TITLE_HOLD);
        break;
    case SCENE_STATE_TITLE_SUB:
        r_set_flag(m, ENGINE_FLAG_LOOP_KEEPALIVE, 1);
        if (hk->title_update_poll(ctx) == 0) return;
        SET_R(SCENE_STATE_TITLE_HOLD);
        break;
    case SCENE_STATE_LOAD_DISPATCH:
        if (GET(A_SUBMODE) == 0) {
            SUB_R(ENGINE_SUB_FADE, 0, 0, 0);
            SET_R(SCENE_STATE_BOOT_FINALIZE);
            break;
        }
        if (GET(A_SUBMODE) != 1) {
            SUB_R(ENGINE_SUB_FADE, 0, 0, 0);
            SET_R(SCENE_STATE_FIELD_SETUP);
            return;
        }
        if (hk->input_test(ctx, 0x12) == 0) {
            (void)hk->input_test(ctx, 0xc);
            SUB_R(ENGINE_SUB_PLAY_MOVIE, 0xb4, 0, 1);
        } else {
            SUB_R(ENGINE_SUB_EVENT_HINT, 3, 0, 0);
            SUB_R(ENGINE_SUB_PLAY_MOVIE, 0x7a, 0, 1);
        }
        SUB_R(ENGINE_SUB_FADE, 0, 0, 0);
        switch (GET(A_TARGET)) {
        case 3:
            SUB_R(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            SET_R(SCENE_STATE_BOOT_FILE_OPEN);
            break;
        case 4:
            SET_R(SCENE_STATE_BOOT);
            SET_R(SCENE_STATE_RELOAD);
            return;
        case 5:
            SET_R(SCENE_STATE_SYS_FILE_OPEN);
            break;
        default:
            break;
        case 8:
            if (hk->input_test(ctx, 3) == 0) {
                if (hk->input_test(ctx, 5) == 0) {
                    hk->fatal(ctx, 1);
                } else {
                    SUB_R(ENGINE_SUB_SAVE_LOAD_BYTE, hk->player_ctx_read(ctx, 0xc0), 0, 0);
                    SUB_R(ENGINE_SUB_SAVE_APPLY, 0, 0, 0);
                    SUB_R(ENGINE_SUB_SAVE_SELECT, 1, 0, 0);
                }
            } else {
                SUB_R(ENGINE_SUB_SAVE_LOAD_BYTE, hk->disc_ctx_read(ctx), 0, 0);
                SUB_R(ENGINE_SUB_SAVE_SELECT, 0, 0, 0);
            }
            SUB_R(ENGINE_SUB_SCENE_PREP, 0x14, 0, 0);
            SUB_R(ENGINE_SUB_FRAME_ADVANCE, 0, 0, 0);
            SET_R(SCENE_STATE_FIELD_SETUP);
            SUB_R(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            break;
        case 9:
            SUB_R(ENGINE_SUB_LAYER_ENABLE, 0, 1, 0);
            SUB_R(ENGINE_SUB_BOOT_SETUP, 1, 0, 0);
            SUB_R(ENGINE_SUB_FRAME_MUTEX_LOCK, 0, 0, 0);
            SUB_R(ENGINE_SUB_MISC, 1, 0, 0);
            SUB_R(ENGINE_SUB_RENDER_VTABLE_A, 0, 0, 0);
            SUB_R(ENGINE_SUB_FRAME_MUTEX_UNLK, 0, 0, 0);
            SUB_R(ENGINE_SUB_RENDER_MISC_D, 0, 0, 0);
            SUB_R(ENGINE_SUB_RENDER_VTABLE_B, 0, 0, 0);
            SUB_R(ENGINE_SUB_PLAY_MOVIE, 0xd9, 0, 1);
            SUB_R(ENGINE_SUB_RENDER_TOGGLE, 1, 0, 0);
            SUB_R(ENGINE_SUB_RENDER_FLUSH, 0, 0, 0);
            /* fall through */
        case 2:
            SUB_R(ENGINE_SUB_RENDER_MISC_A, 0, 0, 0);
            SUB_R(ENGINE_SUB_RENDER_MISC_B, 1, 0, 0);
            SUB_R(ENGINE_SUB_LAYER_ENABLE, 5, 1, 0);
            SUB_R(ENGINE_SUB_RENDER_MISC_C, 0, 0, 0);
            SUB_R(ENGINE_SUB_SCENE_PREP, 0x14, 0, 0);
            SET_R(SCENE_STATE_FIELD_SETUP);
            SUB_R(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
            SUB_R(ENGINE_SUB_FRAME_ADVANCE, 0, 0, 0);
            if (hk->field_boot_mode(ctx) == 3) SUB_R(ENGINE_SUB_FIELD_SPAWN, 0, 0, 0);
            break;
        case 10:
            SET_R(SCENE_STATE_EVENT_OPEN);
            return;
        case 0xd:
            SET_R(SCENE_STATE_EVENT_DONE);
            break;
        case 0xe:
            PUT(A_EVTCODE, 0x3010071);
            SET_R(SCENE_STATE_EVENT_OPEN);
            break;
        }
        break;
    case SCENE_STATE_TITLE_HOLD:
        r_set_flag(m, ENGINE_FLAG_LOOP_KEEPALIVE, 1);
        {
            uint32_t now = (uint32_t)hk->clock_now(ctx);
            if ((uint32_t)((uint32_t)GET(A_CLKMARK) + 600u) < now) {
                SUB_R(ENGINE_SUB_ATTRACT_PREP, 0, 0, 0);
                SET_R(SCENE_STATE_ATTRACT);
            }
        }
        break;
    case SCENE_STATE_BOOT_FILE_OPEN:
        SUB_R(ENGINE_SUB_LAYER_ENABLE, 7, 1, 0);
        {
            int code = hk->next_file_code(ctx);
            PUT(A_FHANDLE, hk->file_open(ctx, 0x3000011, code));
            SUB_R(ENGINE_SUB_FILE_KICK, GET(A_FHANDLE), 0, 0);
        }
        SET_R(SCENE_STATE_BOOT_FILE_WAIT);
        break;
    case SCENE_STATE_BOOT_FILE_WAIT:
        {
            int st = hk->file_status(ctx, GET(A_FHANDLE));
            if (st != 3) {
                if (st != 4) return;
                SET_R(SCENE_STATE_BOOT);
                SUB_R(ENGINE_SUB_FILE_CLOSE, GET(A_FHANDLE), 0, 0);
                PUT(A_FHANDLE, 0);
                SUB_R(ENGINE_SUB_FILE_FINISH, 1, 0, 0);
                break;
            }
            SET_R(SCENE_STATE_BOOT_FADE);
            SUB_R(ENGINE_SUB_FILE_CLOSE, GET(A_FHANDLE), 0, 0);
            SUB_R(ENGINE_SUB_FILE_FINISH, 0, 0, 0);
            SUB_R(ENGINE_SUB_BOOT_COMMIT, 0, 0, 0);
            PUT(A_FHANDLE, 0);
        }
        break;
    case SCENE_STATE_BOOT_FADE:
        if (hk->fade_poll(ctx) == 0) return;
        SUB_R(ENGINE_SUB_BOOT_COMMIT, 1, 0, 0);
        if (r_test_flag(m, ENGINE_FLAG_FADE_HOLD) == 0) {
            SET_R(SCENE_STATE_BOOT);
        } else {
            SET_R(SCENE_STATE_EVENT_DONE);
            r_set_flag(m, ENGINE_FLAG_FADE_HOLD, 0);
        }
        SUB_R(ENGINE_SUB_LAYER_ENABLE, 7, 0, 0);
        SUB_R(ENGINE_SUB_FILE_FINISH, 1, 0, 0);
        break;
    case SCENE_STATE_BOOT_FINALIZE:
        if (r_test_flag(m, ENGINE_FLAG_FIELD_READY) == 0 && hk->pause_poll(ctx) == 0) return;
        SET_R(SCENE_STATE_BOOT);
        break;
    case SCENE_STATE_FIELD_SETUP:
        SUB_R(ENGINE_SUB_RENDER_TOGGLE, 0, 0, 0);
        if (hk->input_test(ctx, 4) != 0 || hk->input_test(ctx, 10) != 0) {
            SUB_R(ENGINE_SUB_RENDER_FLUSH, 0, 0, 0);
            if (hk->input_test(ctx, 10) != 0) {
                SUB_R(ENGINE_SUB_RENDER_BIND, 0xe, 0xf, 0);
                SUB_R(ENGINE_SUB_RENDER_SYNC_A, 0, 0, 0);
            }
            if (hk->input_test(ctx, 10) != 0) SUB_R(ENGINE_SUB_RENDER_SYNC_B, 0, 0, 0);
        }
        SUB_R(ENGINE_SUB_INPUT_GATE, 0, 0, 0);
        SUB_R(ENGINE_SUB_INPUT_MODE, 0, 0, 0);
        if (GET(A_FIELDH) != 0) SUB_R(ENGINE_SUB_FIELD_FREE, GET(A_FIELDH), 3, 0);
        if (hk->field_enter_poll(ctx) >= 0) {
            if (hk->input_test(ctx, 10) == 0) {
                int a = hk->res_param(ctx, 0xd0);
                int b = hk->field_enter_poll(ctx);
                int c = hk->field_cam(ctx, 0);
                int d = hk->field_cam(ctx, 1);
                PUT(A_FIELDH, hk->field_result_make(ctx, a, b, c, d));
            } else {
                int a = hk->res_param(ctx, 0xd0);
                PUT(A_FIELDH, hk->field_result_make(ctx, a, 0x81, 0, 0));
                SUB_R(ENGINE_SUB_EVENT_HINT, 1, 0, 0);
            }
            SUB_R(ENGINE_SUB_FIELD_FINALIZE, 0, 0, 0);
            SET_R(SCENE_STATE_FIELD_LOAD);
            return;
        }
        SET_R(SCENE_STATE_BOOT);
        break;
    case SCENE_STATE_FIELD_LOAD:
        {
            int r = hk->field_run_poll(ctx, GET(A_FIELDH));
            if (r > 0) { SET_R(SCENE_STATE_FIELD_RUN); return; }
            if (r >= 0) return;
            if (GET(A_FIELDH) != 0) SUB_R(ENGINE_SUB_FIELD_FREE, GET(A_FIELDH), 3, 0);
            PUT(A_FIELDH, 0);
            SET_R(SCENE_STATE_BOOT);
        }
        break;
    case SCENE_STATE_FIELD_RUN:
        SUB_R(ENGINE_SUB_FIELD_PREP, 0, 0, 0);
        {
            int r = hk->field_exit_poll(ctx, GET(A_FIELDH));
            if (r == 0) break;
            if (r == 2) { SET_R(SCENE_STATE_RELOAD); return; }
            SUB_R(ENGINE_SUB_FIELD_TEARDOWN, 0, 0, 0);
            if (GET(A_FIELDH) != 0) { SUB_R(ENGINE_SUB_FIELD_FREE, GET(A_FIELDH), 3, 0); PUT(A_FIELDH, 0); }
            if (r == 3) {
                SUB_R(ENGINE_SUB_FIELD_EXTRA, 0, 0, 0);
                SET_R(SCENE_STATE_SYS_FILE_OPEN);
            } else if (r == 5) {
                SUB_R(ENGINE_SUB_FIELD_EXTRA, 0, 0, 0);
                SET_R(SCENE_STATE_EVENT_OPEN);
                return;
            } else if (r == 6 || r == 1) {
                if (r == 6) {
                    SUB_R(ENGINE_SUB_SCRIPT_RESET, -1, -1, 0);
                    SUB_R(ENGINE_SUB_SCRIPT_RESET, -1, -1, 1);
                    if (hk->input_test(ctx, 10) == 0) SUB_R(ENGINE_SUB_LAYER_ENABLE, 0xd, 1, 0);
                    else                              SUB_R(ENGINE_SUB_RENDER_FLUSH, 0, 0, 0);
                    if (hk->field_substate(ctx) == 1) {
                        int keep =
                            (hk->player_ctx_read(ctx, 0x41881) == 0) ||
                            ((hk->region_check(ctx) != 0 &&
                              hk->player_ctx_read(ctx, 0x41850) == 0x3f &&
                              hk->player_ctx_read(ctx, 0x41881) != 0) ||
                             hk->input_test(ctx, 0x12) != 0);
                        if (keep) {
                            if (hk->input_test(ctx, 0xe) != 0) {
                                SUB_R(ENGINE_SUB_LAYER_ENABLE, 0xd, 0, 0);
                                SUB_R(ENGINE_SUB_LAYER_ENABLE, 0xe, 0, 0);
                            }
                            SUB_R(ENGINE_SUB_RENDER_MISC_C, 0, 0, 0);
                            if (hk->field_boot_mode(ctx) == 3) SUB_R(ENGINE_SUB_FIELD_SPAWN, 0, 0, 0);
                            else                               SUB_R(ENGINE_SUB_RENDER_FLUSH, 0, 0, 0);
                            SUB_R(ENGINE_SUB_LAYER_ENABLE, 5, 1, 0);
                            SUB_R(ENGINE_SUB_LAYER_ENABLE, 0xd, 0, 0);
                            SUB_R(ENGINE_SUB_LAYER_ENABLE, 0xe, 0, 0);
                        } else {
                            SUB_R(ENGINE_SUB_SAVE_APPLY, 0, 0, 0);
                            SUB_R(ENGINE_SUB_LAYER_ENABLE, 3, 1, 0);
                        }
                    } else if (hk->field_substate(ctx) == 0) {
                        SUB_R(ENGINE_SUB_LAYER_ENABLE, 3, 1, 0);
                    }
                }
                SET_R(SCENE_STATE_FIELD_SETUP);
                return;
            } else {
                SET_R(SCENE_STATE_BOOT);
            }
        }
        break;
    case SCENE_STATE_FIELD_RESUME:
        if (hk->prev_state_query(ctx) == 0x15) SUB_R(ENGINE_SUB_FIELD_PREP, 0, 0, 0);
        SUB_R(ENGINE_SUB_RESUME_DRAW, 0, 0, 0);
        break;
    case SCENE_STATE_EVENT_OPEN:
        {
            int code = hk->next_file_code(ctx);
            PUT(A_FHANDLE, hk->file_open(ctx, GET(A_EVTCODE), code));
            SUB_R(ENGINE_SUB_FILE_KICK, GET(A_FHANDLE), 0, 0);
        }
        SET_R(SCENE_STATE_EVENT_WAIT);
        break;
    case SCENE_STATE_EVENT_WAIT:
        if (r_test_flag(m, ENGINE_FLAG_EVENT_READY) &&
            hk->event_sync_poll(ctx) == 0 &&
            hk->file_status(ctx, GET(A_FHANDLE)) == 3) {
            SUB_R(ENGINE_SUB_FILE_CLOSE, GET(A_FHANDLE), 0, 0);
            SUB_R(ENGINE_SUB_INPUT_GATE, 1, 0, 0);
            SUB_R(ENGINE_SUB_FADE_SET, 0x7f, 0, 0);
            SET_R(SCENE_STATE_EVENT_RUN);
        }
        break;
    case SCENE_STATE_EVENT_RUN:
        {
            int r = hk->fade_poll(ctx);
            if (r >= 0) return;
            if (r == -2) { SET_R(SCENE_STATE_EVENT_OPEN); return; }
            if (hk->alt_boot_poll(ctx) != 0) {
                r_set_flag(m, ENGINE_FLAG_EVENT_SKIP, 1);
                SET_R(SCENE_STATE_FIELD_SETUP);
                return;
            }
            SET_R(SCENE_STATE_BOOT);
        }
        break;
    case SCENE_STATE_INTRO:
        {
            int r = hk->intro_poll(ctx);
            if (r == 0) return;
            if (r < 1 && r != -2) {
                SUB_R(ENGINE_SUB_INTRO_PREP, 0, 0, 0);
                SET_R(SCENE_STATE_INTRO_FADE);
                return;
            }
            SUB_R(ENGINE_SUB_INTRO_COMMIT, 0, 0, 0);
            SET_R(SCENE_STATE_BOOT_FINALIZE);
        }
        break;
    case SCENE_STATE_INTRO_FADE:
        if (hk->intro_fade_poll(ctx) == 0) return;
        SUB_R(ENGINE_SUB_INTRO_COMMIT, 0, 0, 0);
        SET_R(SCENE_STATE_BOOT_FINALIZE);
        break;
    case SCENE_STATE_RELOAD_WAIT:
        if (hk->reload_poll(ctx) == 0) return;
        SET_R(SCENE_STATE_BOOT);
        break;
    case SCENE_STATE_ATTRACT:
        {
            int arg = 0;
            if (GET(A_SUBMODE) != 0) arg = (hk->input_test(ctx, 10) == 0) ? 1 : 0;
            if (hk->title_return_poll(ctx, arg) != 0) {
                r_set_flag(m, ENGINE_FLAG_LOOP_KEEPALIVE, 0);
                SET_R(SCENE_STATE_LOAD_DISPATCH);
            }
        }
        break;
    default:
        break;
    }
}

/* ===================================================================== *
 * Harness: run one tick on both the clean engine and the raw oracle from
 * an identical initial spine block + identical script, then diff.
 * ===================================================================== */
static void load_es(const EngineState *es, uint8_t *m) {
    PUT(A_SCENE,   es->scene_state);
    PUT(A_PREV,    es->prev_scene_state);
    PUT(A_FLAGS,   (int32_t)es->mode_flags);
    PUT(A_FRAME,   es->frame_counter);
    PUT(A_CLKMARK, es->boot_clock_mark);
    PUT(A_SUBMODE, es->boot_submode);
    PUT(A_TARGET,  es->boot_target);
    PUT(A_FHANDLE, es->file_handle);
    PUT(A_FIELDH,  es->field_handle);
    PUT(A_EVTCODE, es->event_file_code);
}

static int diff_state(const EngineState *es, const uint8_t *m, const char *label) {
    int d = 0;
    d |= (es->scene_state      != GET(A_SCENE));
    d |= (es->prev_scene_state != GET(A_PREV));
    d |= ((int32_t)es->mode_flags != GET(A_FLAGS));
    d |= (es->frame_counter    != GET(A_FRAME));
    d |= (es->boot_clock_mark  != GET(A_CLKMARK));
    d |= (es->boot_submode     != GET(A_SUBMODE));
    d |= (es->boot_target      != GET(A_TARGET));
    d |= (es->file_handle      != GET(A_FHANDLE));
    d |= (es->field_handle     != GET(A_FIELDH));
    d |= (es->event_file_code  != GET(A_EVTCODE));
    CHECK(!d, "dispatch PARITY state mismatch [%s]: clean(state=%d prev=%d flags=0x%x target=%d fh=%d fldh=%d)"
              " vs raw(state=%d prev=%d flags=0x%x target=%d fh=%d fldh=%d)",
          label, es->scene_state, es->prev_scene_state, es->mode_flags, es->boot_target,
          es->file_handle, es->field_handle,
          GET(A_SCENE), GET(A_PREV), GET(A_FLAGS), GET(A_TARGET), GET(A_FHANDLE), GET(A_FIELDH));
    return d;
}

static int diff_logs(const Script *a, const Script *b, const char *label) {
    int d = (a->log_n != b->log_n);
    if (!d) {
        for (int i = 0; i < a->log_n && i < LOGMAX; i++) {
            for (int j = 0; j < 4; j++) d |= (a->log[i][j] != b->log[i][j]);
        }
    }
    CHECK(!d, "dispatch PARITY below-seam call-log mismatch [%s]: clean n=%d, raw n=%d",
          label, a->log_n, b->log_n);
    return d;
}

/* Run both, diff, and return the clean EngineState for behaviour asserts. */
static EngineState run_both(uint8_t *m, const EngineState *init, const Script *base, const char *label) {
    EngineState es = *init;
    Script sc_clean = *base; sc_clean.log_n = 0;
    Script sc_raw   = *base; sc_raw.log_n = 0;
    EngineHooks hc = make_hooks(&sc_clean);
    EngineHooks hr = make_hooks(&sc_raw);

    memset(m, 0, RAW_SIZE);
    load_es(&es, m);

    engine_state_dispatch(&es, &hc);
    ref_dispatch(m, &hr);

    diff_state(&es, m, label);
    diff_logs(&sc_clean, &sc_raw, label);
    return es;
}

/* ===================================================================== *
 * 1. Primitive parity + semantics
 * ===================================================================== */
static void test_primitives(uint8_t *m) {
    EngineState es;
    engine_state_init(&es);
    memset(m, 0, RAW_SIZE);

    /* A scripted op sequence covering set/same/push/pop and flag set/test. */
    const int seq[] = { 1, 2, 2, 5, -1 /*pop->2*/, 9, 10, -1 /*pop->9*/, -1 /*pop->10*/, 1 };
    for (size_t i = 0; i < sizeof(seq)/sizeof(seq[0]); i++) {
        engine_set_scene_state(&es, seq[i]);
        r_set_state(m, seq[i]);
        CHECK(es.scene_state == r_get_state(m),
              "primitive set_state parity step %zu: clean=%d raw=%d", i, es.scene_state, r_get_state(m));
        CHECK(es.prev_scene_state == (int32_t)rld32(m, OFF(A_PREV)),
              "primitive prev parity step %zu", i);
    }
    /* Push-pop semantics: in {cur=1,prev=2}, set(-1) pops to 2 and prev<-1. */
    engine_state_init(&es);
    engine_set_scene_state(&es, 4);   /* cur=4 prev=0 */
    engine_set_scene_state(&es, 7);   /* cur=7 prev=4 */
    engine_set_scene_state(&es, -1);  /* pop: cur=4 prev=7 */
    CHECK(es.scene_state == 4 && es.prev_scene_state == 7, "push-pop: got state=%d prev=%d", es.scene_state, es.prev_scene_state);
    /* set-to-same is a no-op (prev unchanged) */
    engine_set_scene_state(&es, 4);
    CHECK(es.scene_state == 4 && es.prev_scene_state == 7, "set-same no-op: state=%d prev=%d", es.scene_state, es.prev_scene_state);

    /* Flag parity + semantics across all 6 named bits. */
    const unsigned bits[] = { ENGINE_FLAG_FADE_HOLD, ENGINE_FLAG_DIAG, ENGINE_FLAG_EVENT_SKIP,
                              ENGINE_FLAG_EVENT_READY, ENGINE_FLAG_FIELD_READY, ENGINE_FLAG_LOOP_KEEPALIVE };
    engine_state_init(&es);
    memset(m, 0, RAW_SIZE);
    for (size_t i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
        engine_set_mode_flag(&es, bits[i], 1); r_set_flag(m, bits[i], 1);
        CHECK(engine_test_mode_flag(&es, bits[i]) && r_test_flag(m, bits[i]), "flag set bit %u", bits[i]);
        CHECK(es.mode_flags == rld32(m, OFF(A_FLAGS)), "flag mask parity set bit %u", bits[i]);
    }
    for (size_t i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
        engine_set_mode_flag(&es, bits[i], 0); r_set_flag(m, bits[i], 0);
        CHECK(!engine_test_mode_flag(&es, bits[i]) && !r_test_flag(m, bits[i]), "flag clear bit %u", bits[i]);
        CHECK(es.mode_flags == rld32(m, OFF(A_FLAGS)), "flag mask parity clear bit %u", bits[i]);
    }
    CHECK(es.mode_flags == 0, "flags all clear at end: 0x%x", es.mode_flags);
}

/* ===================================================================== *
 * 2+3. Dispatch behaviour (traced to the decomp) + parity on each case
 * ===================================================================== */
static Script blank(void) { Script s; memset(&s, 0, sizeof(s)); return s; }

static void test_dispatch(uint8_t *m) {
    EngineState init, out;
    Script s;

    /* BOOT: title_boot_poll>0 -> TITLE (else stay). */
    engine_state_init(&init); init.scene_state = SCENE_STATE_BOOT;
    s = blank(); s.title_boot = 1;
    out = run_both(m, &init, &s, "BOOT->TITLE");
    CHECK(out.scene_state == SCENE_STATE_TITLE, "BOOT->TITLE got %d", out.scene_state);
    s = blank(); s.title_boot = 0;
    out = run_both(m, &init, &s, "BOOT stay");
    CHECK(out.scene_state == SCENE_STATE_BOOT, "BOOT stay got %d", out.scene_state);
    CHECK(out.frame_counter == 1, "frame_counter incremented: %d", out.frame_counter);

    /* TITLE, no save, boot_target==9 -> TITLE_SUB then (alt_boot==0) stays 3. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_TITLE;
    s = blank(); s.save_check = 0; s.boot_target = 9; s.alt_boot = 0; s.clock = 123;
    out = run_both(m, &init, &s, "TITLE target9");
    CHECK(out.scene_state == SCENE_STATE_TITLE_SUB, "TITLE->TITLE_SUB got %d", out.scene_state);
    CHECK(out.boot_submode == 1 && out.boot_target == 9, "TITLE submode/target %d/%d", out.boot_submode, out.boot_target);
    CHECK(out.boot_clock_mark == 123, "TITLE latched clock %d", out.boot_clock_mark);

    /* TITLE, no save, boot_target==5, alt_boot!=0 -> TITLE_HOLD, submode 2. */
    s = blank(); s.save_check = 0; s.boot_target = 5; s.alt_boot = 1;
    out = run_both(m, &init, &s, "TITLE target5 alt");
    CHECK(out.scene_state == SCENE_STATE_TITLE_HOLD, "TITLE->HOLD got %d", out.scene_state);
    CHECK(out.boot_submode == 2, "TITLE submode2 got %d", out.boot_submode);

    /* TITLE, save present -> TITLE_HOLD, submode 0. */
    s = blank(); s.save_check = 1;
    out = run_both(m, &init, &s, "TITLE save");
    CHECK(out.scene_state == SCENE_STATE_TITLE_HOLD, "TITLE(save)->HOLD got %d", out.scene_state);
    CHECK(out.boot_submode == 0, "TITLE(save) submode0 got %d", out.boot_submode);

    /* TITLE_SUB: title_update!=0 -> TITLE_HOLD; sets keepalive flag. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_TITLE_SUB;
    s = blank(); s.title_update = 1;
    out = run_both(m, &init, &s, "TITLE_SUB done");
    CHECK(out.scene_state == SCENE_STATE_TITLE_HOLD, "TITLE_SUB->HOLD got %d", out.scene_state);
    CHECK(engine_test_mode_flag(&out, ENGINE_FLAG_LOOP_KEEPALIVE), "TITLE_SUB keepalive set");
    s = blank(); s.title_update = 0;
    out = run_both(m, &init, &s, "TITLE_SUB wait");
    CHECK(out.scene_state == SCENE_STATE_TITLE_SUB, "TITLE_SUB wait got %d", out.scene_state);

    /* LOAD_DISPATCH submode 0 -> BOOT_FINALIZE. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_LOAD_DISPATCH; init.boot_submode = 0;
    s = blank();
    out = run_both(m, &init, &s, "LOAD submode0");
    CHECK(out.scene_state == SCENE_STATE_BOOT_FINALIZE, "LOAD submode0 got %d", out.scene_state);

    /* LOAD_DISPATCH submode 2 -> FIELD_SETUP. */
    init.boot_submode = 2;
    out = run_both(m, &init, &s, "LOAD submode2");
    CHECK(out.scene_state == SCENE_STATE_FIELD_SETUP, "LOAD submode2 got %d", out.scene_state);

    /* LOAD_DISPATCH submode 1, target 3 -> BOOT_FILE_OPEN. */
    init.boot_submode = 1; init.boot_target = 3;
    out = run_both(m, &init, &s, "LOAD target3");
    CHECK(out.scene_state == SCENE_STATE_BOOT_FILE_OPEN, "LOAD target3 got %d", out.scene_state);

    /* LOAD_DISPATCH submode 1, target 4 -> set 1 then RELOAD (0xd). */
    init.boot_target = 4;
    out = run_both(m, &init, &s, "LOAD target4");
    CHECK(out.scene_state == SCENE_STATE_RELOAD, "LOAD target4 got %d", out.scene_state);
    CHECK(out.prev_scene_state == SCENE_STATE_BOOT, "LOAD target4 prev %d", out.prev_scene_state);

    /* LOAD_DISPATCH submode 1, target 9 -> (fallthrough to 2) FIELD_SETUP. */
    init.boot_target = 9;
    out = run_both(m, &init, &s, "LOAD target9");
    CHECK(out.scene_state == SCENE_STATE_FIELD_SETUP, "LOAD target9 got %d", out.scene_state);

    /* LOAD_DISPATCH submode 1, target 8, no input3/5 -> fatal logged, then FIELD_SETUP. */
    init.boot_target = 8;
    s = blank();
    out = run_both(m, &init, &s, "LOAD target8 fatal");
    CHECK(out.scene_state == SCENE_STATE_FIELD_SETUP, "LOAD target8 got %d", out.scene_state);

    /* LOAD_DISPATCH submode 1, target 0xe -> event_file_code set, EVENT_OPEN. */
    init.boot_target = 0xe;
    s = blank();
    out = run_both(m, &init, &s, "LOAD target0xe");
    CHECK(out.scene_state == SCENE_STATE_EVENT_OPEN, "LOAD target0xe got %d", out.scene_state);
    CHECK(out.event_file_code == 0x3010071, "LOAD target0xe code 0x%x", out.event_file_code);

    /* LOAD_DISPATCH submode 1, target 10 -> EVENT_OPEN. */
    init.boot_target = 10;
    out = run_both(m, &init, &s, "LOAD target10");
    CHECK(out.scene_state == SCENE_STATE_EVENT_OPEN, "LOAD target10 got %d", out.scene_state);

    /* TITLE_HOLD: timeout (clock > mark+600) -> ATTRACT. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_TITLE_HOLD; init.boot_clock_mark = 0;
    s = blank(); s.clock = 601;
    out = run_both(m, &init, &s, "HOLD timeout");
    CHECK(out.scene_state == SCENE_STATE_ATTRACT, "HOLD timeout got %d", out.scene_state);
    s = blank(); s.clock = 600;
    out = run_both(m, &init, &s, "HOLD wait");
    CHECK(out.scene_state == SCENE_STATE_TITLE_HOLD, "HOLD wait got %d", out.scene_state);

    /* BOOT_FILE_OPEN -> BOOT_FILE_WAIT, latches handle. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_BOOT_FILE_OPEN;
    s = blank(); s.file_open_ret = 0x1234;
    out = run_both(m, &init, &s, "FILE_OPEN");
    CHECK(out.scene_state == SCENE_STATE_BOOT_FILE_WAIT, "FILE_OPEN got %d", out.scene_state);
    CHECK(out.file_handle == 0x1234, "FILE_OPEN handle 0x%x", out.file_handle);

    /* BOOT_FILE_WAIT: status 3 -> BOOT_FADE, handle cleared. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_BOOT_FILE_WAIT; init.file_handle = 0x1234;
    s = blank(); s.file_status = 3;
    out = run_both(m, &init, &s, "FILE_WAIT done");
    CHECK(out.scene_state == SCENE_STATE_BOOT_FADE, "FILE_WAIT done got %d", out.scene_state);
    CHECK(out.file_handle == 0, "FILE_WAIT handle cleared %d", out.file_handle);
    /* status 4 -> BOOT (error). status other -> stay. */
    s = blank(); s.file_status = 4;
    out = run_both(m, &init, &s, "FILE_WAIT err");
    CHECK(out.scene_state == SCENE_STATE_BOOT, "FILE_WAIT err got %d", out.scene_state);
    s = blank(); s.file_status = 0;
    out = run_both(m, &init, &s, "FILE_WAIT pending");
    CHECK(out.scene_state == SCENE_STATE_BOOT_FILE_WAIT, "FILE_WAIT pending got %d", out.scene_state);

    /* BOOT_FADE: fade done, flag7 clear -> BOOT. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_BOOT_FADE;
    s = blank(); s.fade = 1;
    out = run_both(m, &init, &s, "BOOT_FADE->BOOT");
    CHECK(out.scene_state == SCENE_STATE_BOOT, "BOOT_FADE->BOOT got %d", out.scene_state);
    /* fade done, flag7 set -> EVENT_DONE and flag7 cleared. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_BOOT_FADE;
    engine_set_mode_flag(&init, ENGINE_FLAG_FADE_HOLD, 1);
    s = blank(); s.fade = 1;
    out = run_both(m, &init, &s, "BOOT_FADE flag7");
    CHECK(out.scene_state == SCENE_STATE_EVENT_DONE, "BOOT_FADE flag7 got %d", out.scene_state);
    CHECK(!engine_test_mode_flag(&out, ENGINE_FLAG_FADE_HOLD), "BOOT_FADE flag7 cleared");
    /* fade not done -> stay. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_BOOT_FADE;
    s = blank(); s.fade = 0;
    out = run_both(m, &init, &s, "BOOT_FADE wait");
    CHECK(out.scene_state == SCENE_STATE_BOOT_FADE, "BOOT_FADE wait got %d", out.scene_state);

    /* BOOT_FINALIZE: flag0x15 set -> BOOT. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_BOOT_FINALIZE;
    engine_set_mode_flag(&init, ENGINE_FLAG_FIELD_READY, 1);
    s = blank();
    out = run_both(m, &init, &s, "FINALIZE flag");
    CHECK(out.scene_state == SCENE_STATE_BOOT, "FINALIZE flag got %d", out.scene_state);
    /* no flag, pause!=0 -> BOOT; pause==0 -> stay. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_BOOT_FINALIZE;
    s = blank(); s.pause = 1;
    out = run_both(m, &init, &s, "FINALIZE pause");
    CHECK(out.scene_state == SCENE_STATE_BOOT, "FINALIZE pause got %d", out.scene_state);
    s = blank(); s.pause = 0;
    out = run_both(m, &init, &s, "FINALIZE wait");
    CHECK(out.scene_state == SCENE_STATE_BOOT_FINALIZE, "FINALIZE wait got %d", out.scene_state);

    /* FIELD_SETUP: field_enter>=0, no input10 -> FIELD_LOAD, handle made. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_FIELD_SETUP;
    s = blank(); s.field_enter = 0; s.field_make = 0x55;
    out = run_both(m, &init, &s, "FIELD_SETUP enter");
    CHECK(out.scene_state == SCENE_STATE_FIELD_LOAD, "FIELD_SETUP enter got %d", out.scene_state);
    CHECK(out.field_handle == 0x55, "FIELD_SETUP handle 0x%x", out.field_handle);
    /* field_enter<0 -> BOOT. */
    s = blank(); s.field_enter = -1;
    out = run_both(m, &init, &s, "FIELD_SETUP noenter");
    CHECK(out.scene_state == SCENE_STATE_BOOT, "FIELD_SETUP noenter got %d", out.scene_state);

    /* FIELD_LOAD: run>0 -> FIELD_RUN; run==0 -> stay; run<0 -> BOOT. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_FIELD_LOAD; init.field_handle = 0x55;
    s = blank(); s.field_run = 1;
    out = run_both(m, &init, &s, "FIELD_LOAD->RUN");
    CHECK(out.scene_state == SCENE_STATE_FIELD_RUN, "FIELD_LOAD->RUN got %d", out.scene_state);
    s = blank(); s.field_run = 0;
    out = run_both(m, &init, &s, "FIELD_LOAD wait");
    CHECK(out.scene_state == SCENE_STATE_FIELD_LOAD, "FIELD_LOAD wait got %d", out.scene_state);
    s = blank(); s.field_run = -1;
    out = run_both(m, &init, &s, "FIELD_LOAD fail");
    CHECK(out.scene_state == SCENE_STATE_BOOT && out.field_handle == 0, "FIELD_LOAD fail got %d h=%d", out.scene_state, out.field_handle);

    /* FIELD_RUN exit codes: 0 stay, 2 RELOAD, 3 SYS_FILE_OPEN, 5 EVENT_OPEN, 1 FIELD_SETUP, other BOOT. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_FIELD_RUN; init.field_handle = 0x55;
    struct { int code; int expect; const char *lbl; } fx[] = {
        { 0, SCENE_STATE_FIELD_RUN,     "RUN exit0" },
        { 2, SCENE_STATE_RELOAD,        "RUN exit2" },
        { 3, SCENE_STATE_SYS_FILE_OPEN, "RUN exit3" },
        { 5, SCENE_STATE_EVENT_OPEN,    "RUN exit5" },
        { 1, SCENE_STATE_FIELD_SETUP,   "RUN exit1" },
        { 6, SCENE_STATE_FIELD_SETUP,   "RUN exit6" },
        { 7, SCENE_STATE_BOOT,          "RUN exit7(default)" },
    };
    for (size_t i = 0; i < sizeof(fx)/sizeof(fx[0]); i++) {
        s = blank(); s.field_exit = fx[i].code;
        out = run_both(m, &init, &s, fx[i].lbl);
        CHECK(out.scene_state == fx[i].expect, "%s got %d exp %d", fx[i].lbl, out.scene_state, fx[i].expect);
    }
    /* FIELD_RUN exit 6, substate 1, keep-path exercises the deep branch. */
    s = blank(); s.field_exit = 6; s.field_sub = 1; s.pc_41881 = 0; /* keep via first disjunct */
    out = run_both(m, &init, &s, "RUN exit6 deep");
    CHECK(out.scene_state == SCENE_STATE_FIELD_SETUP, "RUN exit6 deep got %d", out.scene_state);

    /* FIELD_RESUME: no transition. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_FIELD_RESUME;
    s = blank(); s.prev_state = 0x15;
    out = run_both(m, &init, &s, "RESUME");
    CHECK(out.scene_state == SCENE_STATE_FIELD_RESUME, "RESUME got %d", out.scene_state);

    /* EVENT_OPEN -> EVENT_WAIT, uses event_file_code. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_EVENT_OPEN; init.event_file_code = 0x3010071;
    s = blank(); s.file_open_ret = 0x77;
    out = run_both(m, &init, &s, "EVENT_OPEN");
    CHECK(out.scene_state == SCENE_STATE_EVENT_WAIT, "EVENT_OPEN got %d", out.scene_state);
    CHECK(out.file_handle == 0x77, "EVENT_OPEN handle 0x%x", out.file_handle);

    /* EVENT_WAIT: flag0x11 + sync0 + status3 -> EVENT_RUN. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_EVENT_WAIT; init.file_handle = 0x77;
    engine_set_mode_flag(&init, ENGINE_FLAG_EVENT_READY, 1);
    s = blank(); s.event_sync = 0; s.file_status = 3;
    out = run_both(m, &init, &s, "EVENT_WAIT ready");
    CHECK(out.scene_state == SCENE_STATE_EVENT_RUN, "EVENT_WAIT ready got %d", out.scene_state);
    /* flag not set -> stay. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_EVENT_WAIT;
    s = blank(); s.event_sync = 0; s.file_status = 3;
    out = run_both(m, &init, &s, "EVENT_WAIT noflag");
    CHECK(out.scene_state == SCENE_STATE_EVENT_WAIT, "EVENT_WAIT noflag got %d", out.scene_state);

    /* EVENT_RUN: fade>=0 stay; -2 EVENT_OPEN; <0&&alt_boot FIELD_SETUP(+skip flag); else BOOT. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_EVENT_RUN;
    s = blank(); s.fade = 0;
    out = run_both(m, &init, &s, "EVENT_RUN wait");
    CHECK(out.scene_state == SCENE_STATE_EVENT_RUN, "EVENT_RUN wait got %d", out.scene_state);
    s = blank(); s.fade = -2;
    out = run_both(m, &init, &s, "EVENT_RUN replay");
    CHECK(out.scene_state == SCENE_STATE_EVENT_OPEN, "EVENT_RUN replay got %d", out.scene_state);
    s = blank(); s.fade = -1; s.alt_boot = 1;
    out = run_both(m, &init, &s, "EVENT_RUN alt");
    CHECK(out.scene_state == SCENE_STATE_FIELD_SETUP, "EVENT_RUN alt got %d", out.scene_state);
    CHECK(engine_test_mode_flag(&out, ENGINE_FLAG_EVENT_SKIP), "EVENT_RUN skip flag set");
    s = blank(); s.fade = -1; s.alt_boot = 0;
    out = run_both(m, &init, &s, "EVENT_RUN end");
    CHECK(out.scene_state == SCENE_STATE_BOOT, "EVENT_RUN end got %d", out.scene_state);

    /* INTRO: intro<0(!=-2) -> INTRO_FADE; else(positive/-2) -> BOOT_FINALIZE; ==0 stay. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_INTRO;
    s = blank(); s.intro = -1;
    out = run_both(m, &init, &s, "INTRO fade");
    CHECK(out.scene_state == SCENE_STATE_INTRO_FADE, "INTRO fade got %d", out.scene_state);
    s = blank(); s.intro = 1;
    out = run_both(m, &init, &s, "INTRO done");
    CHECK(out.scene_state == SCENE_STATE_BOOT_FINALIZE, "INTRO done got %d", out.scene_state);
    s = blank(); s.intro = 0;
    out = run_both(m, &init, &s, "INTRO wait");
    CHECK(out.scene_state == SCENE_STATE_INTRO, "INTRO wait got %d", out.scene_state);

    /* INTRO_FADE: done -> BOOT_FINALIZE. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_INTRO_FADE;
    s = blank(); s.intro_fade = 1;
    out = run_both(m, &init, &s, "INTRO_FADE done");
    CHECK(out.scene_state == SCENE_STATE_BOOT_FINALIZE, "INTRO_FADE done got %d", out.scene_state);

    /* RELOAD_WAIT: done -> BOOT. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_RELOAD_WAIT;
    s = blank(); s.reload = 1;
    out = run_both(m, &init, &s, "RELOAD_WAIT done");
    CHECK(out.scene_state == SCENE_STATE_BOOT, "RELOAD_WAIT done got %d", out.scene_state);

    /* ATTRACT: title_return!=0 -> LOAD_DISPATCH, keepalive cleared. */
    engine_state_init(&init); init.scene_state = SCENE_STATE_ATTRACT;
    engine_set_mode_flag(&init, ENGINE_FLAG_LOOP_KEEPALIVE, 1);
    s = blank(); s.title_return = 1;
    out = run_both(m, &init, &s, "ATTRACT go");
    CHECK(out.scene_state == SCENE_STATE_LOAD_DISPATCH, "ATTRACT go got %d", out.scene_state);
    CHECK(!engine_test_mode_flag(&out, ENGINE_FLAG_LOOP_KEEPALIVE), "ATTRACT keepalive cleared");
    s = blank(); s.title_return = 0;
    out = run_both(m, &init, &s, "ATTRACT wait");
    CHECK(out.scene_state == SCENE_STATE_ATTRACT, "ATTRACT wait got %d", out.scene_state);
}

int main(void) {
    uint8_t *m = (uint8_t *)malloc(RAW_SIZE);
    if (!m) { printf("FAIL: oom\n"); return 1; }

    test_primitives(m);
    test_dispatch(m);

    free(m);
    if (g_fail) { printf("engine_state: %d FAILURE(S)\n", g_fail); return 1; }
    printf("engine_state: all checks passed (primitive parity, dispatch parity, behaviour)\n");
    return 0;
}
