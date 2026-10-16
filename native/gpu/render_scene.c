/* render_scene.c -- compose a real DoC SCENE on the native GPU: the decoded
 * character (char_e000 / Vincent) standing inside a decoded BMD environment
 * (a multi-floor gallery room), both real native assets, drawn by our own
 * OpenGL 3.3-core renderer through the PURIST RENDER-MATH shader pair, via the
 * EGL-offscreen path -> PNG.
 *
 *   ENVIRONMENT  bmd_06960212.mesh  (DMSH) -- static world geometry, rendered
 *                PRELIT. Its baked PRELIT vertex colours and UVs are not yet
 *                parsed from the BMD VIF stream, so export_bmd_mesh.py bakes a
 *                reconstructed grey directional shade into the prelit colour as
 *                a DOCUMENTED STAND-IN so the room reads as 3D geometry.
 *   CHARACTER    char_e000.mesh     (DMSH) -- posed+skinned actor geometry,
 *                rendered LIT (3 directional lights + ambient) + body texture.
 *
 * The two meshes DO NOT share an authored coordinate frame (we don't have the
 * real scene-placement data: the MOD actor is in bone-local units ~20 tall; the
 * BMD room is in world float units ~150 tall). The character placement here is
 * therefore a HAND-SET transform (uniform scale + yaw + translate), documented
 * as such, chosen so the character reads as a person standing on the room floor
 * at a plausible relative scale. Both end up in ONE GL world, one shared depth
 * buffer, one camera -> they depth-sort correctly.
 *
 *   render_scene [-e ENV.mesh] [-m CHAR.mesh] [-t TEX_CONTAINER] [-o OUTDIR]
 *                [-s scale] [-x cx] [-y feetY] [-z cz] [-yaw deg]
 */
#include "dtex.h"
#include "gfx_context.h"
#include "renderer_gl.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------- mesh load ---- */
typedef struct { RVertex *v; size_t n; float bbmin[3], bbmax[3]; } Mesh;

/* Load a "DMSH" triangle soup. Bounds-checked: the file is untrusted. */
static int mesh_load(const char *path, Mesh *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open mesh %s", path); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 0 || (size_t)fsz < 4 + 4 + 4 + 24) {
        snprintf(err, errlen, "mesh too small (%ld bytes)", fsz); fclose(f); return 1;
    }
    char magic[4]; uint32_t version = 0, vcount = 0; float bb[6];
    int ok = fread(magic, 1, 4, f) == 4
          && fread(&version, 4, 1, f) == 1
          && fread(&vcount, 4, 1, f) == 1
          && fread(bb, 4, 6, f) == 6;
    if (!ok || memcmp(magic, "DMSH", 4) != 0) {
        snprintf(err, errlen, "bad mesh header/magic in %s", path); fclose(f); return 1;
    }
    long remain = fsz - (long)(4 + 4 + 4 + 24);
    if (version != 1 || vcount == 0 || vcount % 3 != 0 ||
        (long)vcount * (long)sizeof(RVertex) != remain) {
        snprintf(err, errlen, "mesh size mismatch (v=%u ver=%u remain=%ld)",
                 vcount, version, remain);
        fclose(f); return 1;
    }
    RVertex *v = (RVertex *)malloc((size_t)vcount * sizeof(RVertex));
    if (!v) { snprintf(err, errlen, "oom %u verts", vcount); fclose(f); return 1; }
    if (fread(v, sizeof(RVertex), vcount, f) != vcount) {
        snprintf(err, errlen, "short read of vertex data"); free(v); fclose(f); return 1;
    }
    fclose(f);
    out->v = v; out->n = vcount;
    memcpy(out->bbmin, bb, 12); memcpy(out->bbmax, bb + 3, 12);
    return 0;
}

/* --------------------------------------------------------- texture decode --- */
/* Decode the body atlas via libdtex (read-only) and keep the LARGEST image. */
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

/* --------------------------------------------- hand-set character placement -
 * p' = R_yaw( s * (p - pivot) ) + (TX, FY, TZ)
 * where pivot = (char X/Z centre, char feet Y). So feet land exactly on FY and
 * the horizontal centre lands at (TX,TZ). Normals are yaw-rotated (uniform
 * positive scale preserves them). Returns a NEW RVertex array + its world bbox. */
static RVertex *place_character(const Mesh *src, float s, float yaw_deg,
                                float TX, float FY, float TZ,
                                float out_min[3], float out_max[3]) {
    RVertex *d = (RVertex *)malloc(src->n * sizeof(RVertex));
    if (!d) return NULL;
    float pivx = (src->bbmin[0] + src->bbmax[0]) * 0.5f;
    float pivz = (src->bbmin[2] + src->bbmax[2]) * 0.5f;
    float pivy = src->bbmin[1];                 /* feet */
    float cy = cosf((float)(yaw_deg * M_PI / 180.0));
    float sy = sinf((float)(yaw_deg * M_PI / 180.0));
    for (int k = 0; k < 3; k++) { out_min[k] = 1e30f; out_max[k] = -1e30f; }
    for (size_t i = 0; i < src->n; i++) {
        const RVertex *v = &src->v[i];
        float px = (v->px - pivx) * s;
        float py = (v->py - pivy) * s;
        float pz = (v->pz - pivz) * s;
        float rx =  cy * px + sy * pz;          /* yaw about Y */
        float rz = -sy * px + cy * pz;
        float wx = rx + TX, wy = py + FY, wz = rz + TZ;
        float nx =  cy * v->nx + sy * v->nz;
        float nz = -sy * v->nx + cy * v->nz;
        d[i] = *v;
        d[i].px = wx; d[i].py = wy; d[i].pz = wz;
        d[i].nx = nx; d[i].nz = nz;             /* ny unchanged */
        float wp[3] = { wx, wy, wz };
        for (int k = 0; k < 3; k++) {
            if (wp[k] < out_min[k]) out_min[k] = wp[k];
            if (wp[k] > out_max[k]) out_max[k] = wp[k];
        }
    }
    return d;
}

int main(int argc, char **argv) {
    const char *envpath  = "../assets/models/out/bmd_06960212.mesh";
    const char *charpath = "../assets/models/out/char_e000.mesh";
    const char *tex_container = "/srv/games/dirge-port/extract/_KEL/noPath/07650002.bin";
    const char *outdir = "out";
    /* hand-set placement defaults (see header + validation notes) */
    /* Stand the character on the middle floor slab (Y=0) near the OPEN +X/+Z
     * corner of the pillared pavilion, so the camera sees it under the roof
     * overhang with the interior partitions behind it as a backdrop. */
    float scale = 2.6f, yaw = 40.f, TX = 268.f, FY = 0.f, TZ = 330.f;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-e")  && i+1<argc) envpath  = argv[++i];
        else if (!strcmp(argv[i], "-m")  && i+1<argc) charpath = argv[++i];
        else if (!strcmp(argv[i], "-t")  && i+1<argc) tex_container = argv[++i];
        else if (!strcmp(argv[i], "-o")  && i+1<argc) outdir  = argv[++i];
        else if (!strcmp(argv[i], "-s")  && i+1<argc) scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-yaw")&& i+1<argc) yaw   = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-x")  && i+1<argc) TX    = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-y")  && i+1<argc) FY    = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-z")  && i+1<argc) TZ    = (float)atof(argv[++i]);
    }

    char err[256] = {0};
    Mesh env, chr;
    if (mesh_load(envpath, &env, err, sizeof err)) { fprintf(stderr, "env: %s\n", err); return 2; }
    if (mesh_load(charpath, &chr, err, sizeof err)) { fprintf(stderr, "char: %s\n", err); free(env.v); return 2; }
    printf("env   : %s  (%zu tris)  X[%.1f,%.1f] Y[%.1f,%.1f] Z[%.1f,%.1f]\n",
           envpath, env.n/3, env.bbmin[0],env.bbmax[0],env.bbmin[1],env.bbmax[1],env.bbmin[2],env.bbmax[2]);
    printf("char  : %s  (%zu tris)  raw height %.2f\n",
           charpath, chr.n/3, chr.bbmax[1]-chr.bbmin[1]);

    /* place the character (hand-set transform) */
    float cmin[3], cmax[3];
    RVertex *placed = place_character(&chr, scale, yaw, TX, FY, TZ, cmin, cmax);
    if (!placed) { fprintf(stderr, "placement alloc failed\n"); free(env.v); free(chr.v); return 3; }
    printf("placed: scale=%.2f yaw=%.0f feet=(%.1f,%.1f,%.1f)  bbox X[%.1f,%.1f] Y[%.1f,%.1f] Z[%.1f,%.1f]\n",
           scale, yaw, TX, FY, TZ, cmin[0],cmax[0],cmin[1],cmax[1],cmin[2],cmax[2]);

    const int W = 1280, H = 960;
    char cerr[256] = {0};
    gfx_context *ctx = gfx_context_create(W, H, cerr, sizeof cerr);
    if (!ctx) { fprintf(stderr, "GL init failed: %s\n", cerr); free(placed); free(env.v); free(chr.v); return 4; }
    printf("gl    : %s\n", gfx_context_describe(ctx));

    unsigned char *tex = NULL; int tw = 0, th = 0; char tname[64] = {0};
    if (tex_load_largest(tex_container, &tex, &tw, &th, tname, sizeof tname))
        fprintf(stderr, "warning: texture decode failed (%s); character untextured\n", tex_container);
    else
        printf("tex   : %s  (%s)\n", tex_container, tname);

    rgl_params p; memset(&p, 0, sizeof p);
    p.width = W; p.height = H; p.bilinear = 1; p.backface_cull = 0;
    p.rig = rgl_default_lightrig(); p.alpha_ref = -1.0f;

    /* two scene draws: environment PRELIT, character LIT + textured */
    rgl_scene_draw draws[2];
    memset(draws, 0, sizeof draws);
    draws[0].verts = env.v;   draws[0].vcount = env.n;
    draws[0].shade_mode = RGL_SHADE_PRELIT; draws[0].tfx = RGL_TFX_MODULATE;
    draws[1].verts = placed;  draws[1].vcount = chr.n;
    draws[1].shade_mode = RGL_SHADE_LIT;    draws[1].tfx = RGL_TFX_MODULATE;
    draws[1].tex_rgba = tex;  draws[1].tex_w = tw; draws[1].tex_h = th;

    /* frame on the character but pull the eye back to show the room around it */
    struct { const char *tag; float az, el, zoom; } views[] = {
        { "hero",   44.f, 13.f, 2.1f },
        { "corner", 22.f, 11.f, 2.2f },
        { "wide",   52.f, 20.f, 3.0f },
    };

    char path[1024];
    int fail = 0;
    for (size_t i = 0; i < sizeof views/sizeof views[0]; i++) {
        p.angle_deg = views[i].az; p.elev_deg = views[i].el;
        unsigned char *img = NULL; char rerr[256] = {0};
        int rc = rgl_render_scene(draws, 2, cmin, cmax, views[i].zoom, &p, &img, rerr, sizeof rerr);
        if (rc != 0 || !img) { fprintf(stderr, "scene render (%s) failed: %s\n", views[i].tag, rerr); fail = 1; continue; }
        snprintf(path, sizeof path, "%s/scene_char_in_env_%s.png", outdir, views[i].tag);
        rc = dtex_write_png(path, img, W, H);
        free(img);
        if (rc != 0) { fprintf(stderr, "PNG write failed: %s\n", path); fail = 1; continue; }
        printf("wrote %-40s  (az=%.0f el=%.0f zoom=%.1f)\n", path, views[i].az, views[i].el, views[i].zoom);
    }

    free(tex); free(placed); free(env.v); free(chr.v);
    gfx_context_destroy(ctx);
    return fail ? 1 : 0;
}
