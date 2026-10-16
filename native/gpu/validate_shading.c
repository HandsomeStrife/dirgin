/* validate_shading.c -- synthetic-mesh validation of the PURIST RENDER-MATH
 * shading (native/gpu/renderer_gl.c, rgl_render_mesh).
 *
 * WHY SYNTHETIC: the real model loader (../assets/models) does not yet emit
 * per-vertex normals / colour / UVs, and that directory is busy -- so this
 * binary does NOT link it. It builds its own meshes in-code (UV sphere,
 * subdivided cube, tiled ground plane) carrying the full RENDER-MATH vertex
 * format (position, normal, UV, RGBA), decodes a REAL game texture through
 * libdtex (read-only), and renders through the GL 3.3-core / EGL-offscreen
 * path -- the same renderer core that real models will use once wired.
 *
 * It exercises both shading modes from RENDER-MATH.md:
 *   LIT    (program D, actors) : 3 directional lights + ambient, (x) vcol.
 *   PRELIT (programs A/B, world): vertex colour pass-through, texture modulate.
 *
 *   validate_shading [-o OUTDIR] [TEXTURE_CONTAINER]
 *
 * Writes the PNGs listed in main() and prints one line each.
 */
#include "dtex.h"
#include "gfx_context.h"
#include "renderer_gl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------ mesh buffer -- */
typedef struct {
    RVertex *v;
    size_t   n, cap;
    float    bbmin[3], bbmax[3];
} Mesh;

static void mesh_init(Mesh *m){
    m->v = NULL; m->n = 0; m->cap = 0;
    m->bbmin[0]=m->bbmin[1]=m->bbmin[2]=  1e30f;
    m->bbmax[0]=m->bbmax[1]=m->bbmax[2]= -1e30f;
}
static void mesh_push(Mesh *m, RVertex vv){
    if (m->n == m->cap){
        m->cap = m->cap ? m->cap*2 : 1024;
        m->v = (RVertex*)realloc(m->v, m->cap*sizeof(RVertex));
    }
    m->v[m->n++] = vv;
    if (vv.px < m->bbmin[0]) m->bbmin[0]=vv.px;
    if (vv.py < m->bbmin[1]) m->bbmin[1]=vv.py;
    if (vv.pz < m->bbmin[2]) m->bbmin[2]=vv.pz;
    if (vv.px > m->bbmax[0]) m->bbmax[0]=vv.px;
    if (vv.py > m->bbmax[1]) m->bbmax[1]=vv.py;
    if (vv.pz > m->bbmax[2]) m->bbmax[2]=vv.pz;
}
static void mesh_free(Mesh *m){ free(m->v); }

static RVertex mkv(float px,float py,float pz, float nx,float ny,float nz,
                   float u,float v, float r,float g,float b,float a){
    RVertex o; o.px=px;o.py=py;o.pz=pz; o.nx=nx;o.ny=ny;o.nz=nz;
    o.u=u;o.v=v; o.r=r;o.g=g;o.b=b;o.a=a; return o;
}

/* ------------------------------------------------------- UV sphere (smooth) -
 * Per-vertex normal = normalized position, so the Gouraud N.L gradient is
 * smooth -- the clearest possible read of the 3-directional-light model. */
static void gen_sphere(Mesh *m, int stacks, int slices, float R,
                       float r,float g,float b,float a){
    mesh_init(m);
    for (int i = 0; i < stacks; i++){
        float t0 = (float)i / stacks,       t1 = (float)(i+1) / stacks;
        float ph0 = (float)(t0*M_PI),        ph1 = (float)(t1*M_PI);
        for (int j = 0; j < slices; j++){
            float s0 = (float)j / slices,     s1 = (float)(j+1) / slices;
            float th0 = (float)(s0*2.0*M_PI), th1 = (float)(s1*2.0*M_PI);
            /* 4 corners */
            float n00[3]={sinf(ph0)*cosf(th0),cosf(ph0),sinf(ph0)*sinf(th0)};
            float n01[3]={sinf(ph0)*cosf(th1),cosf(ph0),sinf(ph0)*sinf(th1)};
            float n10[3]={sinf(ph1)*cosf(th0),cosf(ph1),sinf(ph1)*sinf(th0)};
            float n11[3]={sinf(ph1)*cosf(th1),cosf(ph1),sinf(ph1)*sinf(th1)};
            RVertex A = mkv(R*n00[0],R*n00[1],R*n00[2], n00[0],n00[1],n00[2], s0,t0, r,g,b,a);
            RVertex B = mkv(R*n01[0],R*n01[1],R*n01[2], n01[0],n01[1],n01[2], s1,t0, r,g,b,a);
            RVertex C = mkv(R*n11[0],R*n11[1],R*n11[2], n11[0],n11[1],n11[2], s1,t1, r,g,b,a);
            RVertex D = mkv(R*n10[0],R*n10[1],R*n10[2], n10[0],n10[1],n10[2], s0,t1, r,g,b,a);
            mesh_push(m,A); mesh_push(m,B); mesh_push(m,C);
            mesh_push(m,A); mesh_push(m,C); mesh_push(m,D);
        }
    }
}

/* -------------------------------------------------- subdivided cube ---------
 * 6 flat faces, each `sub`x`sub`, UV 0..1 per face (texture maps once/face),
 * per-face flat normal. `per_face_col` tints each face a distinct vertex
 * colour (to show the PRELIT pass-through); otherwise a uniform colour. */
static const float CUBE_FACE_COL[6][3] = {
    {0.90f,0.30f,0.30f}, {0.30f,0.90f,0.40f}, {0.35f,0.45f,0.95f},
    {0.92f,0.85f,0.30f}, {0.90f,0.45f,0.90f}, {0.35f,0.88f,0.90f},
};
static void gen_cube(Mesh *m, int sub, float H,
                     int per_face_col, float ur,float ug,float ub){
    mesh_init(m);
    /* face basis: origin corner, +u edge, +v edge, outward normal */
    const float F[6][3][3] = {
        /* +X */ {{ 1, 1, 1},{ 0, 0,-2},{ 0,-2, 0}},
        /* -X */ {{-1, 1,-1},{ 0, 0, 2},{ 0,-2, 0}},
        /* +Y */ {{-1, 1,-1},{ 2, 0, 0},{ 0, 0, 2}},
        /* -Y */ {{-1,-1, 1},{ 2, 0, 0},{ 0, 0,-2}},
        /* +Z */ {{-1, 1, 1},{ 2, 0, 0},{ 0,-2, 0}},
        /* -Z */ {{ 1, 1,-1},{-2, 0, 0},{ 0,-2, 0}},
    };
    const float N[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int f = 0; f < 6; f++){
        float cr=ur,cg=ug,cb=ub;
        if (per_face_col){ cr=CUBE_FACE_COL[f][0]; cg=CUBE_FACE_COL[f][1]; cb=CUBE_FACE_COL[f][2]; }
        for (int i = 0; i < sub; i++){
            for (int j = 0; j < sub; j++){
                float u0=(float)i/sub, u1=(float)(i+1)/sub;
                float v0=(float)j/sub, v1=(float)(j+1)/sub;
                /* corner(a,b) in [0..1]^2 -> world */
                #define CORN(uu,vv) mkv( \
                    H*(F[f][0][0] + (uu)*F[f][1][0] + (vv)*F[f][2][0])*0.5f, \
                    H*(F[f][0][1] + (uu)*F[f][1][1] + (vv)*F[f][2][1])*0.5f, \
                    H*(F[f][0][2] + (uu)*F[f][1][2] + (vv)*F[f][2][2])*0.5f, \
                    N[f][0],N[f][1],N[f][2], (uu),(vv), cr,cg,cb,1.0f)
                RVertex A=CORN(u0,v0), B=CORN(u1,v0), C=CORN(u1,v1), D=CORN(u0,v1);
                #undef CORN
                mesh_push(m,A); mesh_push(m,B); mesh_push(m,C);
                mesh_push(m,A); mesh_push(m,C); mesh_push(m,D);
            }
        }
    }
}

/* ---------------------------------------------------- tiled ground plane ----
 * A big XZ plane subdivided into `sub`x`sub` quads, normal +Y, UV tiled
 * 0..`tile`. Viewed at a grazing angle it is the classic perspective-correct
 * texturing proof: the tiles compress toward the far edge iff s/w,t/w is
 * interpolated correctly. */
static void gen_plane(Mesh *m, int sub, float size, float tile,
                      float r,float g,float b){
    mesh_init(m);
    for (int i = 0; i < sub; i++){
        for (int j = 0; j < sub; j++){
            float x0=((float)i/sub-0.5f)*size, x1=((float)(i+1)/sub-0.5f)*size;
            float z0=((float)j/sub-0.5f)*size, z1=((float)(j+1)/sub-0.5f)*size;
            float u0=(float)i/sub*tile, u1=(float)(i+1)/sub*tile;
            float v0=(float)j/sub*tile, v1=(float)(j+1)/sub*tile;
            RVertex A=mkv(x0,0,z0, 0,1,0, u0,v0, r,g,b,1.0f);
            RVertex B=mkv(x1,0,z0, 0,1,0, u1,v0, r,g,b,1.0f);
            RVertex C=mkv(x1,0,z1, 0,1,0, u1,v1, r,g,b,1.0f);
            RVertex D=mkv(x0,0,z1, 0,1,0, u0,v1, r,g,b,1.0f);
            mesh_push(m,A); mesh_push(m,B); mesh_push(m,C);
            mesh_push(m,A); mesh_push(m,C); mesh_push(m,D);
        }
    }
}

/* ------------------------------------------------------------- texture ------ */
static int load_texture(const char *path, unsigned char **rgba, int *w, int *h,
                        char *name, size_t namelen){
    *rgba=NULL; *w=*h=0;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *buf = (n>0)?(uint8_t*)malloc((size_t)n):NULL;
    int ok = (buf && fread(buf,1,(size_t)n,f)==(size_t)n);
    fclose(f);
    if (!ok){ free(buf); return 1; }
    dtex_set set;
    int rc = dtex_decode_container(buf,(size_t)n,&set);
    free(buf);
    if (rc != 0 || set.count < 1) return 1;
    dtex_image *im = &set.items[0];
    *w=im->width; *h=im->height;
    *rgba = (unsigned char*)malloc((size_t)(*w)*(*h)*4);
    memcpy(*rgba, im->rgba, (size_t)(*w)*(*h)*4);
    if (name && namelen){ snprintf(name,namelen,"%s %dx%d", im->source, *w, *h); }
    dtex_free_set(&set);
    return 0;
}

/* ----------------------------------------------------------- one render ----- */
static int render_mesh_png(const Mesh *m, const rgl_params *base,
                           const char *outpath){
    unsigned char *img=NULL; char err[256]={0};
    int rc = rgl_render_mesh(m->v, m->n, m->bbmin, m->bbmax, base, &img, err, sizeof(err));
    if (rc != 0 || !img){ fprintf(stderr,"render failed (%s): %s\n", outpath, err); return 1; }
    rc = dtex_write_png(outpath, img, base->width, base->height);
    free(img);
    if (rc != 0){ fprintf(stderr,"PNG write failed: %s\n", outpath); return 1; }
    printf("wrote %-40s  (%zu tris)\n", outpath, m->n/3);
    return 0;
}

int main(int argc, char **argv){
    const char *outdir = "out";
    const char *tex_container = "/srv/games/dirge-port/extract/_KEL/noPath/06d1ff0b.bin";
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"-o") && i+1<argc) outdir = argv[++i];
        else tex_container = argv[i];
    }

    char path[1024];
    char cerr[256]={0};
    gfx_context *ctx = gfx_context_create(1000, 750, cerr, sizeof(cerr));
    if (!ctx){ fprintf(stderr,"GL context init failed: %s\n", cerr); return 3; }
    printf("gl    : %s\n", gfx_context_describe(ctx));

    /* real decoded texture (libdtex, read-only) */
    unsigned char *tex=NULL; int tw=0,th=0; char tname[64]={0};
    if (load_texture(tex_container, &tex, &tw, &th, tname, sizeof(tname)) != 0)
        fprintf(stderr,"warning: could not decode texture %s (textured renders skipped)\n", tex_container);
    else
        printf("tex   : %s  (%s)\n", tex_container, tname);

    const int W=1000, H=750;
    const rgl_lightrig *rig = rgl_default_lightrig();

    /* base params shared by all renders */
    rgl_params p; memset(&p,0,sizeof p);
    p.width=W; p.height=H; p.bilinear=1; p.backface_cull=0;
    p.rig=rig; p.tfx=RGL_TFX_MODULATE; p.alpha_ref=-1.0f;

    int fail=0;

    /* 1) LIT sphere -- 3-directional-light N.L gradient, white vcol, no tex. */
    {
        Mesh m; gen_sphere(&m, 48, 64, 1.0f, 1.0f,1.0f,1.0f,1.0f);
        rgl_params q=p; q.shade_mode=RGL_SHADE_LIT; q.tex_rgba=NULL;
        q.angle_deg=35; q.elev_deg=20;
        snprintf(path,sizeof path,"%s/gpu_lit_sphere.png",outdir);
        fail |= render_mesh_png(&m, &q, path);
        mesh_free(&m);
    }

    /* 2) PRELIT textured cube -- real crate texture, neutral (128) vcol so the
     *    PS2 x2 MODULATE yields the texture at natural brightness. */
    if (tex){
        Mesh m; gen_cube(&m, 2, 1.6f, 0, 0.5f,0.5f,0.5f);  /* neutral vcol = PS2 128 */
        rgl_params q=p; q.shade_mode=RGL_SHADE_PRELIT;
        q.tex_rgba=tex; q.tex_w=tw; q.tex_h=th;
        q.angle_deg=35; q.elev_deg=25;
        snprintf(path,sizeof path,"%s/gpu_textured.png",outdir);
        fail |= render_mesh_png(&m, &q, path);
        mesh_free(&m);
    }

    /* 3) Perspective-correct proof -- tiled crate texture on a grazing plane. */
    if (tex){
        Mesh m; gen_plane(&m, 32, 8.0f, 6.0f, 0.5f,0.5f,0.5f);
        rgl_params q=p; q.shade_mode=RGL_SHADE_PRELIT;
        q.tex_rgba=tex; q.tex_w=tw; q.tex_h=th; q.bilinear=1;
        q.angle_deg=0; q.elev_deg=11;   /* low, grazing -> strong foreshortening */
        snprintf(path,sizeof path,"%s/gpu_perspective_plane.png",outdir);
        fail |= render_mesh_png(&m, &q, path);
        mesh_free(&m);
    }

    /* 4) PRELIT vertex-colour cube -- A/B pass-through (no N.L, no texture). */
    {
        Mesh m; gen_cube(&m, 1, 1.6f, 1, 1,1,1);   /* per-face distinct colours */
        rgl_params q=p; q.shade_mode=RGL_SHADE_PRELIT; q.tex_rgba=NULL;
        q.angle_deg=35; q.elev_deg=25;
        snprintf(path,sizeof path,"%s/gpu_prelit_vcolor.png",outdir);
        fail |= render_mesh_png(&m, &q, path);
        mesh_free(&m);
    }

    /* 5) LIT + textured sphere -- full program D: lighting x texture. Neutral
     *    (128) albedo vcol, so lit*0.5 then x2 MODULATE keeps lit areas natural
     *    and shades the texture on the dark side. */
    if (tex){
        Mesh m; gen_sphere(&m, 48, 64, 1.0f, 0.5f,0.5f,0.5f,1.0f);
        rgl_params q=p; q.shade_mode=RGL_SHADE_LIT;
        q.tex_rgba=tex; q.tex_w=tw; q.tex_h=th;
        q.angle_deg=35; q.elev_deg=20;
        snprintf(path,sizeof path,"%s/gpu_lit_textured.png",outdir);
        fail |= render_mesh_png(&m, &q, path);
        mesh_free(&m);
    }

    free(tex);
    gfx_context_destroy(ctx);
    return fail ? 1 : 0;
}
