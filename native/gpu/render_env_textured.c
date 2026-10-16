/* render_env_textured.c -- render a TEXTURED Dirge of Cerberus BMD ENVIRONMENT
 * on the native GPU: the decoded room geometry with its REAL per-vertex UVs,
 * REAL baked PRELIT vertex colours, and the room's REAL TXD textures, drawn
 * through the existing purist RENDER-MATH shader (PRELIT + GS MODULATE) via the
 * EGL-offscreen path -> PNG.
 *
 * Input is the `.list` sidecar emitted by ../assets/models/export_bmd_textured.py:
 * one line per MATERIAL GROUP:
 *     <mesh.mesh> \t <texture_container.bin|NONE> \t <abe 0/1>
 * Each group mesh is a DMSH triangle soup (pos3,nrm3,uv2,rgba4) carrying the
 * decoded UVs + colours; each texture is decoded from its TXD container by
 * libdtex (largest image), exactly like render_scene.c. Every group is one
 * rgl_scene_draw (PRELIT + MODULATE + its own texture) in a single GL world, so
 * the whole textured room draws against one depth buffer and one camera.
 *
 * This is additive: it links the existing renderer_gl/gl_api/gfx_context/libdtex
 * read-only and does NOT modify them.
 *
 *   render_env_textured [-l LIST] [-o OUTDIR] [-flipy] [-notex]
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

#define MAXG 64

typedef struct { RVertex *v; size_t n; float bbmin[3], bbmax[3];
                 unsigned char *tex; int tw, th; int abe; char texname[64]; } Group;

/* Load a "DMSH" triangle soup (same bounds-checked format as render_scene). */
static int mesh_load(const char *path, RVertex **vout, size_t *nout,
                     float bbmin[3], float bbmax[3], char *err, size_t errlen) {
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
        snprintf(err, errlen, "mesh size mismatch (v=%u)", vcount); fclose(f); return 1; }
    RVertex *v = (RVertex *)malloc((size_t)vcount * sizeof(RVertex));
    if (!v) { snprintf(err, errlen, "oom"); fclose(f); return 1; }
    if (fread(v, sizeof(RVertex), vcount, f) != vcount) {
        snprintf(err, errlen, "short read"); free(v); fclose(f); return 1; }
    fclose(f);
    *vout = v; *nout = vcount;
    memcpy(bbmin, bb, 12); memcpy(bbmax, bb + 3, 12);
    return 0;
}

/* Decode the LARGEST image from a TXD container via libdtex (read-only). */
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

/* dir(list) so relative mesh basenames resolve next to the .list file. */
static void dirof(const char *path, char *dir, size_t n) {
    snprintf(dir, n, "%s", path);
    char *s = strrchr(dir, '/');
    if (s) *s = 0; else snprintf(dir, n, ".");
}

int main(int argc, char **argv) {
    const char *listpath = "../assets/models/out/bmd_06960212_tex.list";
    const char *outdir = "out";
    const char *tag = "19_environment_textured";
    int flipy = 0, notex = 0, neutralcol = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-l") && i+1<argc) listpath = argv[++i];
        else if (!strcmp(argv[i], "-o") && i+1<argc) outdir  = argv[++i];
        else if (!strcmp(argv[i], "-tag") && i+1<argc) tag   = argv[++i];
        else if (!strcmp(argv[i], "-flipy")) flipy = 1;
        else if (!strcmp(argv[i], "-notex")) notex = 1;
        else if (!strcmp(argv[i], "-neutralcol")) neutralcol = 1;  /* show raw tex */
    }

    char ldir[1024]; dirof(listpath, ldir, sizeof ldir);
    FILE *lf = fopen(listpath, "r");
    if (!lf) { fprintf(stderr, "cannot open list %s\n", listpath); return 2; }

    Group g[MAXG]; int ng = 0;
    float rmin[3] = { 1e30f, 1e30f, 1e30f }, rmax[3] = { -1e30f, -1e30f, -1e30f };
    char line[2048];
    while (fgets(line, sizeof line, lf) && ng < MAXG) {
        char meshf[1024] = {0}, texf[1024] = {0}; int abe = 0;
        if (sscanf(line, "%1023s\t%1023s\t%d", meshf, texf, &abe) < 1) continue;
        if (meshf[0] == 0 || meshf[0] == '#') continue;
        char meshpath[2048];
        snprintf(meshpath, sizeof meshpath, "%s/%s", ldir, meshf);
        char err[256] = {0};
        RVertex *v = NULL; size_t n = 0; float bbmn[3], bbmx[3];
        if (mesh_load(meshpath, &v, &n, bbmn, bbmx, err, sizeof err)) {
            fprintf(stderr, "skip %s: %s\n", meshf, err); continue;
        }
        g[ng].v = v; g[ng].n = n; g[ng].abe = abe;
        memcpy(g[ng].bbmin, bbmn, 12); memcpy(g[ng].bbmax, bbmx, 12);
        g[ng].tex = NULL; g[ng].tw = g[ng].th = 0; g[ng].texname[0] = 0;
        if (!notex && strcmp(texf, "NONE") != 0) {
            if (tex_load_largest(texf, &g[ng].tex, &g[ng].tw, &g[ng].th,
                                 g[ng].texname, sizeof g[ng].texname))
                fprintf(stderr, "warn: tex decode failed for group %d (%s)\n", ng, texf);
        }
        for (int k = 0; k < 3; k++) {
            if (bbmn[k] < rmin[k]) rmin[k] = bbmn[k];
            if (bbmx[k] > rmax[k]) rmax[k] = bbmx[k];
        }
        ng++;
    }
    fclose(lf);
    if (ng == 0) { fprintf(stderr, "no groups loaded\n"); return 2; }

    /* optional: force neutral (0.5 -> x2 = 1.0) vertex colour so the DECODED
     * textures show at full brightness (the baked PRELIT colours are a dark
     * interior; this variant isolates the texture+UV decode for inspection). */
    if (neutralcol) {
        for (int i = 0; i < ng; i++)
            for (size_t k = 0; k < g[i].n; k++) {
                g[i].v[k].r = g[i].v[k].g = g[i].v[k].b = 0.5f; g[i].v[k].a = 0.5f;
            }
    }
    /* optional Y flip so +Y-up camera shows the room upright */
    if (flipy) {
        for (int i = 0; i < ng; i++)
            for (size_t k = 0; k < g[i].n; k++) { g[i].v[k].py = -g[i].v[k].py; g[i].v[k].ny = -g[i].v[k].ny; }
        float t = rmin[1]; rmin[1] = -rmax[1]; rmax[1] = -t;
    }

    size_t tottri = 0; for (int i = 0; i < ng; i++) tottri += g[i].n / 3;
    printf("groups=%d  total tris=%zu  room bbox X[%.1f,%.1f] Y[%.1f,%.1f] Z[%.1f,%.1f]\n",
           ng, tottri, rmin[0], rmax[0], rmin[1], rmax[1], rmin[2], rmax[2]);

    const int W = 1280, H = 960;
    char cerr[256] = {0};
    gfx_context *ctx = gfx_context_create(W, H, cerr, sizeof cerr);
    if (!ctx) { fprintf(stderr, "GL init failed: %s\n", cerr); return 4; }
    printf("gl    : %s\n", gfx_context_describe(ctx));

    rgl_params p; memset(&p, 0, sizeof p);
    p.width = W; p.height = H; p.bilinear = 1; p.backface_cull = 0;
    p.rig = rgl_default_lightrig(); p.alpha_ref = -1.0f;

    rgl_scene_draw draws[MAXG];
    memset(draws, 0, sizeof draws);
    for (int i = 0; i < ng; i++) {
        draws[i].verts = g[i].v; draws[i].vcount = g[i].n;
        draws[i].shade_mode = RGL_SHADE_PRELIT; draws[i].tfx = RGL_TFX_MODULATE;
        draws[i].tex_rgba = g[i].tex; draws[i].tex_w = g[i].tw; draws[i].tex_h = g[i].th;
    }

    struct { const char *name; float az, el, zoom; } views[] = {
        { "hero",   40.f, 18.f, 1.25f },
        { "wide",   62.f, 26.f, 1.55f },
        { "low",    18.f,  9.f, 1.30f },
    };
    char path[1024]; int fail = 0;
    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        p.angle_deg = views[i].az; p.elev_deg = views[i].el;
        unsigned char *img = NULL; char rerr[256] = {0};
        int rc = rgl_render_scene(draws, ng, rmin, rmax, views[i].zoom, &p, &img, rerr, sizeof rerr);
        if (rc != 0 || !img) { fprintf(stderr, "render %s failed: %s\n", views[i].name, rerr); fail = 1; continue; }
        snprintf(path, sizeof path, "%s/%s_%s.png", outdir, tag, views[i].name);
        rc = dtex_write_png(path, img, W, H);
        free(img);
        if (rc != 0) { fprintf(stderr, "PNG write failed %s\n", path); fail = 1; continue; }
        printf("wrote %-48s (az=%.0f el=%.0f zoom=%.2f)\n", path, views[i].az, views[i].el, views[i].zoom);
    }

    for (int i = 0; i < ng; i++) { free(g[i].v); free(g[i].tex); }
    gfx_context_destroy(ctx);
    return fail;
}
