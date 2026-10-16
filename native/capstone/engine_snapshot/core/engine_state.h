/* =====================================================================
 * engine_state.h  --  Reauthored ENGINE SPINE: per-frame top-level state
 * machine + mode-flag bitmask (purist port, P4).
 *
 * This is the highly-portable control layer that drives every subsystem
 * each frame. engine-map.md §1 ("boot -> per-frame loop", agent: spine):
 *
 *   PER-FRAME LOOP = FUN_00248110 calls, in order:
 *     ... frame-mutex lock, RENDER open, INPUT poll, subsystem updates ...
 *     FUN_00247508   <-- STATE DISPATCH (switch on scene state)  == this
 *     ... per-frame hook, RENDER kick, mutex unlock, VBLANK+flip ...
 *
 *   STATE MACHINE:
 *     scene state int DAT_004b8b2c (prev DAT_004b8b30);
 *       set FUN_00241d38(n)  (n<0 = pop to previous),
 *       get FUN_00241d68, dispatched by FUN_00247508.
 *     mode-flag bitmask DAT_004b8980;
 *       set FUN_00241378(bit,on), test FUN_002413c0(bit).
 *
 * Reauthored from (analysis/decomp/, read-only oracle):
 *   FUN_00247508  per-frame state dispatch  -> engine_state_dispatch
 *   FUN_00241d38  set scene-state (n<0=pop) -> engine_set_scene_state
 *   FUN_00241d68  get scene-state           -> engine_get_scene_state
 *   FUN_00241378  set mode-flag bit         -> engine_set_mode_flag
 *   FUN_002413c0  test mode-flag bit        -> engine_test_mode_flag
 *
 * The dispatch's PURE STATE LOGIC (which state transitions to which next
 * state under which conditions, and which mode-flag bits it sets/clears)
 * is reauthored verbatim. Every call BELOW THE SEAM (rendering, file
 * load, input, audio, scripting, ...) is an explicit, documented hook in
 * EngineHooks; the engine never no-ops silently.
 *
 * Guest addresses are recorded against the module state and against every
 * state/flag so the map stays auditable and a byte-accurate transcription
 * can be diffed against it (tests/test_engine_state.c).
 * ===================================================================== */
#ifndef DOC_ENGINE_STATE_H
#define DOC_ENGINE_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- *
 * Scene states (values of DAT_004b8b2c, dispatched by FUN_00247508).
 *
 * Numeric values are from the switch in the decomp and are LOAD-BEARING
 * (they are stored in the guest state var and compared by other overlays).
 * Names are inferred from behaviour and labelled CONSERVATIVELY: the boot/
 * title/load/field/event spine is well traced; a few targets are only ever
 * *set* here and serviced by a chained-ELF overlay, flagged "target-only".
 * --------------------------------------------------------------------- */
typedef enum EngineSceneState {
    SCENE_STATE_BOOT          = 0x01, /* case 1: wait title_boot_poll>0, then -> TITLE */
    SCENE_STATE_TITLE         = 0x02, /* case 2: latch clock, save/boot-target resolve, route 3/5 */
    SCENE_STATE_TITLE_SUB     = 0x03, /* case 3: keepalive; wait title_update_poll; -> TITLE_HOLD
                                       *         (entered from TITLE when boot_target==9)        */
    SCENE_STATE_LOAD_DISPATCH = 0x04, /* case 4: hub; switch on boot_target -> field/event/... */
    SCENE_STATE_TITLE_HOLD    = 0x05, /* case 5: keepalive; 600-tick timeout -> ATTRACT         */
    SCENE_STATE_SYS_FILE_OPEN = 0x08, /* target-only (set from LOAD_DISPATCH, boot_target==5)   */
    SCENE_STATE_BOOT_FILE_OPEN= 0x09, /* case 9: open file 0x3000011 -> BOOT_FILE_WAIT          */
    SCENE_STATE_BOOT_FILE_WAIT= 0x0a, /* case 10: poll handle status (3=done/4=err)             */
    SCENE_STATE_BOOT_FADE     = 0x0b, /* case 0xb: fade out; -> BOOT (or EVENT_DONE if flag 7)  */
    SCENE_STATE_RELOAD        = 0x0d, /* target-only (set from LOAD_DISPATCH mode4 / FIELD_RUN) */
    SCENE_STATE_BOOT_FINALIZE = 0x10, /* case 0x10: wait flag 0x15 / pause_poll; -> BOOT        */
    SCENE_STATE_FIELD_SETUP   = 0x13, /* case 0x13: build field; make field_handle -> FIELD_LOAD*/
    SCENE_STATE_FIELD_LOAD    = 0x14, /* case 0x14: field_run_poll progress -> FIELD_RUN         */
    SCENE_STATE_FIELD_RUN     = 0x15, /* case 0x15: field_exit_poll -> many exits                */
    SCENE_STATE_FIELD_RESUME  = 0x17, /* case 0x17: resume (prev==FIELD_RUN); no transition      */
    SCENE_STATE_EVENT_OPEN    = 0x18, /* case 0x18: open event_file_code -> EVENT_WAIT           */
    SCENE_STATE_EVENT_WAIT    = 0x19, /* case 0x19: flag 0x11 + sync + handle done -> EVENT_RUN  */
    SCENE_STATE_EVENT_RUN     = 0x1a, /* case 0x1a: fade_poll -> EVENT_OPEN/FIELD_SETUP/BOOT     */
    SCENE_STATE_EVENT_DONE    = 0x1c, /* target-only (set from BOOT_FADE / FIELD_RUN)            */
    SCENE_STATE_INTRO         = 0x1d, /* case 0x1d: intro_poll -> INTRO_FADE / BOOT_FINALIZE     */
    SCENE_STATE_INTRO_FADE    = 0x1e, /* case 0x1e: intro_fade_poll -> BOOT_FINALIZE             */
    SCENE_STATE_RELOAD_WAIT   = 0x1f, /* case 0x1f: reload_poll -> BOOT                          */
    SCENE_STATE_ATTRACT       = 0x20  /* case 0x20: title_return_poll -> LOAD_DISPATCH           */
} EngineSceneState;

/* --------------------------------------------------------------------- *
 * Mode-flag bits (bit indices into DAT_004b8980).
 * Only the bits this dispatch touches are named; the mask itself holds up
 * to 32 bits (set/clear/test take a raw bit index 0..31, see primitives).
 * Names for the game-logic bits are CONSERVATIVE inferences from use site.
 * --------------------------------------------------------------------- */
typedef enum EngineModeFlag {
    ENGINE_FLAG_FADE_HOLD      = 0x07, /* tested+cleared in BOOT_FADE (case 0xb)                */
    ENGINE_FLAG_DIAG           = 0x09, /* engine-map.md: "bit9 diagnostic loop" (not set here)  */
    ENGINE_FLAG_EVENT_SKIP     = 0x0c, /* set in EVENT_RUN (case 0x1a)                          */
    ENGINE_FLAG_EVENT_READY    = 0x11, /* tested in EVENT_WAIT (case 0x19)                      */
    ENGINE_FLAG_FIELD_READY    = 0x15, /* tested in BOOT_FINALIZE (case 0x10)                   */
    ENGINE_FLAG_LOOP_KEEPALIVE = 0x1a  /* engine-map.md: "0x1a loop-keepalive"; set in 3/5,     */
                                       /* cleared in ATTRACT (case 0x20)                        */
} EngineModeFlag;

/* --------------------------------------------------------------------- *
 * Module state: the named replacement for the spine DAT_ globals.
 *
 * The three CORE globals are the state machine proper; the rest are the
 * dispatch's own cross-frame working variables (boot routing, latched
 * clock, active handles). Guest address against each so the map is
 * auditable (and diffable against the raw transcription).
 * --------------------------------------------------------------------- */
typedef struct EngineState {
    int32_t  scene_state;       /* <- DAT_004b8b2c : current scene state       */
    int32_t  prev_scene_state;  /* <- DAT_004b8b30 : previous (back-stack of 1) */
    uint32_t mode_flags;        /* <- DAT_004b8980 : mode-flag bitmask          */

    int32_t  frame_counter;     /* <- DAT_004bcd84 : ++ each dispatch           */
    int32_t  boot_clock_mark;   /* <- DAT_004bcbcc : clock latched in TITLE     */
    int32_t  boot_submode;      /* <- DAT_004bcbc8 : 0/1/2 boot sub-path        */
    int32_t  boot_target;       /* <- DAT_0046b3c0 : routing selector for LOAD  */
    int32_t  file_handle;       /* <- DAT_0046b3c4 : active loader handle       */
    int32_t  field_handle;      /* <- DAT_0046b3c8 : active field/event handle  */
    int32_t  event_file_code;   /* <- DAT_0046b228 : file code for EVENT_OPEN   */
} EngineState;

/* --------------------------------------------------------------------- *
 * Below-the-seam side-effect calls, enumerated for audit. The dispatch
 * routes every pure side-effect (no control-flow-relevant return) through
 * EngineHooks::subsystem(ctx, tag, a, b, c). Tags carry the original guest
 * address in the name so the mapping stays traceable. These are HOOKS,
 * not reimplementations: rendering/audio/VU/GS, loader IO, script VM, and
 * the per-subsystem per-frame updates all live below this seam.
 * --------------------------------------------------------------------- */
typedef enum EngineSubsysCall {
    /* per-frame preamble updates (run before the switch, every frame) */
    ENGINE_SUB_PREUPDATE_1,      /* FUN_00246c98 */
    ENGINE_SUB_PREUPDATE_2,      /* FUN_00247048 */
    ENGINE_SUB_PREUPDATE_3,      /* FUN_00247318 */
    ENGINE_SUB_PREUPDATE_4,      /* FUN_003fdf00 */
    ENGINE_SUB_RESET_SCRATCH,    /* DAT_004fc7d8 = 0 */
    /* fade / screen control */
    ENGINE_SUB_FADE,             /* FUN_003ed740(a,b)  (title fade / curtain) */
    ENGINE_SUB_FADE_SET,         /* FUN_003ed7d0(a) */
    /* input / HUD gating */
    ENGINE_SUB_INPUT_GATE,       /* FUN_00254628(a) */
    ENGINE_SUB_INPUT_MODE,       /* FUN_002420e0(a,b,c) */
    /* object/layer enable (second bitmask setter FUN_00241990(id,on)) */
    ENGINE_SUB_LAYER_ENABLE,     /* FUN_00241990(a=id,b=on) */
    /* movie / cutscene playback */
    ENGINE_SUB_PLAY_MOVIE,       /* FUN_00241ed8(a=id,0,0,1) */
    /* scene / frame bookkeeping */
    ENGINE_SUB_SCENE_PREP,       /* FUN_002e29b8(a) */
    ENGINE_SUB_FRAME_ADVANCE,    /* FUN_00241a20(a) */
    ENGINE_SUB_SCENE_BEGIN,      /* FUN_00253fd8(a) */
    ENGINE_SUB_SCENE_OVERLAY,    /* FUN_00aa3ad8(a)  (overlay ELF @0x00aa0000) */
    ENGINE_SUB_BOOT_SETUP,       /* FUN_003fdf00-family boot prep FUN_003e0cc8(a) */
    /* loader lifecycle */
    ENGINE_SUB_FILE_KICK,        /* FUN_0024f580(handle,0,0,0) */
    ENGINE_SUB_FILE_CLOSE,       /* FUN_00250098(handle) */
    ENGINE_SUB_FILE_FINISH,      /* FUN_0024fe78(a) */
    ENGINE_SUB_BOOT_COMMIT,      /* FUN_002438c0 / FUN_002438e0 */
    /* render / script bring-up + teardown */
    ENGINE_SUB_RENDER_TOGGLE,    /* FUN_00312c68(a) */
    ENGINE_SUB_RENDER_FLUSH,     /* FUN_0030f3d8 */
    ENGINE_SUB_RENDER_BIND,      /* FUN_0031d6b8(a,b) */
    ENGINE_SUB_RENDER_SYNC_A,    /* FUN_003fdaa0 */
    ENGINE_SUB_RENDER_SYNC_B,    /* FUN_002561b8(a) */
    ENGINE_SUB_RENDER_VTABLE_A,  /* (**DAT_0047d95c+700)  object-method dispatch */
    ENGINE_SUB_RENDER_VTABLE_B,  /* (**DAT_0047d938+0xe4) object-method dispatch */
    ENGINE_SUB_FRAME_MUTEX_LOCK, /* FUN_0042d228(0x4bccd8) */
    ENGINE_SUB_FRAME_MUTEX_UNLK, /* FUN_0042d390(0x4bccd8) */
    ENGINE_SUB_RENDER_MISC_A,    /* FUN_00242990(a) */
    ENGINE_SUB_RENDER_MISC_B,    /* FUN_00242980(a) */
    ENGINE_SUB_RENDER_MISC_C,    /* FUN_0023cf18(a) */
    ENGINE_SUB_RENDER_MISC_D,    /* FUN_00242e10 */
    /* scene / field / event setup actions */
    ENGINE_SUB_FIELD_PREP,       /* FUN_00246fc8 */
    ENGINE_SUB_FIELD_FINALIZE,   /* FUN_003ad530 */
    ENGINE_SUB_FIELD_TEARDOWN,   /* FUN_003ad5e0 */
    ENGINE_SUB_FIELD_FREE,       /* FUN_003ff458(handle,a) */
    ENGINE_SUB_FIELD_EXTRA,      /* FUN_00242ed8 */
    ENGINE_SUB_FIELD_SPAWN,      /* FUN_00403a20 */
    ENGINE_SUB_EVENT_HINT,       /* FUN_00242c18(a) */
    ENGINE_SUB_SCRIPT_RESET,     /* FUN_003ea1d0 / FUN_003e62d0 */
    ENGINE_SUB_SAVE_APPLY,       /* FUN_0023dc48 */
    ENGINE_SUB_SAVE_SELECT,      /* FUN_0023d5a8(a) */
    ENGINE_SUB_SAVE_LOAD_BYTE,   /* FUN_00241e68(a)  (consumes a ctx byte) */
    ENGINE_SUB_RESUME_DRAW,      /* FUN_00379908(a) */
    ENGINE_SUB_INTRO_PREP,       /* FUN_003bf3c8(a) */
    ENGINE_SUB_INTRO_COMMIT,     /* FUN_0023f230 */
    ENGINE_SUB_ATTRACT_PREP,     /* FUN_003ebfa0 */
    ENGINE_SUB_MISC              /* catch-all for any other below-seam call */
} EngineSubsysCall;

/* --------------------------------------------------------------------- *
 * The seam. Every call out of the state machine that either returns a
 * control-flow-relevant value or performs a below-seam side effect goes
 * through here. A query returns an int the dispatch branches on; the
 * subsystem sink performs an ordered side effect. ctx is opaque caller
 * state (in the real engine, the global machine; in tests, a script).
 *
 * Use engine_hooks_default() to get an all-safe baseline, then override.
 * The dispatch assumes every pointer is non-NULL.
 * --------------------------------------------------------------------- */
typedef struct EngineHooks {
    void *ctx;

    /* ---- control-flow queries (guest FUN_ in comment) ---- */
    int (*title_boot_poll)   (void *ctx);               /* FUN_00245f68  (>0 ready) */
    int (*clock_now)         (void *ctx);               /* FUN_0023c1f0  (frame clock) */
    int (*save_slot_check)   (void *ctx);               /* FUN_00245978 */
    int (*boot_target_resolve)(void *ctx);              /* FUN_002459e8  -> boot_target */
    int (*alt_boot_poll)     (void *ctx);               /* FUN_00241ea0 */
    int (*title_update_poll) (void *ctx);               /* FUN_00247450 */
    int (*input_test)        (void *ctx, int id);       /* FUN_002419d8(id) (!=0 active) */
    int (*player_ctx_read)   (void *ctx, int off);      /* FUN_0023c890 then read +off */
    int (*disc_ctx_read)     (void *ctx);               /* FUN_0023ce00 then read [0] */
    int (*field_boot_mode)   (void *ctx);               /* FUN_00242c28  (==3 special) */
    int (*next_file_code)    (void *ctx);               /* FUN_002423f0 */
    int (*file_open)         (void *ctx, int code, int arg); /* FUN_0024fe88 -> handle */
    int (*file_status)       (void *ctx, int handle);   /* guest handle+0x23a (3=done,4=err) */
    int (*fade_poll)         (void *ctx);               /* FUN_00241b98(0,0,0) */
    int (*pause_poll)        (void *ctx);               /* FUN_00400508(2,1) */
    int (*field_enter_poll)  (void *ctx);               /* FUN_00241e78  (>=0 ready) */
    int (*field_cam)         (void *ctx, int i);        /* FUN_00241e88(i) */
    int (*res_param)         (void *ctx, int id);       /* FUN_00252120(id) */
    int (*field_result_make) (void *ctx, int a, int b, int c, int d); /* FUN_003ff0c0 -> handle */
    int (*field_run_poll)    (void *ctx, int handle);   /* FUN_00402220(handle) */
    int (*field_exit_poll)   (void *ctx, int handle);   /* FUN_00401b90(handle) 0/1/2/3/5/6 */
    int (*field_substate)    (void *ctx);               /* FUN_0023d598  (0/1) */
    int (*region_check)      (void *ctx);               /* FUN_002424a0 */
    int (*prev_state_query)  (void *ctx);               /* FUN_00241f30 */
    int (*event_sync_poll)   (void *ctx);               /* FUN_0023dc90 */
    int (*title_return_poll) (void *ctx, int arg);      /* FUN_00245a68(arg) */
    int (*intro_poll)        (void *ctx);               /* FUN_002406b0 */
    int (*intro_fade_poll)   (void *ctx);               /* FUN_00246fb8 */
    int (*reload_poll)       (void *ctx);               /* FUN_00246cd0 */

    /* ---- below-seam side-effect sink ---- */
    void (*subsystem)        (void *ctx, int tag, int a, int b, int c);
    void (*fatal)            (void *ctx, int code);     /* trap(1): unreachable assert */
} EngineHooks;

/* --------------------------------------------------------------------- *
 * State / flag primitives  (the clean replacements for the 4 small funcs)
 * --------------------------------------------------------------------- */

/* Zero all module state (scene_state = 0, flags clear). */
void engine_state_init(EngineState *es);

/* FUN_00241d38: set the scene state. A NEGATIVE argument pops to the
 * previous state (the back-stack is one deep). Writing the same state the
 * machine is already in is a no-op and does NOT disturb prev. */
void engine_set_scene_state(EngineState *es, int32_t next);

/* FUN_00241d68: current scene state. */
int32_t engine_get_scene_state(const EngineState *es);

/* FUN_00241378: set (on!=0) or clear (on==0) mode-flag bit `bit` (0..31). */
void engine_set_mode_flag(EngineState *es, unsigned bit, int on);

/* FUN_002413c0: test mode-flag bit `bit` (0..31). */
int engine_test_mode_flag(const EngineState *es, unsigned bit);

/* --------------------------------------------------------------------- *
 * The spine: one per-frame tick of the top-level state machine.
 * FUN_00247508. Runs the preamble updates, then dispatches on the current
 * scene state, applying the pure transition logic and routing every
 * below-seam call through `hooks`.
 * --------------------------------------------------------------------- */
void engine_state_dispatch(EngineState *es, const EngineHooks *hooks);

/* An all-safe baseline: queries return 0, side effects are no-ops, fatal
 * does nothing. Override fields for real behaviour / tests. */
EngineHooks engine_hooks_default(void);

#ifdef __cplusplus
}
#endif

#endif /* DOC_ENGINE_STATE_H */
