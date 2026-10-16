/* main.c -- GPU renderer CLI for Dirge of Cerberus models (Milestone P3, GPU).
 *
 * Same interface and framing as the software reference (../renderer/renderer.c),
 * but the actual rasterization runs on the GPU through an EGL-offscreen GL
 * 3.3-core context + an FBO. Loads a real DoC BMD/MOD model (libdoc_model) and
 * an optional decoded RGBA texture (libdtex), renders, and writes a PNG with
 * the texture lib's self-contained writer.
 *
 *   gpu_renderer <model.bin> [texture.bin] [out.png]
 *                [--angle DEG] [--elev DEG] [--size WxH]
 *                [--mode textured|shaded|wire] [--nearest] [--cull]
 */
#include "doc_model.h"
#include "dtex.h"
#include "gfx_context.h"
#include "renderer_gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv){
    if (argc < 2){
        fprintf(stderr,
          "usage: %s <model.bin> [texture.bin] [out.png]\n"
          "          [--angle DEG] [--elev DEG] [--size WxH]\n"
          "          [--mode textured|shaded|wire] [--nearest] [--cull]\n",
          argv[0]);
        return 2;
    }
    const char *model_path = argv[1];
    const char *tex_path = NULL;
    const char *out_path = "out.png";
    float angle_deg = 35.0f, elev_deg = 20.0f;
    int W = 1000, H = 750;
    int mode = -1;
    int bilinear = 1, cull = 0;

    int pos = 0;
    for (int i = 2; i < argc; i++){
        if (!strcmp(argv[i], "--angle") && i+1 < argc){ angle_deg = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--elev") && i+1 < argc){ elev_deg = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--size") && i+1 < argc){
            int a,b; if (sscanf(argv[++i], "%dx%d", &a,&b)==2 && a>0 && b>0){ W=a; H=b; }
        }
        else if (!strcmp(argv[i], "--mode") && i+1 < argc){
            const char *mm = argv[++i];
            if (!strcmp(mm,"textured")) mode = RGL_TEXTURED;
            else if (!strcmp(mm,"shaded")) mode = RGL_SHADED;
            else if (!strcmp(mm,"wire")) mode = RGL_WIRE;
        }
        else if (!strcmp(argv[i], "--nearest")) bilinear = 0;
        else if (!strcmp(argv[i], "--cull")) cull = 1;
        else if (argv[i][0] == '-'){ fprintf(stderr,"unknown option %s\n", argv[i]); }
        else {
            size_t L = strlen(argv[i]);
            int is_png = (L >= 4 && !strcmp(argv[i] + L - 4, ".png"));
            if (is_png) out_path = argv[i];
            else if (pos == 0){ tex_path = argv[i]; pos++; }
            else out_path = argv[i];
        }
    }

    /* ---- load model ---- */
    int lerr = 0;
    DocModel *m = doc_model_load(model_path, &lerr);
    if (!m){ fprintf(stderr, "model load failed (%d): %s\n", lerr, model_path); return 1; }
    doc_model_stats(m);
    printf("model : %s  kind=%s ver=%d  objs=%d strips=%d verts=%d tris=%d\n",
           model_path, m->kind, m->version, m->nobjs, m->total_strips,
           m->total_verts, m->total_tris);
    printf("bbox  : (%g %g %g) .. (%g %g %g)\n",
           m->bbmin[0],m->bbmin[1],m->bbmin[2], m->bbmax[0],m->bbmax[1],m->bbmax[2]);

    /* ---- load texture (optional) ---- */
    unsigned char *tex_rgba = NULL; int tex_w = 0, tex_h = 0;
    if (tex_path){
        FILE *f = fopen(tex_path, "rb");
        if (!f){ fprintf(stderr, "warning: cannot open texture %s\n", tex_path); }
        else {
            fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
            uint8_t *buf = (n>0)?(uint8_t*)malloc((size_t)n):NULL;
            if (buf && fread(buf,1,(size_t)n,f)==(size_t)n){
                dtex_set set;
                if (dtex_decode_container(buf,(size_t)n,&set)==0 && set.count>0){
                    dtex_image *im = &set.items[0];
                    tex_w = im->width; tex_h = im->height;
                    tex_rgba = (unsigned char*)malloc((size_t)tex_w*tex_h*4);
                    memcpy(tex_rgba, im->rgba, (size_t)tex_w*tex_h*4);
                    printf("tex   : %s  %dx%d %s\n", tex_path, im->width, im->height, im->source);
                    dtex_free_set(&set);
                } else fprintf(stderr, "warning: no texture decoded from %s\n", tex_path);
            }
            free(buf); fclose(f);
        }
    }

    /* ---- choose mode ---- */
    int has_uv = !(m->umax <= m->umin + 1e-6f && m->vmax <= m->vmin + 1e-6f);
    if (mode < 0) mode = (tex_rgba && has_uv) ? RGL_TEXTURED : RGL_SHADED;
    if (mode == RGL_TEXTURED && (!tex_rgba || !has_uv)){
        fprintf(stderr, "note: textured mode requested but %s; falling back to shaded\n",
                !tex_rgba ? "no texture loaded" : "model has no usable UVs");
        mode = RGL_SHADED;
    }
    const char *mode_name = mode==RGL_TEXTURED?"textured":mode==RGL_WIRE?"wire":"shaded";

    /* ---- GL context (the platform seam) ---- */
    char cerr[256] = {0};
    gfx_context *ctx = gfx_context_create(W, H, cerr, sizeof(cerr));
    if (!ctx){
        fprintf(stderr, "GL context init failed: %s\n", cerr);
        free(tex_rgba); doc_model_free(m); return 3;
    }
    printf("gl    : %s\n", gfx_context_describe(ctx));
    printf("mode  : %s   uvs=%s   angle=%.1f elev=%.1f   %dx%d\n",
           mode_name, has_uv?"yes":"no/provisional", angle_deg, elev_deg, W, H);

    /* ---- render ---- */
    rgl_params p = {0};
    p.width = W; p.height = H;
    p.angle_deg = angle_deg; p.elev_deg = elev_deg;
    p.mode = mode; p.bilinear = bilinear; p.backface_cull = cull;
    p.tex_rgba = tex_rgba; p.tex_w = tex_w; p.tex_h = tex_h;

    unsigned char *img = NULL; char rerr[256] = {0};
    int rc = rgl_render(m, &p, &img, rerr, sizeof(rerr));
    if (rc != 0 || !img){
        fprintf(stderr, "render failed: %s\n", rerr);
        gfx_context_destroy(ctx); free(tex_rgba); doc_model_free(m); return 4;
    }

    if (dtex_write_png(out_path, img, W, H) != 0){
        fprintf(stderr, "PNG write failed: %s\n", out_path);
        free(img); gfx_context_destroy(ctx); free(tex_rgba); doc_model_free(m); return 5;
    }
    printf("wrote : %s   (GPU render, %d tris)\n", out_path, m->total_tris);

    free(img);
    gfx_context_destroy(ctx);
    free(tex_rgba);
    doc_model_free(m);
    return 0;
}
