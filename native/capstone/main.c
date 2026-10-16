/* =====================================================================
 * capstone/main.c  --  THE CONVERGENCE.
 *
 * The native Dirge of Cerberus ENGINE frame loop (doc_engine_frame, the
 * reauthored FUN_00248110 per-frame `while(1)` body) driving the native
 * GPU RENDERER (native/gpu: renderer_gl + gl_api + EGL-offscreen) to draw
 * a REAL decoded DoC scene -- the posed+textured character (char_e000,
 * Vincent) standing in the decoded+textured BMD room 06960212 -- once per
 * frame, from the engine's own DocCamera, as the camera orbits under
 * scripted DocInput and the engine state machine advances.
 *
 * This is ONE native program, zero emulation: the engine loop is what
 * drives the GPU render each frame (R->submit_scene fires at the object-
 * list draw point inside doc_engine_frame; R->present reads the FBO to a
 * PNG). Not a standalone render call -- the loop is the driver.
 *
 * WHAT IS REAL vs STUBBED -- see NOTES.md.  In one line: the engine loop,
 * the state machine, the object-list walk, the camera/light/input math and
 * the GPU renderer + decoded meshes/textures are all REAL ported/native
 * code; the below-seam per-subsystem updates (audio, VU/GS DMA, loader IO,
 * script VM) are the frame loop's documented hooks, left as no-ops here,
 * and the per-scene light rig + state-machine poll results are SCRIPTED
 * (the real values come from KEL-VM bytecode that is a known engine gap).
 *
 * ISOLATION: built ONLY from capstone/engine_snapshot/ (a one-time copy of
 * the STABLE native/engine modules; actor.{h,c} excluded). native/gpu and
 * native/assets are linked/read read-only and never modified.
 *
 * Build:  make           (see Makefile)
 * Run:    ./capstone [-o OUTDIR] [-n FRAMES]
 * ===================================================================== */

#include "dtex.h"
#include "gfx_context.h"
#include "renderer_gl.h"

/* --- snapshot engine (built from capstone/engine_snapshot, NOT native/engine) --- */
#include "math/dmath.h"
#include "core/frame_loop.h"
#include "core/engine_state.h"
#include "core/input.h"
#include "scene/camera.h"
#include "scene/light.h"
#include "scene/scene_graph.h"
#include "scene/object_list.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(d) ((float)((d) * M_PI / 180.0))
#define RAD2DEG(r) ((float)((r) * 180.0 / M_PI))

#define MAXG 64

/* --------------------------------------------------------------------- *
 * Asset loaders -- the SAME bounds-checked DMSH / libdtex loaders the
 * scene renderer used (render_scene_textured.c), reused verbatim so the
 * untrusted on-disk formats are decoded identically and safely.
 * --------------------------------------------------------------------- */
typedef struct { RVertex *v; size_t n; float bbmin[3], bbmax[3];
                 unsigned char *tex; int tw, th; char texname[64]; } Group;

/* Load a bounds-checked "DMSH" triangle soup (untrusted on-disk format). */
static int mesh_load(const char *path, RVertex **vout, size_t *nout,
                     float bbmin[3], float bbmax[3], char *err, size_t errlen) {
    *vout = NULL; *nout = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open mesh %s", path); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 0 || (size_t)fsz < 4 + 4 + 4 + 24) {
        snprintf(err, errlen, "mesh too small (%ld)", fsz); fclose(f); return 1; }
    char magic[4]; uint32_t version = 0, vcount = 0; float bb[6];
    int ok = fread(magic, 1, 4, f) == 4 && fread(&version, 4, 1, f) == 1 &&
             fread(&vcount, 4, 1, f) == 1 && fread(bb, 4, 6, f) == 6;
    if (!ok || memcmp(magic, "DMSH", 4) != 0) {
        snprintf(err, errlen, "bad mesh magic %s", path); fclose(f); return 1; }
    long remain = fsz - (long)(4 + 4 + 4 + 24);
    if (version != 1 || vcount == 0 || vcount % 3 != 0 ||
        (long)vcount * (long)sizeof(RVertex) != remain) {
        snprintf(err, errlen, "mesh size mismatch (v=%u ver=%u remain=%ld)",
                 vcount, version, remain); fclose(f); return 1; }
    RVertex *v = (RVertex *)malloc((size_t)vcount * sizeof(RVertex));
    if (!v) { snprintf(err, errlen, "oom"); fclose(f); return 1; }
    if (fread(v, sizeof(RVertex), vcount, f) != vcount) {
        snprintf(err, errlen, "short read"); free(v); fclose(f); return 1; }
    fclose(f);
    *vout = v; *nout = vcount;
    memcpy(bbmin, bb, 12); memcpy(bbmax, bb + 3, 12);
    return 0;
}

/* Decode the LARGEST image from a TXD/RST container via libdtex (read-only). */
static int tex_load_largest(const char *path, unsigned char **rgba, int *w, int *h,
                            char *name, size_t namelen) {
    *rgba = NULL; *w = *h = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (n > 0) ? (uint8_t *)malloc((size_t)n) : NULL;
    int ok = (buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    if (!ok) { free(buf); return 1; }
    dtex_set set;
    int rc = dtex_decode_container(buf, (size_t)n, &set);
    free(buf);
    if (rc != 0 || set.count < 1) { if (rc == 0) dtex_free_set(&set); return 1; }
    int best = 0; long best_px = -1;
    for (int i = 0; i < set.count; i++) {
        long px = (long)set.items[i].width * set.items[i].height;
        if (px > best_px) { best_px = px; best = i; }
    }
    dtex_image *im = &set.items[best];
    *w = im->width; *h = im->height;
    *rgba = (unsigned char *)malloc((size_t)(*w) * (*h) * 4);
    if (!*rgba) { dtex_free_set(&set); return 1; }
    memcpy(*rgba, im->rgba, (size_t)(*w) * (*h) * 4);
    if (name && namelen) snprintf(name, namelen, "%s %dx%d", im->source, *w, *h);
    dtex_free_set(&set);
    return 0;
}

static void dirof(const char *path, char *dir, size_t n) {
    snprintf(dir, n, "%s", path);
    char *s = strrchr(dir, '/');
    if (s) *s = 0; else snprintf(dir, n, ".");
}

/* Hand-set character placement (identical math to render_scene_textured.c):
 * p' = R_yaw( s*(p-pivot) ) + (TX,FY,TZ); pivot = (char XZ centre, feet Y). */
static RVertex *place_character(const RVertex *src, size_t n,
                                const float smin[3], const float smax[3],
                                float s, float yaw_deg, float TX, float FY, float TZ,
                                float out_min[3], float out_max[3]) {
    RVertex *d = (RVertex *)malloc(n * sizeof(RVertex));
    if (!d) return NULL;
    float pivx = (smin[0] + smax[0]) * 0.5f;
    float pivz = (smin[2] + smax[2]) * 0.5f;
    float pivy = smin[1];
    float cy = cosf((float)(yaw_deg * M_PI / 180.0));
    float sy = sinf((float)(yaw_deg * M_PI / 180.0));
    for (int k = 0; k < 3; k++) { out_min[k] = 1e30f; out_max[k] = -1e30f; }
    for (size_t i = 0; i < n; i++) {
        const RVertex *v = &src[i];
        float px = (v->px - pivx) * s, py = (v->py - pivy) * s, pz = (v->pz - pivz) * s;
        float rx =  cy * px + sy * pz;
        float rz = -sy * px + cy * pz;
        float wx = rx + TX, wy = py + FY, wz = rz + TZ;
        float nx =  cy * v->nx + sy * v->nz;
        float nz = -sy * v->nx + cy * v->nz;
        d[i] = *v;
        d[i].px = wx; d[i].py = wy; d[i].pz = wz;
        d[i].nx = nx; d[i].nz = nz;
        float wp[3] = { wx, wy, wz };
        for (int k = 0; k < 3; k++) {
            if (wp[k] < out_min[k]) out_min[k] = wp[k];
            if (wp[k] > out_max[k]) out_max[k] = wp[k];
        }
    }
    return d;
}

/* =====================================================================
 * The capstone application state -- the shared context threaded through
 * the DocRenderer / DocFrameHooks / EngineHooks seams.
 * ===================================================================== */
typedef struct {
    /* --- GL + loaded scene (REAL decoded assets) --- */
    gfx_context    *gl;
    int             W, H;
    Group           env[MAXG];
    int             ng;
    RVertex        *placed;      /* placed character verts */
    size_t          cn;
    unsigned char  *ctex; int ctw, cth;
    float           cmin[3], cmax[3];   /* character world bbox = render focus */
    rgl_scene_draw  draws[MAXG + 1];
    int             ndraws;
    rgl_params      rp;

    /* --- engine camera (the native DocCamera is the authoritative view) --- */
    DocCamera       cam;
    DocLightSet     lights;
    float           center[3];   /* orbit centre = focus centre */
    float           radius;      /* focus half-diagonal */
    float           base_dist;   /* renderer tight-fit distance (dist_scale 1) */
    float           az_deg, el_deg, zoom;   /* current orbit, stepped by input */

    /* --- input --- */
    DocInputState   input;

    /* --- object list bindings (the scene-graph walk operates on these) --- */
    DocSceneObject    char_obj;
    DocObjectDrawInfo char_draw;

    /* --- plumbing --- */
    DocEngine      *engine;      /* back-ref so hooks can read scene_state */
    unsigned char  *pending_img; /* FBO readback set by submit, drained by present */
    int             frame_index; /* rendered-frame counter */
    const char     *outdir;
    FILE           *log;
    char            best_path[1024];
} CapstoneApp;

/* Script context for the engine state-machine hooks (below). */
typedef struct { uint32_t clock; CapstoneApp *app; } ScriptCtx;

/* --------------------------------------------------------------------- *
 * Camera framing math -- replicated EXACTLY from native/gpu renderer_gl.c
 * frame_camera_ex(), so the DocCamera eye we compute here is byte-for-byte
 * the eye the renderer reconstructs from (az,el,dist_scale). This is what
 * lets the DocCamera be the authoritative view while the read-only
 * renderer frames itself from its public (focus-box + orbit) entry point.
 * --------------------------------------------------------------------- */
static void framing_setup(CapstoneApp *a) {
    float ext[3];
    for (int k = 0; k < 3; k++) {
        a->center[k] = (a->cmin[k] + a->cmax[k]) * 0.5f;
        ext[k] = (a->cmax[k] - a->cmin[k]) * 0.5f;
    }
    float radius = sqrtf(ext[0]*ext[0] + ext[1]*ext[1] + ext[2]*ext[2]);
    if (radius < 1e-4f) radius = 1.0f;
    a->radius = radius;
    float fovy = DEG2RAD(45.0f);
    float aspect = (float)a->W / (float)a->H;
    float fit = fminf(fovy, 2.0f*atanf(tanf(fovy*0.5f)*aspect));
    a->base_dist = radius / sinf(fit * 0.5f) * 1.25f;  /* dist_scale == 1 */
}

/* eye on the orbit sphere for (az,el,zoom) -- the renderer's exact formula. */
static dvec3 orbit_eye(const CapstoneApp *a, float az_deg, float el_deg, float zoom) {
    float dist = a->base_dist * zoom;
    float az = DEG2RAD(az_deg), el = DEG2RAD(el_deg);
    return dvec3_make(a->center[0] + dist*cosf(el)*sinf(az),
                      a->center[1] + dist*sinf(el),
                      a->center[2] + dist*cosf(el)*cosf(az));
}

/* Invert a world eye back to (az,el,dist_scale) -- proves the orbit params
 * the renderer gets are derived FROM the DocCamera's eye, not vice-versa. */
static void eye_to_orbit(const CapstoneApp *a, dvec3 eye,
                         float *az_deg, float *el_deg, float *dist_scale) {
    float dx = eye.x - a->center[0], dy = eye.y - a->center[1], dz = eye.z - a->center[2];
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    *el_deg = RAD2DEG(asinf(dist > 1e-6f ? dy / dist : 0.0f));
    *az_deg = RAD2DEG(atan2f(dx, dz));
    *dist_scale = (a->base_dist > 1e-6f) ? dist / a->base_dist : 1.0f;
}

/* =====================================================================
 * DocInput synthetic backend (the purist replacement for the padman RPC):
 * port 0 connected, analog mode, right stick deflected +X so the camera
 * controller orbits. Port 1 disconnected. This is the SCRIPTED input.
 * ===================================================================== */
static int synth_read_pad(void *ctx, int port, uint8_t out[DOC_PAD_BUFFER_SIZE]) {
    (void)ctx;
    memset(out, 0, DOC_PAD_BUFFER_SIZE);
    if (port != 0) return 0;                 /* only port 0 connected */
    out[0x01] = DOC_PAD_MODE_ANALOG;         /* 0x79: sticks live */
    out[0x02] = 0xFF; out[0x03] = 0xFF;      /* active-low: no buttons pressed */
    out[0x04] = 0xC8;                        /* right stick X: deflected right */
    out[0x05] = 0x80;                        /* right stick Y: centred */
    out[0x06] = 0x80; out[0x07] = 0x80;      /* left stick centred */
    return 1;
}

/* =====================================================================
 * The camera controller: reads the decoded DocInput and steers the
 * DocCamera. This runs behind the frame loop's input_poll hook (it is the
 * per-frame input fold -> game state the loop does right after polling).
 * ===================================================================== */
static void camera_controller_step(CapstoneApp *a) {
    const DocInputState *in = &a->input;
    float ox = 0.0f, oy = 0.0f;
    int moved = 0;
    if (doc_input_held(in, 0, 0xFFFF) || in->pads[0].current.connected) {
        int dx = (int)in->pads[0].current.rstick_x - DOC_PAD_STICK_CENTRE;
        int dy = (int)in->pads[0].current.rstick_y - DOC_PAD_STICK_CENTRE;
        moved = doc_input_stick_normalize(dx, dy, &ox, &oy);
    }
    /* right stick X -> orbit azimuth; small fixed elevation drift for life */
    if (moved) a->az_deg += ox * 6.0f;
    a->el_deg = 12.0f + 3.0f * sinf(DEG2RAD(a->az_deg));

    /* place the DocCamera eye on the orbit and aim it at the scene centre.
     * doc_camera_set stores the eye and derives the Euler view orientation
     * (the real KerberosCamera.set path). The DocCamera is now the view. */
    dvec3 eye = orbit_eye(a, a->az_deg, a->el_deg, a->zoom);
    dvec3 ctr = dvec3_make(a->center[0], a->center[1], a->center[2]);
    dvec3 up  = dvec3_make(0.0f, 1.0f, 0.0f);
    doc_camera_set(&a->cam, eye, ctr, up);
}

/* =====================================================================
 * DocRenderer implementation -- the PURIST substitution for the PS2 GS DMA
 * transport bracket, backed by the REAL native GPU renderer.
 *   begin_frame  <- FUN_0030b768 open
 *   submit_scene <- inline GIF/GS emit  : draw char+env from the DocCamera
 *   end_frame    <- FUN_0030b868 kick
 *   present      <- FUN_0040ce28        : read the FBO to a PNG
 * ===================================================================== */
static void cap_begin(void *ctx) { (void)ctx; }
static void cap_end(void *ctx)   { (void)ctx; }

static void cap_submit(void *ctx, DocRenderQueue *q) {
    CapstoneApp *a = (CapstoneApp *)ctx;
    (void)q;   /* the object-list walk populated q just before this call */

    /* Derive the renderer's orbit params FROM the DocCamera's eye, and
     * confirm they reconstruct that exact eye (the camera drives the GPU). */
    float az, el, ds;
    eye_to_orbit(a, a->cam.eye, &az, &el, &ds);
    dvec3 re = orbit_eye(a, az, el, ds);
    float ex = re.x - a->cam.eye.x, ey = re.y - a->cam.eye.y, ez = re.z - a->cam.eye.z;
    float eyeerr = sqrtf(ex*ex + ey*ey + ez*ez);

    /* The native engine's own view+projection (PS2-convention) for the log. */
    dmat4 view = doc_camera_view_matrix(&a->cam);
    (void)doc_camera_projection_matrix(&a->cam);

    a->rp.angle_deg = az;
    a->rp.elev_deg  = el;

    unsigned char *img = NULL; char rerr[256] = {0};
    int rc = rgl_render_scene(a->draws, a->ndraws, a->cmin, a->cmax, ds,
                              &a->rp, &img, rerr, sizeof rerr);
    if (rc != 0 || !img) {
        fprintf(stderr, "submit_scene: GPU render failed: %s\n", rerr);
        a->pending_img = NULL;
        return;
    }
    a->pending_img = img;

    int32_t st = a->engine ? engine_get_scene_state(&a->engine->state) : -1;
    if (a->log) {
        fprintf(a->log,
            "  submit  state=0x%02x  cam.eye=(%.1f,%.1f,%.1f) look=(%.1f,%.1f,%.1f)\n"
            "          -> renderer az=%.1f el=%.1f dist_scale=%.3f  (eye-match err=%.4f)\n"
            "          view row0=[% .3f % .3f % .3f % .3f]\n",
            st, a->cam.eye.x, a->cam.eye.y, a->cam.eye.z,
            a->center[0], a->center[1], a->center[2],
            az, el, ds, eyeerr,
            view.m[0], view.m[4], view.m[8], view.m[12]);
    }
}

static void cap_present(void *ctx, int mode) {
    CapstoneApp *a = (CapstoneApp *)ctx;
    if (!a->pending_img) return;   /* e.g. the boot preamble present (mode 1) */
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%02d.png", a->outdir, a->frame_index);
    int rc = dtex_write_png(path, a->pending_img, a->W, a->H);
    if (rc == 0 && a->log)
        fprintf(a->log, "  present mode=%d -> %s\n", mode, path);
    else if (rc != 0)
        fprintf(stderr, "present: PNG write failed: %s\n", path);
    /* keep a mid-run frame as the hero to copy to the shared drive */
    if (rc == 0) snprintf(a->best_path, sizeof a->best_path, "%s", path);
    free(a->pending_img);
    a->pending_img = NULL;
    a->frame_index++;
}

static int cap_pending(void *ctx) {
    CapstoneApp *a = (CapstoneApp *)ctx;
    return a->pending_img != NULL;
}

/* =====================================================================
 * Frame-loop below-seam hooks. input_poll folds the pad + steers the
 * camera; input_latch flushes rumble; the rest are the documented no-op
 * subsystem hooks (each a real subsystem pending its own port).
 * ===================================================================== */
static void cap_input_poll(void *ctx) {
    CapstoneApp *a = (CapstoneApp *)ctx;
    doc_input_begin(&a->input);        /* FUN_002548d8 -> decode both pads */
    camera_controller_step(a);         /* fold input -> DocCamera this frame */
}
static void cap_input_latch(void *ctx) {
    CapstoneApp *a = (CapstoneApp *)ctx;
    doc_input_latch(&a->input);        /* FUN_002548f8 -> actuator flush */
}
static void cap_v1(void *c, int a) { (void)c; (void)a; }
static void cap_sub(void *c, int t, int a) { (void)c; (void)t; (void)a; }
static int  cap_q0(void *c) { (void)c; return 0; }

/* =====================================================================
 * Engine state-machine hooks (EngineHooks). SCRIPTED poll results that
 * drive a faithful boot arc BOOT -> TITLE -> TITLE_HOLD -> ATTRACT ->
 * LOAD_DISPATCH -> FIELD_SETUP -> FIELD_LOAD -> FIELD_RUN and then hold in
 * FIELD_RUN (in-level). The dispatch logic itself is the REAL ported code;
 * only the below-seam poll values are scripted (the real values come from
 * the per-scene KEL-VM bytecode, a documented engine gap).
 * ===================================================================== */
static int sc_title_boot_poll(void *c) { (void)c; return 1; }       /* BOOT -> TITLE */
static int sc_clock_now(void *c) { ScriptCtx *s = c; s->clock += 1000u; return (int)s->clock; }
static int sc_save_slot_check(void *c) { (void)c; return 0; }
static int sc_boot_target_resolve(void *c) { (void)c; return 3; }   /* non-0, not 9 */
static int sc_alt_boot_poll(void *c) { (void)c; return 1; }         /* TITLE -> submode 2 */
static int sc_input_test(void *c, int id) { (void)c; (void)id; return 0; }
static int sc_field_enter_poll(void *c) { (void)c; return 0; }      /* >=0: make field */
static int sc_field_cam(void *c, int i) { (void)c; (void)i; return 0; }
static int sc_res_param(void *c, int id) { (void)c; (void)id; return 0; }
static int sc_field_result_make(void *c, int a, int b, int d, int e) {
    (void)c; (void)a; (void)b; (void)d; (void)e; return 0x1000; }   /* field handle */
static int sc_field_run_poll(void *c, int h) { (void)c; (void)h; return 1; }   /* -> FIELD_RUN */
static int sc_field_exit_poll(void *c, int h) { (void)c; (void)h; return 0; }  /* stay */
static int sc_title_return_poll(void *c, int arg) { (void)c; (void)arg; return 1; } /* ATTRACT -> LOAD */

static EngineHooks make_dispatch_hooks(ScriptCtx *sc) {
    EngineHooks h = engine_hooks_default();
    h.ctx = sc;
    h.title_boot_poll     = sc_title_boot_poll;
    h.clock_now           = sc_clock_now;
    h.save_slot_check     = sc_save_slot_check;
    h.boot_target_resolve = sc_boot_target_resolve;
    h.alt_boot_poll       = sc_alt_boot_poll;
    h.input_test          = sc_input_test;
    h.field_enter_poll    = sc_field_enter_poll;
    h.field_cam           = sc_field_cam;
    h.res_param           = sc_res_param;
    h.field_result_make   = sc_field_result_make;
    h.field_run_poll      = sc_field_run_poll;
    h.field_exit_poll     = sc_field_exit_poll;
    h.title_return_poll   = sc_title_return_poll;
    return h;   /* all other polls stay at the all-safe 0 default */
}

static const char *state_name(int32_t s) {
    switch (s) {
        case SCENE_STATE_BOOT:          return "BOOT";
        case SCENE_STATE_TITLE:         return "TITLE";
        case SCENE_STATE_TITLE_SUB:     return "TITLE_SUB";
        case SCENE_STATE_LOAD_DISPATCH: return "LOAD_DISPATCH";
        case SCENE_STATE_TITLE_HOLD:    return "TITLE_HOLD";
        case SCENE_STATE_FIELD_SETUP:   return "FIELD_SETUP";
        case SCENE_STATE_FIELD_LOAD:    return "FIELD_LOAD";
        case SCENE_STATE_FIELD_RUN:     return "FIELD_RUN";
        case SCENE_STATE_ATTRACT:       return "ATTRACT";
        default:                        return "other";
    }
}

/* =====================================================================
 * main
 * ===================================================================== */
int main(int argc, char **argv) {
    const char *listpath = "../assets/models/out/bmd_06960212_tex.list";
    const char *charpath = "../assets/models/out/char_e000.mesh";
    const char *chartex  = "/srv/games/dirge-port/extract/_KEL/noPath/07650002.bin";
    const char *outdir   = "out";
    int nframes = 9;
    /* hero placement (TUNED values from render_scene_textured.c) */
    float scale = 2.6f, yaw = 160.f, TX = 268.f, FY = 0.f, TZ = 330.f;
    float colscale = 1.8f;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-o") && i+1 < argc) outdir  = argv[++i];
        else if (!strcmp(argv[i], "-n") && i+1 < argc) nframes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i+1 < argc) listpath = argv[++i];
        else if (!strcmp(argv[i], "-m") && i+1 < argc) charpath = argv[++i];
        else if (!strcmp(argv[i], "-t") && i+1 < argc) chartex  = argv[++i];
    }
    if (nframes < 1) nframes = 1;
    if (nframes > 64) nframes = 64;

    CapstoneApp app;
    memset(&app, 0, sizeof app);
    app.W = 1024; app.H = 768;
    app.outdir = outdir;
    app.az_deg = 20.0f; app.el_deg = 12.0f; app.zoom = 1.9f;

    /* ---- load the TEXTURED environment groups (REAL decoded room) ---- */
    char ldir[1024]; dirof(listpath, ldir, sizeof ldir);
    FILE *lf = fopen(listpath, "r");
    if (!lf) { fprintf(stderr, "cannot open list %s\n", listpath); return 2; }
    char line[2048];
    while (fgets(line, sizeof line, lf) && app.ng < MAXG) {
        char meshf[1024] = {0}, texf[1024] = {0}; int abe = 0;
        if (sscanf(line, "%1023s\t%1023s\t%d", meshf, texf, &abe) < 1) continue;
        if (meshf[0] == 0 || meshf[0] == '#') continue;
        char meshpath[2048];
        snprintf(meshpath, sizeof meshpath, "%s/%s", ldir, meshf);
        char err[256] = {0};
        RVertex *v = NULL; size_t n = 0; float bbmn[3], bbmx[3];
        if (mesh_load(meshpath, &v, &n, bbmn, bbmx, err, sizeof err)) {
            fprintf(stderr, "skip %s: %s\n", meshf, err); continue; }
        Group *gg = &app.env[app.ng];
        gg->v = v; gg->n = n;
        memcpy(gg->bbmin, bbmn, 12); memcpy(gg->bbmax, bbmx, 12);
        gg->tex = NULL; gg->tw = gg->th = 0; gg->texname[0] = 0;
        if (strcmp(texf, "NONE") != 0)
            tex_load_largest(texf, &gg->tex, &gg->tw, &gg->th, gg->texname, sizeof gg->texname);
        app.ng++;
    }
    fclose(lf);
    if (app.ng == 0) { fprintf(stderr, "no env groups loaded\n"); return 2; }

    /* env PRELIT display gain (documented exposure lift; character untouched) */
    for (int i = 0; i < app.ng; i++)
        for (size_t k = 0; k < app.env[i].n; k++) {
            float r = app.env[i].v[k].r * colscale;
            float g = app.env[i].v[k].g * colscale;
            float b = app.env[i].v[k].b * colscale;
            app.env[i].v[k].r = r > 1.f ? 1.f : r;
            app.env[i].v[k].g = g > 1.f ? 1.f : g;
            app.env[i].v[k].b = b > 1.f ? 1.f : b;
        }

    /* ---- load + place the character (REAL posed+skinned actor) ---- */
    char err[256] = {0};
    RVertex *cv = NULL; size_t cn = 0; float cbmn[3], cbmx[3];
    if (mesh_load(charpath, &cv, &cn, cbmn, cbmx, err, sizeof err)) {
        fprintf(stderr, "char: %s\n", err); return 2; }
    app.placed = place_character(cv, cn, cbmn, cbmx, scale, yaw, TX, FY, TZ,
                                 app.cmin, app.cmax);
    app.cn = cn;
    free(cv);
    if (!app.placed) { fprintf(stderr, "placement alloc failed\n"); return 3; }

    /* ---- GL context (EGL offscreen) ---- */
    char cerr[256] = {0};
    app.gl = gfx_context_create(app.W, app.H, cerr, sizeof cerr);
    if (!app.gl) { fprintf(stderr, "GL init failed: %s\n", cerr); return 4; }

    if (tex_load_largest(chartex, &app.ctex, &app.ctw, &app.cth, NULL, 0))
        fprintf(stderr, "warning: char texture decode failed (%s); char untextured\n", chartex);

    /* ---- build the draw list: env groups PRELIT + char LIT ---- */
    memset(&app.rp, 0, sizeof app.rp);
    app.rp.width = app.W; app.rp.height = app.H; app.rp.bilinear = 1;
    app.rp.backface_cull = 0; app.rp.rig = rgl_default_lightrig(); app.rp.alpha_ref = -1.0f;
    for (int i = 0; i < app.ng; i++) {
        app.draws[i].verts = app.env[i].v; app.draws[i].vcount = app.env[i].n;
        app.draws[i].shade_mode = RGL_SHADE_PRELIT; app.draws[i].tfx = RGL_TFX_MODULATE;
        app.draws[i].tex_rgba = app.env[i].tex;
        app.draws[i].tex_w = app.env[i].tw; app.draws[i].tex_h = app.env[i].th;
    }
    app.draws[app.ng].verts = app.placed; app.draws[app.ng].vcount = app.cn;
    app.draws[app.ng].shade_mode = RGL_SHADE_LIT; app.draws[app.ng].tfx = RGL_TFX_MODULATE;
    app.draws[app.ng].tex_rgba = app.ctex; app.draws[app.ng].tex_w = app.ctw;
    app.draws[app.ng].tex_h = app.cth;
    app.ndraws = app.ng + 1;

    framing_setup(&app);

    /* ---- the native DocCamera: the authoritative engine view ---- */
    doc_camera_init(&app.cam);
    doc_camera_set_fov(&app.cam, DEG2RAD(45.0f));   /* match the GL renderer fovy */
    app.cam.screen_w = (float)app.W; app.cam.screen_h = (float)app.H;
    app.cam.znear = 1.0f; app.cam.zfar = app.base_dist + app.radius * 4.0f;

    /* ---- the native DocLightSet (engine defaults; real light-state API) ---- */
    doc_light_init(&app.lights);
    doc_light_set_ambient(&app.lights, 24, 24, 30);
    doc_light_commit(&app.lights);

    /* ---- DocInput with the scripted synthetic backend ---- */
    DocInputBackend be; memset(&be, 0, sizeof be);
    be.read_pad = synth_read_pad; be.set_actuator = NULL;
    doc_input_init(&app.input, &be);

    /* ---- object list: register the character as a visible scene object ---- */
    app.char_draw.flags = DOC_OBJ_VISIBLE | DOC_OBJ_LIST_ACTIVE | (1u << 16); /* draw mode 1 */
    memset(&app.char_obj, 0, sizeof app.char_obj);
    app.char_obj.name = "char_e000";
    app.char_obj.draw_info = &app.char_draw;
    app.char_obj.next = NULL;
    doc_scene_object_list_set_head(&app.char_obj);
    doc_scene_object_list_set_enabled(1);

    /* ---- the render queue (the 0x4ee2e0 draw context submitted each frame) ---- */
    DocRenderQueue rq; memset(&rq, 0, sizeof rq);
    rq.id = 0x4ee2e0; rq.name = "main";

    /* ---- wire the three hook tables ---- */
    ScriptCtx sc; sc.clock = 0; sc.app = &app;
    EngineHooks dispatch_hooks = make_dispatch_hooks(&sc);

    DocRenderer renderer = doc_renderer_default();
    renderer.ctx = &app;
    renderer.begin_frame   = cap_begin;
    renderer.submit_scene  = cap_submit;
    renderer.end_frame     = cap_end;
    renderer.present        = cap_present;
    renderer.pending_query = cap_pending;

    DocFrameHooks fh = doc_frame_hooks_default();  /* run_state_dispatch / run_object_list = REAL ported code */
    fh.ctx          = &app;
    fh.input_poll   = cap_input_poll;
    fh.input_latch  = cap_input_latch;
    fh.frame_lock   = cap_v1;
    fh.frame_unlock = cap_v1;
    fh.subsystem    = cap_sub;
    fh.boot_cond_query   = cap_q0;
    fh.render_sync_query = cap_q0;
    fh.per_frame_hook    = cap_q0;
    fh.exit_query        = cap_q0;
    /* frame_advance / run_state_dispatch / run_object_list keep the library
     * defaults set by doc_frame_hooks_default() above (the last two invoke
     * the REAL ported engine_state_dispatch / doc_scene_object_list_update). */

    /* ---- the engine ---- */
    DocEngine engine;
    doc_engine_init(&engine, &dispatch_hooks, &renderer, &fh, &rq);
    engine.state.scene_state = SCENE_STATE_BOOT;   /* start at boot */
    app.engine = &engine;

    /* ---- run log ---- */
    char logpath[1024];
    snprintf(logpath, sizeof logpath, "%s/run.log", outdir);
    app.log = fopen(logpath, "w");
    FILE *L = app.log ? app.log : stderr;

    fprintf(L, "=== Dirge of Cerberus NATIVE CAPSTONE: engine loop drives GPU render ===\n");
    fprintf(L, "scene : char_e000 (LIT, %zu tris) in room 06960212 (%d PRELIT groups)\n",
            app.cn/3, app.ng);
    fprintf(L, "focus : centre=(%.1f,%.1f,%.1f) radius=%.1f base_dist=%.1f  out=%dx%d\n",
            app.center[0], app.center[1], app.center[2], app.radius, app.base_dist,
            app.W, app.H);
    fprintf(L, "gl    : %s\n", gfx_context_describe(app.gl));
    fprintf(L, "lights: 3 directional + ambient (DocLightSet, engine defaults; rig=placeholder)\n");
    fprintf(L, "-----------------------------------------------------------------------\n");

    /* ---- the FUN_00248110 preamble, then the per-frame loop ---- */
    doc_engine_frame_preamble(&engine);   /* present(mode 1): no scene yet, skipped */

    int ran = 0;
    for (int i = 0; i < nframes; i++) {
        int32_t pre = engine_get_scene_state(&engine.state);
        fprintf(L, "frame %02d: state=%-13s (0x%02x) az=%.1f\n",
                i, state_name(pre), pre, app.az_deg);
        DocFrameResult r = doc_engine_frame(&engine);   /* <-- drives GPU this frame */
        int32_t post = engine_get_scene_state(&engine.state);
        if (post != pre)
            fprintf(L, "         state advanced %s -> %s\n", state_name(pre), state_name(post));
        ran++;
        if (r == DOC_FRAME_STOP) { fprintf(L, "         loop exit fired\n"); break; }
    }

    fprintf(L, "-----------------------------------------------------------------------\n");
    fprintf(L, "ran %d engine frames; %d frames rendered to PNG; engine.frame_counter=%d\n",
            ran, app.frame_index, engine.frame_counter);
    fprintf(L, "object-list walk: enabled=%d head=%s char draw_mode=%u\n",
            doc_scene_object_list_enabled(),
            doc_scene_object_list_head() ? doc_scene_object_list_head()->name : "(null)",
            doc_scene_object_draw_mode(&app.char_obj));

    if (app.log) fclose(app.log);

    /* also echo a tail of the log so the console shows the loop advancing */
    printf("capstone: ran %d engine frames, %d PNGs in %s/ (log: %s)\n",
           ran, app.frame_index, outdir, logpath);

    /* ---- cleanup ---- */
    free(app.ctex); free(app.placed);
    for (int i = 0; i < app.ng; i++) { free(app.env[i].v); free(app.env[i].tex); }
    gfx_context_destroy(app.gl);
    return (app.frame_index > 0) ? 0 : 1;
}
