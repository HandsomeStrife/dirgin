/* render_char.c -- load the REAL posed+textured Dirge of Cerberus character
 * (exported by ../assets/models/render_skinned.py) and render it on the actual
 * GPU through the PURIST RENDER-MATH shader pair (renderer_gl.c, rgl_render_mesh)
 * via the EGL-offscreen path -> PNG.
 *
 * This is the convergence milestone: the native GL renderer drawing real decoded
 * game content (char e000, Vincent), not a synthetic test mesh.
 *
 *   INPUT  char_e000.mesh  -- custom binary "DMSH" triangle soup. Header:
 *            { char magic[4]="DMSH", u32 version, u32 vcount, f32 bbmin[3],
 *              f32 bbmax[3] } then vcount * RVertex (12 f32: pos3,nrm3,uv2,rgba4).
 *            The layout is byte-identical to `RVertex`, so verts fread straight.
 *   TEXTURE  the body atlas (RST 512x256 PSMT8) decoded natively from the sibling
 *            texture container via libdtex (read-only) -- the largest image in it.
 *
 *   render_char [-m MESH] [-t TEX_CONTAINER] [-o OUTDIR]
 *
 * Writes PRELIT+textured and LIT+textured front / three-quarter PNGs (plus a LIT
 * untextured "clean" pass), auto-framed, and prints one line each.
 */
#include "dtex.h"
#include "gfx_context.h"
#include "renderer_gl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- mesh load ---- */
typedef struct {
    RVertex *v;
    size_t   n;              /* vertex count (= 3 * triangles) */
    float    bbmin[3], bbmax[3];
} CharMesh;

/* Load the "DMSH" triangle soup. Bounds-checked: the file is treated as
 * untrusted. Returns 0 on success. */
static int mesh_load(const char *path, CharMesh *out, char *err, size_t errlen) {
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
        snprintf(err, errlen, "bad mesh header/magic"); fclose(f); return 1;
    }
    /* guard vcount against the actual remaining file size */
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
/* Decode the body atlas natively via libdtex and keep the LARGEST image in the
 * container (the 512x256 body sheet; the rest are tiny eye/envmap maps). This is
 * the same atlas render_skinned.py's software rasteriser samples. */
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
    if (name && namelen)
        snprintf(name, namelen, "%s %dx%d", im->source, *w, *h);
    dtex_free_set(&set);
    return 0;
}

/* ------------------------------------------------------------- one render --- */
static int render_png(const CharMesh *m, const rgl_params *p, const char *path) {
    unsigned char *img = NULL; char err[256] = {0};
    int rc = rgl_render_mesh(m->v, m->n, m->bbmin, m->bbmax, p, &img, err, sizeof err);
    if (rc != 0 || !img) { fprintf(stderr, "render failed (%s): %s\n", path, err); return 1; }
    rc = dtex_write_png(path, img, p->width, p->height);
    free(img);
    if (rc != 0) { fprintf(stderr, "PNG write failed: %s\n", path); return 1; }
    printf("wrote %-44s (%zu tris)\n", path, m->n / 3);
    return 0;
}

int main(int argc, char **argv) {
    const char *meshpath = "../assets/models/out/char_e000.mesh";
    const char *tex_container = "/srv/games/dirge-port/extract/_KEL/noPath/07650002.bin";
    const char *outdir = "out";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) meshpath = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) tex_container = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
    }

    char err[256] = {0};
    CharMesh mesh;
    if (mesh_load(meshpath, &mesh, err, sizeof err) != 0) {
        fprintf(stderr, "mesh load: %s\n", err); return 2;
    }
    printf("mesh  : %s  (%zu verts / %zu tris)\n", meshpath, mesh.n, mesh.n / 3);
    printf("bbox  : X[%.2f,%.2f] Y[%.2f,%.2f] Z[%.2f,%.2f]\n",
           mesh.bbmin[0], mesh.bbmax[0], mesh.bbmin[1], mesh.bbmax[1],
           mesh.bbmin[2], mesh.bbmax[2]);

    const int W = 900, H = 1200;
    char cerr[256] = {0};
    gfx_context *ctx = gfx_context_create(W, H, cerr, sizeof cerr);
    if (!ctx) { fprintf(stderr, "GL context init failed: %s\n", cerr); free(mesh.v); return 3; }
    printf("gl    : %s\n", gfx_context_describe(ctx));

    unsigned char *tex = NULL; int tw = 0, th = 0; char tname[64] = {0};
    if (tex_load_largest(tex_container, &tex, &tw, &th, tname, sizeof tname) != 0)
        fprintf(stderr, "warning: texture decode failed (%s); textured passes skipped\n",
                tex_container);
    else
        printf("tex   : %s  (%s)\n", tex_container, tname);

    /* base params: two-sided (strip winding isn't globally consistent), modulate */
    rgl_params base; memset(&base, 0, sizeof base);
    base.width = W; base.height = H; base.bilinear = 1; base.backface_cull = 0;
    base.rig = rgl_default_lightrig(); base.tfx = RGL_TFX_MODULATE; base.alpha_ref = -1.0f;

    char path[1024];
    int fail = 0;
    struct { const char *tag; float az, el; } views[] = {
        { "front", 18.f, 6.f }, { "3q", 55.f, 8.f },
    };

    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        /* PRELIT + textured: baked vertex colour passes through, texture modulates
         * (VU1 programs A/B). The flat, unshaded original look. */
        if (tex) {
            rgl_params q = base; q.shade_mode = RGL_SHADE_PRELIT;
            q.tex_rgba = tex; q.tex_w = tw; q.tex_h = th;
            q.angle_deg = views[i].az; q.elev_deg = views[i].el;
            snprintf(path, sizeof path, "%s/char_e000_prelit_%s.png", outdir, views[i].tag);
            fail |= render_png(&mesh, &q, path);
        }
        /* LIT + textured: full VU1 program D -- 3 directional lights + ambient,
         * (x) vertex colour, texture modulate. */
        if (tex) {
            rgl_params q = base; q.shade_mode = RGL_SHADE_LIT;
            q.tex_rgba = tex; q.tex_w = tw; q.tex_h = th;
            q.angle_deg = views[i].az; q.elev_deg = views[i].el;
            snprintf(path, sizeof path, "%s/char_e000_lit_%s.png", outdir, views[i].tag);
            fail |= render_png(&mesh, &q, path);
        }
    }

    /* LIT, untextured -- the shading/connectivity read of the bare body surface. */
    {
        rgl_params q = base; q.shade_mode = RGL_SHADE_LIT; q.tex_rgba = NULL;
        q.angle_deg = 18.f; q.elev_deg = 6.f;
        snprintf(path, sizeof path, "%s/char_e000_lit_clean_front.png", outdir);
        fail |= render_png(&mesh, &q, path);
    }

    free(tex); free(mesh.v);
    gfx_context_destroy(ctx);
    return fail ? 1 : 0;
}
