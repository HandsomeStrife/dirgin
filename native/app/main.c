/* =====================================================================
 * main.c  --  NATIVE APPLICATION SHELL for the Dirge of Cerberus purist
 * PC port (integration milestone).
 *
 * This is NOT an engine subsystem. It is the native app that WIRES THE
 * REAUTHORED ENGINE SUBSYSTEMS TOGETHER and RUNS THE FRAME LOOP, proving
 * that the independently-reauthored modules in native/engine/ execute as a
 * coherent whole rather than only passing per-module unit tests.
 *
 * What it ties together (all real, ported engine code -- read-only to us):
 *   - core/frame_loop.c   : the per-frame main loop (FUN_00248110)          .
 *   - core/engine_state.c : the top-level scene-state machine (FUN_00247508).
 *   - core/input.c        : controller poll + edge detect (FUN_00254638..)  .
 *   - scene/camera.c      : view/projection build (FUN_00259928.., 00263968).
 *   - scene/object_list.c : object-list walk + render-queue flagging        .
 *   - scene/scene_graph.c : model-node tree walk, draw-queue bits           .
 *   - scene/material.c    : material -> texture identity bridge             .
 *
 * The one purist substitution the engine already defines -- the PS2 GS DMA
 * transport bracket replaced by the native DocRenderer interface -- is
 * satisfied here by a LOGGING renderer backend. The padman RPC is replaced
 * by a scripted/synthetic DocInputBackend. The scene is a hand-built
 * DocSceneObject / DocModelNode tree (real model loading via doc_model is a
 * separate, busy subsystem) so the object-list traversal and render-queue
 * flagging actually run against real data.
 *
 * ADAPTERS NOTED (worked around in app/, engine untouched):
 *   A1. The view/camera is not a frame_loop hook (camera upload to VU1 is
 *       below the renderer seam). We drive the ported camera subsystem from
 *       the app's input_poll hook -- "read this frame's pad, steer the view"
 *       -- then the renderer reads the camera at begin_frame. See
 *       app_input_poll / app_camera_control.
 *   A2. doc_scene_object_list_update() flags the engine's OWN internal main
 *       render context (scene_graph.c's g_render_queue_main, bit 4) -- a
 *       file-static we cannot read. We observe its effect directly on our
 *       scene nodes' queue_flags. Separately we exercise the app-owned
 *       DocRenderQueue + the by-kind node-list walker (bit 10) so the
 *       render queue we submit carries live per-frame visit counts.
 * ===================================================================== */
#include "frame_loop.h"
#include "input.h"
#include "camera.h"
#include "object_list.h"   /* pulls in scene_graph.h */
#include "material.h"
#include "dmath.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define APP_NUM_FRAMES   120
#define RAD2DEG          57.2957795131f

/* --------------------------------------------------------------------- *
 * Per-frame trace accumulators + everything threaded through the hooks.
 * One AppContext is the ctx for the EngineHooks, the DocFrameHooks, the
 * DocRenderer and the DocInputBackend -- the single integration seam.
 * --------------------------------------------------------------------- */
typedef struct AppContext {
    int             frame;          /* current 0-based frame index         */
    DocEngine      *engine;         /* back-ref for state name             */
    DocInputState   input;          /* the input subsystem                 */
    DocCamera       camera;         /* the view subsystem                  */
    DocRenderQueue *queue;          /* the render queue we submit          */

    /* ---- per-frame trace capture (reset at top of each app frame) ---- */
    char   eng_subs[512];           /* engine-dispatch subsystem tags fired */
    int    eng_sub_count;
    int    frame_sub_count;         /* below-seam frame-loop hooks fired    */
    int    r_begin, r_submit, r_end, r_present, r_present_mode;

    /* renderer-built view data (proof the matrices are rebuilt each frame) */
    float  view_t[3];               /* camera position in view space        */
    float  proj00;                  /* projection [0][0]                    */

    /* material-resolve result for the frame's sample draw word             */
    DocSelectStatus mat_status;
    DocTextureDesc  mat_desc;
} AppContext;

/* ===================================================================== *
 * Synthetic scene: a hand-built object list + model-node trees.
 * 3 entities, 3 groups, 4 leaves. Static storage so the pointers the
 * engine walks stay valid for the whole run.
 * ===================================================================== */
static DocMesh         g_mesh[4];
static DocNodeBinding  g_bind[4];
static DocModelNode    g_leaf[4];
static DocModelNode   *g_kids0[2], *g_kids1[1], *g_kids2[1];
static DocModelNode    g_grp[3];
static DocModelParts   g_parts[3];
static DocObjectDrawInfo g_info[3];
static DocSceneObject  g_obj[3];
static DocRenderQueue  g_queue;

/* Material side: minimal model + texture table so the material->texture
 * bridge resolves a real descriptor (slot 2, 256x128). */
static int               g_submesh_dummy;
static DocSubmesh       *g_subs[1];
static DocMaterialGroup  g_group0;
static DocMaterialGroup *g_groups[1];
static const uint16_t    g_layerlist0[2] = { 1, 2 };   /* count=1, tag=2 (table slot 2) */
static const uint16_t   *g_layerlists[DOC_MATERIAL_MAX_LAYER_LISTS];
static DocMaterial       g_mat0;
static DocMaterial      *g_materials[1];
static DocMaterialModel  g_matmodel;
static DocTextureHandle  g_texhandle;
static DocTextureHandle *g_texhandles[3];
static DocTextureContext g_texctx;

static void build_scene(void)
{
    const char *mesh_names[4]  = { "vincent_body", "vincent_cape", "gun_cerberus", "enemy_guard" };
    const char *group_names[3] = { "vincent", "weapon", "guard" };
    int i;

    for (i = 0; i < 4; i++) {
        memset(&g_mesh[i], 0, sizeof(g_mesh[i]));
        g_mesh[i].name = mesh_names[i];
        g_mesh[i].submesh_count = 1;

        g_bind[i].id = (uint8_t)(0x10 + i);

        memset(&g_leaf[i], 0, sizeof(g_leaf[i]));
        g_leaf[i].kind        = DOC_NODE_LEAF;
        g_leaf[i].queue_flags = 0;
        g_leaf[i].mesh        = &g_mesh[i];
        g_leaf[i].name        = mesh_names[i];
        g_leaf[i].select_kind = 7;
        g_leaf[i].binding     = &g_bind[i];
    }

    g_kids0[0] = &g_leaf[0]; g_kids0[1] = &g_leaf[1];
    g_kids1[0] = &g_leaf[2];
    g_kids2[0] = &g_leaf[3];

    for (i = 0; i < 3; i++) {
        memset(&g_grp[i], 0, sizeof(g_grp[i]));
        g_grp[i].kind        = DOC_NODE_GROUP;
        g_grp[i].name        = group_names[i];
        g_grp[i].select_kind = 7;          /* matched by the by-kind walk */
    }
    g_grp[0].child_count = 2; g_grp[0].children = g_kids0;
    g_grp[1].child_count = 1; g_grp[1].children = g_kids1;
    g_grp[2].child_count = 1; g_grp[2].children = g_kids2;

    /* model-parts: one root group per entity */
    for (i = 0; i < 3; i++) {
        memset(&g_parts[i], 0, sizeof(g_parts[i]));
        g_parts[i].flags      = 0x80;      /* visible/enabled toggle bit  */
        g_parts[i].node_count = 1;         /* non-zero: fan-out proceeds   */
        g_parts[i].roots[0]   = &g_grp[i];
    }

    /* draw info: obj0 mode1+active, obj1 mode2+active, obj2 mode3 not-active */
    g_info[0].flags = DOC_OBJ_VISIBLE | DOC_OBJ_LIST_ACTIVE | (1u << 16);
    g_info[1].flags = DOC_OBJ_VISIBLE | DOC_OBJ_LIST_ACTIVE | (2u << 16);
    g_info[2].flags = DOC_OBJ_VISIBLE | (3u << 16);

    for (i = 0; i < 3; i++) {
        memset(&g_obj[i], 0, sizeof(g_obj[i]));
        g_obj[i].model_parts = &g_parts[i];
        g_obj[i].draw_info   = &g_info[i];
        g_obj[i].name        = group_names[i];
    }
    g_obj[0].next = &g_obj[1];
    g_obj[1].next = &g_obj[2];
    g_obj[2].next = NULL;

    /* the app-owned render queue: a flat node-list for the by-kind walker,
     * linked through DocModelNode.next (distinct from the tree children). */
    memset(&g_queue, 0, sizeof(g_queue));
    g_queue.id        = 0x4ee2e0;
    g_queue.name      = "main";
    g_queue.node_list = &g_grp[0];
    g_grp[0].next     = &g_grp[1];
    g_grp[1].next     = &g_grp[2];
    g_grp[2].next     = NULL;

    /* register with the object-list module (replaces the DAT_ globals) */
    doc_scene_object_list_set_head(&g_obj[0]);
    doc_scene_object_list_set_enabled(1);
    doc_scene_set_pass_source_flag(1);

    /* ---- material/texture tables (minimal, resolves OK) ---- */
    g_subs[0]   = (DocSubmesh *)&g_submesh_dummy;
    memset(&g_group0, 0, sizeof(g_group0));
    g_group0.submesh_count     = 1;
    g_group0.material_selector = 0;
    g_group0.submeshes         = g_subs;
    g_groups[0] = &g_group0;

    memset(g_layerlists, 0, sizeof(g_layerlists));
    g_layerlists[0] = g_layerlist0;
    memset(&g_mat0, 0, sizeof(g_mat0));
    g_mat0.stage0      = 0;
    g_mat0.layer_lists = g_layerlists;
    g_materials[0] = &g_mat0;

    memset(&g_matmodel, 0, sizeof(g_matmodel));
    g_matmodel.group_count = 1;
    g_matmodel.groups      = g_groups;
    g_matmodel.materials   = g_materials;

    memset(&g_texhandle, 0, sizeof(g_texhandle));
    g_texhandle.width   = 256;
    g_texhandle.height  = 128;
    g_texhandle.psm     = 0x00;
    g_texhandle.tbw     = 4;
    g_texhandle.cpsm    = 0;
    g_texhandle.texels  = (const void *)1;   /* non-null: has texels */
    g_texhandle.clut    = NULL;
    g_texhandle.override = NULL;

    g_texhandles[0] = NULL;
    g_texhandles[1] = NULL;
    g_texhandles[2] = &g_texhandle;          /* slot 2 (tag 2) */
    g_texctx.handles      = g_texhandles;
    g_texctx.handle_count = 3;
}

/* ===================================================================== *
 * Name tables for the trace.
 * ===================================================================== */
static const char *scene_state_name(int32_t s)
{
    switch (s) {
        case 0:                          return "(init)";
        case SCENE_STATE_BOOT:           return "BOOT";
        case SCENE_STATE_TITLE:          return "TITLE";
        case SCENE_STATE_TITLE_SUB:      return "TITLE_SUB";
        case SCENE_STATE_LOAD_DISPATCH:  return "LOAD_DISPATCH";
        case SCENE_STATE_TITLE_HOLD:     return "TITLE_HOLD";
        case SCENE_STATE_SYS_FILE_OPEN:  return "SYS_FILE_OPEN";
        case SCENE_STATE_BOOT_FILE_OPEN: return "BOOT_FILE_OPEN";
        case SCENE_STATE_BOOT_FILE_WAIT: return "BOOT_FILE_WAIT";
        case SCENE_STATE_BOOT_FADE:      return "BOOT_FADE";
        case SCENE_STATE_RELOAD:         return "RELOAD";
        case SCENE_STATE_BOOT_FINALIZE:  return "BOOT_FINALIZE";
        case SCENE_STATE_FIELD_SETUP:    return "FIELD_SETUP";
        case SCENE_STATE_FIELD_LOAD:     return "FIELD_LOAD";
        case SCENE_STATE_FIELD_RUN:      return "FIELD_RUN";
        case SCENE_STATE_FIELD_RESUME:   return "FIELD_RESUME";
        case SCENE_STATE_EVENT_OPEN:     return "EVENT_OPEN";
        case SCENE_STATE_EVENT_WAIT:     return "EVENT_WAIT";
        case SCENE_STATE_EVENT_RUN:      return "EVENT_RUN";
        case SCENE_STATE_EVENT_DONE:     return "EVENT_DONE";
        case SCENE_STATE_INTRO:          return "INTRO";
        case SCENE_STATE_INTRO_FADE:     return "INTRO_FADE";
        case SCENE_STATE_RELOAD_WAIT:    return "RELOAD_WAIT";
        case SCENE_STATE_ATTRACT:        return "ATTRACT";
        default:                         return "?";
    }
}

static const char *engine_sub_name(int tag)
{
    switch (tag) {
        case ENGINE_SUB_PREUPDATE_1:   return "preupd1";
        case ENGINE_SUB_PREUPDATE_2:   return "preupd2";
        case ENGINE_SUB_PREUPDATE_3:   return "preupd3";
        case ENGINE_SUB_PREUPDATE_4:   return "preupd4";
        case ENGINE_SUB_RESET_SCRATCH: return "reset_scratch";
        case ENGINE_SUB_FADE:          return "fade";
        case ENGINE_SUB_FADE_SET:      return "fade_set";
        case ENGINE_SUB_INPUT_GATE:    return "input_gate";
        case ENGINE_SUB_INPUT_MODE:    return "input_mode";
        case ENGINE_SUB_LAYER_ENABLE:  return "layer_enable";
        case ENGINE_SUB_PLAY_MOVIE:    return "play_movie";
        case ENGINE_SUB_SCENE_PREP:    return "scene_prep";
        case ENGINE_SUB_FRAME_ADVANCE: return "frame_advance";
        case ENGINE_SUB_SCENE_BEGIN:   return "scene_begin";
        case ENGINE_SUB_SCENE_OVERLAY: return "scene_overlay";
        case ENGINE_SUB_BOOT_SETUP:    return "boot_setup";
        case ENGINE_SUB_FILE_KICK:     return "file_kick";
        case ENGINE_SUB_FILE_CLOSE:    return "file_close";
        case ENGINE_SUB_FILE_FINISH:   return "file_finish";
        case ENGINE_SUB_BOOT_COMMIT:   return "boot_commit";
        case ENGINE_SUB_RENDER_TOGGLE: return "render_toggle";
        case ENGINE_SUB_RENDER_FLUSH:  return "render_flush";
        case ENGINE_SUB_RENDER_BIND:   return "render_bind";
        case ENGINE_SUB_RENDER_SYNC_A: return "render_sync_a";
        case ENGINE_SUB_RENDER_SYNC_B: return "render_sync_b";
        case ENGINE_SUB_RENDER_VTABLE_A: return "vtable_a";
        case ENGINE_SUB_RENDER_VTABLE_B: return "vtable_b";
        case ENGINE_SUB_FRAME_MUTEX_LOCK: return "mutex_lock";
        case ENGINE_SUB_FRAME_MUTEX_UNLK: return "mutex_unlk";
        case ENGINE_SUB_RENDER_MISC_A: return "misc_a";
        case ENGINE_SUB_RENDER_MISC_B: return "misc_b";
        case ENGINE_SUB_RENDER_MISC_C: return "misc_c";
        case ENGINE_SUB_RENDER_MISC_D: return "misc_d";
        case ENGINE_SUB_FIELD_PREP:    return "field_prep";
        case ENGINE_SUB_FIELD_FINALIZE: return "field_finalize";
        case ENGINE_SUB_FIELD_TEARDOWN: return "field_teardown";
        case ENGINE_SUB_FIELD_FREE:    return "field_free";
        case ENGINE_SUB_FIELD_EXTRA:   return "field_extra";
        case ENGINE_SUB_FIELD_SPAWN:   return "field_spawn";
        case ENGINE_SUB_EVENT_HINT:    return "event_hint";
        case ENGINE_SUB_SCRIPT_RESET:  return "script_reset";
        case ENGINE_SUB_SAVE_APPLY:    return "save_apply";
        case ENGINE_SUB_SAVE_SELECT:   return "save_select";
        case ENGINE_SUB_SAVE_LOAD_BYTE: return "save_load_byte";
        case ENGINE_SUB_RESUME_DRAW:   return "resume_draw";
        case ENGINE_SUB_INTRO_PREP:    return "intro_prep";
        case ENGINE_SUB_INTRO_COMMIT:  return "intro_commit";
        case ENGINE_SUB_ATTRACT_PREP:  return "attract_prep";
        case ENGINE_SUB_MISC:          return "misc";
        default:                       return "sub?";
    }
}

static const struct { uint16_t bit; const char *name; } g_btns[] = {
    { DOC_PAD_SELECT, "SELECT" }, { DOC_PAD_L3, "L3" }, { DOC_PAD_R3, "R3" },
    { DOC_PAD_START, "START" },   { DOC_PAD_UP, "UP" }, { DOC_PAD_RIGHT, "RIGHT" },
    { DOC_PAD_DOWN, "DOWN" },     { DOC_PAD_LEFT, "LEFT" },
    { DOC_PAD_L2, "L2" }, { DOC_PAD_R2, "R2" }, { DOC_PAD_L1, "L1" }, { DOC_PAD_R1, "R1" },
    { DOC_PAD_TRIANGLE, "TRIANGLE" }, { DOC_PAD_CIRCLE, "CIRCLE" },
    { DOC_PAD_CROSS, "CROSS" }, { DOC_PAD_SQUARE, "SQUARE" }
};

static void fmt_buttons(char *buf, size_t n, uint16_t mask)
{
    size_t off = 0;
    unsigned i;
    buf[0] = '\0';
    for (i = 0; i < sizeof(g_btns) / sizeof(g_btns[0]); i++) {
        if (mask & g_btns[i].bit) {
            int w = snprintf(buf + off, n - off, "%s%s",
                             off ? " " : "", g_btns[i].name);
            if (w > 0 && (size_t)w < n - off) off += (size_t)w;
        }
    }
}

static void fmt_flags(char *buf, size_t n, uint32_t flags)
{
    static const struct { unsigned bit; const char *name; } ft[] = {
        { ENGINE_FLAG_FADE_HOLD, "FADE_HOLD" },
        { ENGINE_FLAG_DIAG, "DIAG" },
        { ENGINE_FLAG_EVENT_SKIP, "EVENT_SKIP" },
        { ENGINE_FLAG_EVENT_READY, "EVENT_READY" },
        { ENGINE_FLAG_FIELD_READY, "FIELD_READY" },
        { ENGINE_FLAG_LOOP_KEEPALIVE, "KEEPALIVE" }
    };
    size_t off = 0;
    unsigned i;
    buf[0] = '\0';
    for (i = 0; i < sizeof(ft) / sizeof(ft[0]); i++) {
        if (flags & (1u << ft[i].bit)) {
            int w = snprintf(buf + off, n - off, "%s%s", off ? "|" : "", ft[i].name);
            if (w > 0 && (size_t)w < n - off) off += (size_t)w;
        }
    }
}

/* ===================================================================== *
 * Scripted synthetic controller backend (replaces the padman RPC).
 * Port 0 connected; port 1 unplugged. Button/stick input is a function of
 * the frame number so the run is reproducible and the state machine /
 * camera respond to a fixed script.
 * ===================================================================== */
static int app_read_pad(void *ctx, int port, uint8_t out[DOC_PAD_BUFFER_SIZE])
{
    AppContext *app = (AppContext *)ctx;
    int f = app->frame;
    uint16_t pressed = 0;
    uint8_t rx = DOC_PAD_STICK_CENTRE, ry = DOC_PAD_STICK_CENTRE;
    uint8_t lx = DOC_PAD_STICK_CENTRE, ly = DOC_PAD_STICK_CENTRE;
    uint16_t alw;

    memset(out, 0, DOC_PAD_BUFFER_SIZE);
    if (port != 0)
        return 0;                       /* port 1 unplugged */

    /* ---- scripted digital buttons ---- */
    if (f == 5)                   pressed |= DOC_PAD_START;     /* confirm boot */
    if (f >= 22 && f <= 28)       pressed |= DOC_PAD_UP;        /* d-pad: pitch */
    if (f >= 34 && f <= 40)       pressed |= DOC_PAD_RIGHT;     /* d-pad: yaw   */
    if (f >= 46 && f <= 52)       pressed |= DOC_PAD_DOWN;      /* d-pad: pitch */
    if (f >= 58 && f <= 64)       pressed |= DOC_PAD_LEFT;      /* d-pad: yaw   */
    if (f == 70)                  pressed |= DOC_PAD_CROSS;     /* -> rumble    */
    if (f >= 80 && f <= 95)       pressed |= DOC_PAD_L1;

    /* ---- scripted left-stick (eye translation) ---- */
    if (f >= 40 && f <= 50)       lx = 0xE0;                    /* push right   */
    if (f >= 100 && f <= 115)     ly = 0x20;                    /* push forward */

    out[0x01] = DOC_PAD_MODE_ANALOG;
    alw = (uint16_t)(~pressed & 0xffffu);                       /* active-low   */
    out[0x02] = (uint8_t)(alw & 0xff);
    out[0x03] = (uint8_t)((alw >> 8) & 0xff);
    out[0x04] = rx; out[0x05] = ry; out[0x06] = lx; out[0x07] = ly;
    return 1;                           /* connected */
}

static void app_set_actuator(void *ctx, int port, const uint8_t act[6])
{
    AppContext *app = (AppContext *)ctx;
    /* Record a rumble request in the trace when one is actually staged. */
    if ((act[0] || act[1]) && port == 0) {
        printf("        [input] actuator port0 rumble small=%u large=%u\n",
               act[0], act[1]);
    }
    (void)app;
}

/* ===================================================================== *
 * The app's view/camera controller (ADAPTER A1). Runs from input_poll:
 * reads this frame's decoded pad and steers the ported camera subsystem.
 *   - D-pad rotates the view (yaw/pitch) via doc_camera_set_angles.
 *   - Left stick translates the eye via doc_camera_move + the ported
 *     dead-zone normalize (doc_input_stick_normalize).
 * ===================================================================== */
static void app_camera_control(AppContext *app)
{
    DocCamera *c = &app->camera;
    const int   port      = 0;
    const float yaw_step   = doc_angle_units_to_radians(48.0f);
    const float pitch_step = doc_angle_units_to_radians(32.0f);
    const float move_speed = 40.0f;
    dvec3 ang = c->angles;
    int lx, ly;
    float nx, ny;

    if (doc_input_held(&app->input, port, DOC_PAD_LEFT))  ang.y -= yaw_step;
    if (doc_input_held(&app->input, port, DOC_PAD_RIGHT)) ang.y += yaw_step;
    if (doc_input_held(&app->input, port, DOC_PAD_UP))    ang.x -= pitch_step;
    if (doc_input_held(&app->input, port, DOC_PAD_DOWN))  ang.x += pitch_step;
    doc_camera_set_angles(c, ang);

    lx = (int)app->input.pads[port].current.lstick_x - DOC_PAD_STICK_CENTRE;
    ly = (int)app->input.pads[port].current.lstick_y - DOC_PAD_STICK_CENTRE;
    if (doc_input_stick_normalize(lx, ly, &nx, &ny)) {
        /* stick up (ly negative) drives forward (+... into the scene) */
        doc_camera_move(c, nx * move_speed, 0.0f, -ny * move_speed);
    }

    /* stage a rumble on the CROSS press so the latch hook has work to flush */
    if (doc_input_pressed(&app->input, port, DOC_PAD_CROSS))
        doc_input_set_rumble(&app->input, port, 1, 200);
    else
        doc_input_set_rumble(&app->input, port, 0, 0);
}

/* ===================================================================== *
 * Frame-loop hook table implementations (DocFrameHooks).
 * ===================================================================== */
static void app_input_poll(void *ctx)
{
    AppContext *app = (AppContext *)ctx;
    doc_input_begin(&app->input);   /* FUN_002548d8: poll both pads, edges */
    app_camera_control(app);        /* ADAPTER A1: input -> view           */
}

static void app_input_latch(void *ctx)
{
    AppContext *app = (AppContext *)ctx;
    doc_input_latch(&app->input);   /* FUN_002548f8: flush actuator state  */
}

/* ADAPTER A2: object-list walk (real) + app render-queue by-kind flagging. */
static void app_run_object_list(DocEngine *e)
{
    AppContext *app = (AppContext *)e->hooks->ctx;
    (void)e;
    /* the real FUN_00408758 entry: walks the module object list, sets each
     * visible object's draw mode + render_state, which flags the engine's
     * internal main render context (bit 4) across our model nodes. */
    doc_scene_object_list_update();

    /* exercise the app-owned render queue + the by-kind node-list walker
     * (FUN_002ebdb8): flag kind==7 subtrees with bit 10. Reset the per-run
     * visit counters so the submitted queue carries this frame's counts. */
    app->queue->visited_groups = 0;
    app->queue->visited_leaves = 0;
    doc_render_queue_flag_nodes_by_kind(app->queue, 7, 0, 10);
}

static void app_frame_sub(void *ctx, int tag, int arg)
{
    AppContext *app = (AppContext *)ctx;
    (void)tag; (void)arg;
    app->frame_sub_count++;   /* count every below-seam per-subsystem update */
}

/* ===================================================================== *
 * Native renderer backend (DocRenderer) -- the purist GS-DMA substitution.
 * Logging/stub: records the calls and the frame's view + submitted scene.
 * ===================================================================== */
static void app_r_begin(void *ctx)
{
    AppContext *app = (AppContext *)ctx;
    dmat4 view = doc_camera_view_matrix(&app->camera);        /* world->camera */
    dmat4 proj = doc_camera_projection_matrix(&app->camera);  /* viewscreen    */
    app->view_t[0] = view.m[12];
    app->view_t[1] = view.m[13];
    app->view_t[2] = view.m[14];
    app->proj00    = proj.m[0];
    app->r_begin++;
}

static void app_r_submit(void *ctx, DocRenderQueue *q)
{
    AppContext *app = (AppContext *)ctx;
    uint32_t draw_word = 0;   /* group 0, submesh 0, single (non-batch) path */
    app->r_submit++;
    (void)q;
    /* run the material->texture bridge for the sample draw word */
    app->mat_status = doc_material_resolve_texture(&g_matmodel, &g_texctx,
                                                   draw_word, &app->mat_desc);
}

static void app_r_end(void *ctx)    { ((AppContext *)ctx)->r_end++; }
static void app_r_present(void *ctx, int mode)
{
    AppContext *app = (AppContext *)ctx;
    app->r_present++;
    app->r_present_mode = mode;
}
static int  app_r_pending(void *ctx){ (void)ctx; return 0; }

/* ===================================================================== *
 * Scripted engine-state dispatch hooks (EngineHooks).
 * These supply ONLY the gate conditions; every transition is decided by
 * the real engine_state_dispatch logic. The script drives the spine:
 *   BOOT -> TITLE -> TITLE_HOLD -> ATTRACT -> LOAD_DISPATCH ->
 *   FIELD_SETUP -> FIELD_LOAD -> FIELD_RUN.
 * ===================================================================== */
static int app_title_boot_poll(void *ctx)   { return ((AppContext *)ctx)->frame >= 5 ? 1 : 0; }
static int app_clock_now(void *ctx)         { return ((AppContext *)ctx)->frame * 100; }
static int app_boot_target_resolve(void *c) { (void)c; return 2; }  /* non-zero, != 9 */
static int app_alt_boot_poll(void *c)       { (void)c; return 1; }  /* -> boot_submode 2 */
static int app_field_result_make(void *c, int a, int b, int d, int e)
{ (void)c; (void)a; (void)b; (void)d; (void)e; return 0x1000; }     /* field handle */
static int app_field_run_poll(void *ctx, int h)
{ (void)h; return ((AppContext *)ctx)->frame >= 20 ? 1 : 0; }       /* loads at f>=20 */
static int app_title_return_poll(void *ctx, int arg)
{ (void)arg; return ((AppContext *)ctx)->frame >= 15 ? 1 : 0; }     /* attract->load at f>=15 */

/* The engine dispatch side-effect sink: record the tag names fired. */
static void app_engine_sub(void *ctx, int tag, int a, int b, int c)
{
    AppContext *app = (AppContext *)ctx;
    size_t off = strlen(app->eng_subs);
    int w;
    (void)a; (void)b; (void)c;
    app->eng_sub_count++;
    w = snprintf(app->eng_subs + off, sizeof(app->eng_subs) - off,
                 "%s%s", off ? " " : "", engine_sub_name(tag));
    if (w < 0 || (size_t)w >= sizeof(app->eng_subs) - off) {
        /* keep the buffer terminated if we ran out of room */
        app->eng_subs[sizeof(app->eng_subs) - 1] = '\0';
    }
}

/* ===================================================================== *
 * Per-frame trace block.
 * ===================================================================== */
static void trace_frame(AppContext *app, int32_t s_before, int32_t s_after)
{
    char btn_pressed[128], btn_released[128], flags[128];
    const DocInputPad *p0 = &app->input.pads[0];
    dvec3 ang = app->camera.angles;
    int flagged_bit4 = 0, flagged_bit10 = 0, i;

    for (i = 0; i < 4; i++) {
        if (g_leaf[i].queue_flags & (1u << 4))  flagged_bit4++;
        if (g_leaf[i].queue_flags & (1u << 10)) flagged_bit10++;
    }

    fmt_buttons(btn_pressed, sizeof(btn_pressed), p0->pressed);
    fmt_buttons(btn_released, sizeof(btn_released), p0->released);
    fmt_flags(flags, sizeof(flags), app->engine->state.mode_flags);

    printf("--- frame %3d -------------------------------------------------\n", app->frame);
    if (s_before != s_after)
        printf("  state      : %s -> %s\n", scene_state_name(s_before), scene_state_name(s_after));
    else
        printf("  state      : %s\n", scene_state_name(s_after));
    printf("  mode-flags : 0x%08x [%s]\n", app->engine->state.mode_flags, flags);
    printf("  dispatch   : %d subs { %s }\n", app->eng_sub_count, app->eng_subs);
    printf("  frame-hooks: %d below-seam updates\n", app->frame_sub_count);
    printf("  input      : port0 %s port1 %s | pressed[%s] released[%s] | Lstick(%u,%u)\n",
           p0->current.connected ? "conn" : "----",
           app->input.pads[1].current.connected ? "conn" : "----",
           btn_pressed, btn_released,
           p0->current.lstick_x, p0->current.lstick_y);
    printf("  camera     : eye(%.1f,%.1f,%.1f) ang(%.1f,%.1f,%.1f)deg | view.t(%.1f,%.1f,%.1f) proj00=%.3f\n",
           app->camera.eye.x, app->camera.eye.y, app->camera.eye.z,
           ang.x * RAD2DEG, ang.y * RAD2DEG, ang.z * RAD2DEG,
           app->view_t[0], app->view_t[1], app->view_t[2], app->proj00);
    printf("  object-list: render_state[%u,%u,%u] draw_mode[%u,%u,%u] | main-queue bit4 leaves=%d\n",
           g_obj[0].render_state & 1u, g_obj[1].render_state & 1u, g_obj[2].render_state & 1u,
           doc_scene_object_draw_mode(&g_obj[0]),
           doc_scene_object_draw_mode(&g_obj[1]),
           doc_scene_object_draw_mode(&g_obj[2]),
           flagged_bit4);
    printf("  render-q   : '%s' by-kind(k=7,bit10) groups=%u leaves=%u | flagged-leaves bit10=%d\n",
           app->queue->name, app->queue->visited_groups, app->queue->visited_leaves,
           flagged_bit10);
    printf("  renderer   : begin=%d submit=%d end=%d present=%d(mode %d) | tex ",
           app->r_begin, app->r_submit, app->r_end, app->r_present, app->r_present_mode);
    if (app->mat_status == DOC_SEL_OK)
        printf("slot=%d %ux%u psm=%u tbw=%u texels=%d\n",
               app->mat_desc.slot, app->mat_desc.width, app->mat_desc.height,
               app->mat_desc.psm, app->mat_desc.tbw, app->mat_desc.has_texels);
    else
        printf("(select status %d)\n", (int)app->mat_status);
}

/* ===================================================================== *
 * main
 * ===================================================================== */
int main(void)
{
    AppContext app;
    DocEngine  engine;
    EngineHooks   dispatch_hooks;
    DocFrameHooks frame_hooks;
    DocRenderer   renderer;
    DocInputBackend backend;
    int i, continue_frames = 0, stop_frame = -1;

    /* ---- app context + subsystems ---- */
    memset(&app, 0, sizeof(app));
    app.engine = &engine;
    app.queue  = &g_queue;

    build_scene();
    doc_camera_init(&app.camera);

    memset(&backend, 0, sizeof(backend));
    backend.ctx          = &app;
    backend.read_pad     = app_read_pad;
    backend.set_actuator = app_set_actuator;
    doc_input_init(&app.input, &backend);

    /* ---- dispatch hooks: defaults + scripted gate conditions ---- */
    dispatch_hooks = engine_hooks_default();
    dispatch_hooks.ctx                = &app;
    dispatch_hooks.title_boot_poll    = app_title_boot_poll;
    dispatch_hooks.clock_now          = app_clock_now;
    dispatch_hooks.boot_target_resolve= app_boot_target_resolve;
    dispatch_hooks.alt_boot_poll      = app_alt_boot_poll;
    dispatch_hooks.field_result_make  = app_field_result_make;
    dispatch_hooks.field_run_poll     = app_field_run_poll;
    dispatch_hooks.title_return_poll  = app_title_return_poll;
    dispatch_hooks.subsystem          = app_engine_sub;

    /* ---- frame-loop hooks: defaults + app wiring ---- */
    frame_hooks = doc_frame_hooks_default();
    frame_hooks.ctx             = &app;
    frame_hooks.run_object_list = app_run_object_list;
    frame_hooks.input_poll      = app_input_poll;
    frame_hooks.input_latch     = app_input_latch;
    frame_hooks.subsystem       = app_frame_sub;

    /* ---- native renderer (purist GS-DMA replacement) ---- */
    renderer = doc_renderer_default();
    renderer.ctx           = &app;
    renderer.begin_frame   = app_r_begin;
    renderer.submit_scene  = app_r_submit;
    renderer.end_frame     = app_r_end;
    renderer.present       = app_r_present;
    renderer.pending_query = app_r_pending;

    /* ---- build the engine ---- */
    doc_engine_init(&engine, &dispatch_hooks, &renderer, &frame_hooks, &g_queue);
    engine.render_overlay_enabled = 1;   /* gate the overlay subsystem hook on */
    engine.audio_teardown_enabled = 1;   /* exercise the shutdown teardown     */
    engine.misc_update_arg        = 0x1234;

    /* start the spine in BOOT (engine_state_init leaves it at 0) */
    engine_set_scene_state(&engine.state, SCENE_STATE_BOOT);

    printf("===============================================================\n");
    printf(" Dirge of Cerberus -- native engine integration run\n");
    printf(" Wiring: frame_loop + engine_state + input + camera +\n");
    printf("         object_list + scene_graph + material, native renderer.\n");
    printf(" Scene : %d objects, %d groups, %d model-node leaves.\n", 3, 3, 4);
    printf(" Driving %d frames of doc_engine_frame().\n", APP_NUM_FRAMES);
    printf("===============================================================\n\n");

    /* ---- preamble (FUN_00248110 lines 8-12) ---- */
    doc_engine_frame_preamble(&engine);
    printf("[preamble] present(mode1) issued; boot-cond/diag gate evaluated.\n");
    printf("           renderer present calls so far = %d\n\n", app.r_present);

    /* ---- the frame loop ---- */
    for (i = 0; i < APP_NUM_FRAMES; i++) {
        int32_t s_before, s_after;
        DocFrameResult res;

        /* reset per-frame trace capture */
        app.frame          = i;
        app.eng_subs[0]    = '\0';
        app.eng_sub_count  = 0;
        app.frame_sub_count= 0;
        app.r_begin = app.r_submit = app.r_end = app.r_present = 0;
        app.r_present_mode = -1;

        s_before = engine_get_scene_state(&engine.state);
        res      = doc_engine_frame(&engine);
        s_after  = engine_get_scene_state(&engine.state);

        trace_frame(&app, s_before, s_after);

        if (res == DOC_FRAME_STOP) {
            stop_frame = i;
            printf("  >> exit condition fired: frame STOP (no present/advance)\n");
            break;
        }
        continue_frames++;
    }

    printf("\n===============================================================\n");
    printf(" Loop finished: %d frames presented+advanced%s.\n",
           continue_frames,
           stop_frame >= 0 ? " (stopped early)" : "");
    printf(" Final scene-state : %s\n", scene_state_name(engine.state.scene_state));
    printf(" engine frame_counter=%d  dispatch frame_counter=%d\n",
           engine.frame_counter, engine.state.frame_counter);
    printf(" Final camera eye  : (%.1f, %.1f, %.1f)  angles(deg): (%.1f, %.1f, %.1f)\n",
           app.camera.eye.x, app.camera.eye.y, app.camera.eye.z,
           app.camera.angles.x * RAD2DEG, app.camera.angles.y * RAD2DEG,
           app.camera.angles.z * RAD2DEG);
    printf("===============================================================\n\n");

    /* ---- shutdown pipeline drain (FUN_00248110 lines 90-100) ---- */
    app.frame = APP_NUM_FRAMES;
    app.r_begin = app.r_submit = app.r_end = app.r_present = 0;
    printf("[shutdown] running the 32-iteration pipeline drain...\n");
    doc_engine_shutdown_drain(&engine);
    printf("           drain drove renderer: begin=%d end=%d present=%d\n",
           app.r_begin, app.r_end, app.r_present);
    printf("           (the DAT_ shutdown markers/audio-teardown live in the\n");
    printf("            full doc_engine_run() post-loop, not the drain itself.)\n");
    printf("[done] native engine ran as an integrated whole.\n");

    return 0;
}
