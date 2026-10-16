/* renderer.c -- offscreen software rasterizer for Dirge of Cerberus models.
 *
 * Milestone P3: the first renderer for the purist native PC port. It links the
 * native model loader (../assets/models) and the native texture decoder
 * (../assets/textures/libdtex), loads a real DoC BMD/MOD model and a decoded
 * RGBA texture, transforms the geometry with a perspective camera, rasterizes
 * it with a z-buffer and perspective-correct attribute interpolation, applies
 * simple directional shading, and writes a PNG.
 *
 * This server is HEADLESS (no window system / GLFW / SDL), so this is a
 * dependency-free, portable C software rasterizer -- a reference / validation
 * renderer. The real GPU (Vulkan/D3D) backend is a later Windows milestone;
 * this software renderer serves as its ground-truth oracle. See README.md.
 *
 * Input bytes (models, textures) are untrusted; all parsing lives in the two
 * sibling loaders, which bounds-check every read. This file only consumes the
 * already-validated in-memory structures.
 */
#include "doc_model.h"
#include "dtex.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(d) ((float)((d) * M_PI / 180.0))

/* ------------------------------------------------------------------ math -- */

typedef struct { float x, y, z; } v3;
typedef struct { float m[16]; } mat4;   /* column-major */

static v3 v3sub(v3 a, v3 b){ return (v3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static v3 v3cross(v3 a, v3 b){
    return (v3){a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static float v3dot(v3 a, v3 b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
static v3 v3norm(v3 a){
    float l = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z);
    if (l < 1e-20f) return (v3){0,0,0};
    return (v3){a.x/l, a.y/l, a.z/l};
}

static mat4 mat_mul(mat4 a, mat4 b){   /* a*b, column-major */
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++){
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k*4+row] * b.m[c*4+k];
            r.m[c*4+row] = s;
        }
    return r;
}

/* Right-handed lookAt: eye looks at center with the given up. */
static mat4 mat_lookat(v3 eye, v3 center, v3 up){
    v3 f = v3norm(v3sub(center, eye));      /* forward */
    v3 s = v3norm(v3cross(f, up));          /* right   */
    v3 u = v3cross(s, f);                   /* true up */
    mat4 r = {{0}};
    r.m[0]=s.x; r.m[4]=s.y; r.m[8] =s.z;  r.m[12]=-v3dot(s, eye);
    r.m[1]=u.x; r.m[5]=u.y; r.m[9] =u.z;  r.m[13]=-v3dot(u, eye);
    r.m[2]=-f.x;r.m[6]=-f.y;r.m[10]=-f.z; r.m[14]= v3dot(f, eye);
    r.m[15]=1.0f;
    return r;
}

/* Right-handed perspective, maps near..far -> NDC z -1..1. */
static mat4 mat_persp(float fovy_rad, float aspect, float znear, float zfar){
    float ff = 1.0f / tanf(fovy_rad * 0.5f);
    mat4 r = {{0}};
    r.m[0]  = ff / aspect;
    r.m[5]  = ff;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

/* ---------------------------------------------------------- framebuffer -- */

typedef struct {
    int      w, h;
    uint8_t *rgba;      /* w*h*4 */
    float   *depth;     /* w*h, NDC z; cleared to +inf */
} Framebuffer;

static Framebuffer fb_create(int w, int h, uint8_t bgr, uint8_t bgg, uint8_t bgb){
    Framebuffer f;
    f.w = w; f.h = h;
    f.rgba  = (uint8_t*)malloc((size_t)w*h*4);
    f.depth = (float*)malloc((size_t)w*h*sizeof(float));
    for (int i = 0; i < w*h; i++){
        f.rgba[i*4+0]=bgr; f.rgba[i*4+1]=bgg; f.rgba[i*4+2]=bgb; f.rgba[i*4+3]=255;
        f.depth[i] = 1e30f;
    }
    return f;
}
static void fb_free(Framebuffer *f){ free(f->rgba); free(f->depth); }

/* ------------------------------------------------------------- texture -- */

typedef struct {
    int      w, h;
    uint8_t *rgba;      /* may be NULL (no texture) */
    int      bilinear;
} Texture;

/* Sample with wrap addressing; UVs are provisional so wrap is the safe choice. */
static void tex_sample(const Texture *t, float u, float v,
                       float *r, float *g, float *b){
    if (!t->rgba){ *r=*g=*b=1.0f; return; }
    /* wrap into [0,1) */
    u -= floorf(u);
    v -= floorf(v);
    float fx = u * (float)t->w - 0.5f;
    float fy = v * (float)t->h - 0.5f;
    if (!t->bilinear){
        int x = (int)floorf(fx + 0.5f);
        int y = (int)floorf(fy + 0.5f);
        x = ((x % t->w) + t->w) % t->w;
        y = ((y % t->h) + t->h) % t->h;
        const uint8_t *p = t->rgba + ((size_t)y*t->w + x)*4;
        *r=p[0]/255.0f; *g=p[1]/255.0f; *b=p[2]/255.0f;
        return;
    }
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - x0, ty = fy - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    int X0 = ((x0 % t->w)+t->w)%t->w, X1 = ((x1 % t->w)+t->w)%t->w;
    int Y0 = ((y0 % t->h)+t->h)%t->h, Y1 = ((y1 % t->h)+t->h)%t->h;
    const uint8_t *p00=t->rgba+((size_t)Y0*t->w+X0)*4;
    const uint8_t *p10=t->rgba+((size_t)Y0*t->w+X1)*4;
    const uint8_t *p01=t->rgba+((size_t)Y1*t->w+X0)*4;
    const uint8_t *p11=t->rgba+((size_t)Y1*t->w+X1)*4;
    for (int c = 0; c < 3; c++){
        float a = p00[c]*(1-tx) + p10[c]*tx;
        float bb= p01[c]*(1-tx) + p11[c]*tx;
        float val = a*(1-ty) + bb*ty;
        if (c==0)*r=val/255.0f; else if(c==1)*g=val/255.0f; else *b=val/255.0f;
    }
}

/* -------------------------------------------------------- rasterizer -- */

/* A vertex ready for rasterization (already in clip space + attributes). */
typedef struct {
    float x, y, z, w;   /* clip space                                   */
    float u, v;         /* texcoord                                     */
    v3    vpos;         /* view-space position (for flat normal)        */
} RVert;

enum { MODE_TEXTURED = 0, MODE_SHADED = 1, MODE_WIRE = 2 };

typedef struct {
    int   mode;
    int   backface_cull;    /* 0 = two-sided (default; winding unresolved)   */
    v3    light_dir;        /* view-space direction TOWARD the light          */
    float ambient;
    const Texture *tex;
} RenderState;

static void put_px(Framebuffer *fb, int x, int y, float r, float g, float b){
    if (x<0||y<0||x>=fb->w||y>=fb->h) return;
    uint8_t *p = fb->rgba + ((size_t)y*fb->w + x)*4;
    p[0]=(uint8_t)(r<0?0:r>1?255:r*255.0f+0.5f);
    p[1]=(uint8_t)(g<0?0:g>1?255:g*255.0f+0.5f);
    p[2]=(uint8_t)(b<0?0:b>1?255:b*255.0f+0.5f);
    p[3]=255;
}

static void draw_line(Framebuffer *fb, int x0,int y0,int x1,int y1,
                      float r,float g,float b){
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = -abs(y1-y0), sy = y0<y1?1:-1;
    int err = dx+dy;
    for(;;){
        put_px(fb,x0,y0,r,g,b);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 >= dy){ err += dy; x0 += sx; }
        if (e2 <= dx){ err += dx; y0 += sy; }
    }
}

/* Rasterize one triangle given three clip-space RVerts. Flat per-face shading
 * from the view-space normal (two-sided), perspective-correct UV. */
static void raster_tri(Framebuffer *fb, const RenderState *rs,
                       RVert a, RVert b, RVert c){
    /* Near-plane reject: anything at/behind the eye is skipped (keeps the
     * reference renderer simple; the auto-framed camera puts the whole model
     * in front of the near plane). */
    if (a.w <= 1e-4f || b.w <= 1e-4f || c.w <= 1e-4f) return;

    /* flat face normal + shading in view space (constant over the triangle) */
    v3 n = v3norm(v3cross(v3sub(b.vpos, a.vpos), v3sub(c.vpos, a.vpos)));
    /* centroid; viewer sits at origin looking down -Z */
    v3 ctr = {(a.vpos.x+b.vpos.x+c.vpos.x)/3.0f,
              (a.vpos.y+b.vpos.y+c.vpos.y)/3.0f,
              (a.vpos.z+b.vpos.z+c.vpos.z)/3.0f};
    v3 view_dir = v3norm((v3){-ctr.x,-ctr.y,-ctr.z});
    int facing_away = v3dot(n, view_dir) < 0.0f;
    if (rs->backface_cull && facing_away) return;
    if (facing_away){ n.x=-n.x; n.y=-n.y; n.z=-n.z; }  /* two-sided */
    float ndl = v3dot(n, rs->light_dir);
    if (ndl < 0) ndl = 0;
    float shade = rs->ambient + (1.0f - rs->ambient) * ndl;

    /* perspective divide -> screen */
    float iw[3] = {1.0f/a.w, 1.0f/b.w, 1.0f/c.w};
    float sx[3], sy[3], sz[3];
    RVert *vv[3] = {&a,&b,&c};
    for (int i = 0; i < 3; i++){
        float ndcx = vv[i]->x * iw[i];
        float ndcy = vv[i]->y * iw[i];
        float ndcz = vv[i]->z * iw[i];
        sx[i] = (ndcx*0.5f + 0.5f) * (float)fb->w;
        sy[i] = (1.0f - (ndcy*0.5f + 0.5f)) * (float)fb->h;
        sz[i] = ndcz;
    }

    if (rs->mode == MODE_WIRE){
        float r = shade, g = shade, bl = shade;
        draw_line(fb,(int)sx[0],(int)sy[0],(int)sx[1],(int)sy[1],r,g,bl);
        draw_line(fb,(int)sx[1],(int)sy[1],(int)sx[2],(int)sy[2],r,g,bl);
        draw_line(fb,(int)sx[2],(int)sy[2],(int)sx[0],(int)sy[0],r,g,bl);
        return;
    }

    /* bounding box */
    int minx = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2])));
    int maxx = (int)ceilf (fmaxf(sx[0], fmaxf(sx[1], sx[2])));
    int miny = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2])));
    int maxy = (int)ceilf (fmaxf(sy[0], fmaxf(sy[1], sy[2])));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= fb->w) maxx = fb->w-1;
    if (maxy >= fb->h) maxy = fb->h-1;
    if (maxx < minx || maxy < miny) return;

    /* edge-function area (screen space). */
    float area = (sx[1]-sx[0])*(sy[2]-sy[0]) - (sx[2]-sx[0])*(sy[1]-sy[0]);
    if (fabsf(area) < 1e-7f) return;   /* degenerate */
    float inv_area = 1.0f / area;

    /* perspective-correct attribute numerators (attr * 1/w) */
    float u_iw[3] = {a.u*iw[0], b.u*iw[1], c.u*iw[2]};
    float v_iw[3] = {a.v*iw[0], b.v*iw[1], c.v*iw[2]};

    for (int y = miny; y <= maxy; y++){
        float py = y + 0.5f;
        for (int x = minx; x <= maxx; x++){
            float px = x + 0.5f;
            /* barycentric via edge functions */
            float w0 = ((sx[1]-px)*(sy[2]-py) - (sx[2]-px)*(sy[1]-py)) * inv_area;
            float w1 = ((sx[2]-px)*(sy[0]-py) - (sx[0]-px)*(sy[2]-py)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;

            float depth = w0*sz[0] + w1*sz[1] + w2*sz[2];
            size_t di = (size_t)y*fb->w + x;
            if (depth >= fb->depth[di]) continue;

            float r, g, bl;
            if (rs->mode == MODE_TEXTURED && rs->tex && rs->tex->rgba){
                float inv_w = w0*iw[0] + w1*iw[1] + w2*iw[2];
                float u = (w0*u_iw[0] + w1*u_iw[1] + w2*u_iw[2]) / inv_w;
                float v = (w0*v_iw[0] + w1*v_iw[1] + w2*v_iw[2]) / inv_w;
                tex_sample(rs->tex, u, v, &r, &g, &bl);
            } else {
                /* shaded / untextured: warm neutral surface colour */
                r = 0.82f; g = 0.80f; bl = 0.78f;
            }
            r *= shade; g *= shade; bl *= shade;

            fb->depth[di] = depth;
            put_px(fb, x, y, r, g, bl);
        }
    }
}

/* ---------------------------------------------------------- framing -- */

/* Transform one DocVertex to an RVert (clip + view space). */
static RVert make_rvert(const DocVertex *dv, const mat4 *mv, const mat4 *mvp){
    RVert r;
    float x=dv->x, y=dv->y, z=dv->z;
    /* view-space position */
    r.vpos.x = mv->m[0]*x + mv->m[4]*y + mv->m[8] *z + mv->m[12];
    r.vpos.y = mv->m[1]*x + mv->m[5]*y + mv->m[9] *z + mv->m[13];
    r.vpos.z = mv->m[2]*x + mv->m[6]*y + mv->m[10]*z + mv->m[14];
    /* clip-space position */
    r.x = mvp->m[0]*x + mvp->m[4]*y + mvp->m[8] *z + mvp->m[12];
    r.y = mvp->m[1]*x + mvp->m[5]*y + mvp->m[9] *z + mvp->m[13];
    r.z = mvp->m[2]*x + mvp->m[6]*y + mvp->m[10]*z + mvp->m[14];
    r.w = mvp->m[3]*x + mvp->m[7]*y + mvp->m[11]*z + mvp->m[15];
    r.u = dv->u; r.v = dv->v;
    return r;
}

/* Does a model carry usable per-vertex UVs? (BMD has none yet; a MOD whose UVs
 * are all 0 or all identical is treated as having none.) */
static int model_has_uvs(const DocModel *m){
    if (m->umax <= m->umin + 1e-6f && m->vmax <= m->vmin + 1e-6f) return 0;
    return 1;
}

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
    int mode = -1;            /* -1 => auto (textured if UVs+tex, else shaded) */
    int bilinear = 1, cull = 0;

    /* positional: first non-flag after model = texture, second = out.png */
    int pos = 0;
    for (int i = 2; i < argc; i++){
        if (!strcmp(argv[i], "--angle") && i+1 < argc){ angle_deg = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--elev") && i+1 < argc){ elev_deg = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--size") && i+1 < argc){
            int a,b; if (sscanf(argv[++i], "%dx%d", &a,&b)==2 && a>0 && b>0){ W=a; H=b; }
        }
        else if (!strcmp(argv[i], "--mode") && i+1 < argc){
            const char *mm = argv[++i];
            if (!strcmp(mm,"textured")) mode = MODE_TEXTURED;
            else if (!strcmp(mm,"shaded")) mode = MODE_SHADED;
            else if (!strcmp(mm,"wire")) mode = MODE_WIRE;
        }
        else if (!strcmp(argv[i], "--nearest")) bilinear = 0;
        else if (!strcmp(argv[i], "--cull")) cull = 1;
        else if (argv[i][0] == '-'){ fprintf(stderr,"unknown option %s\n", argv[i]); }
        else {
            /* a positional ending in .png is the output; otherwise a texture */
            size_t L = strlen(argv[i]);
            int is_png = (L >= 4 && !strcmp(argv[i] + L - 4, ".png"));
            if (is_png) out_path = argv[i];
            else if (pos == 0){ tex_path = argv[i]; pos++; }
            else out_path = argv[i];
        }
    }

    /* ---- load model ---- */
    int err = 0;
    DocModel *m = doc_model_load(model_path, &err);
    if (!m){ fprintf(stderr, "model load failed (%d): %s\n", err, model_path); return 1; }
    doc_model_stats(m);
    printf("model : %s  kind=%s ver=%d  objs=%d strips=%d verts=%d tris=%d\n",
           model_path, m->kind, m->version, m->nobjs, m->total_strips,
           m->total_verts, m->total_tris);
    printf("bbox  : (%g %g %g) .. (%g %g %g)\n",
           m->bbmin[0],m->bbmin[1],m->bbmin[2], m->bbmax[0],m->bbmax[1],m->bbmax[2]);

    /* ---- load texture (optional) ---- */
    Texture tex = {0,0,NULL,bilinear};
    if (tex_path){
        FILE *f = fopen(tex_path, "rb");
        if (!f){ fprintf(stderr, "warning: cannot open texture %s\n", tex_path); }
        else {
            fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
            uint8_t *buf = (n>0)?(uint8_t*)malloc((size_t)n):NULL;
            if (buf && fread(buf,1,(size_t)n,f)==(size_t)n){
                dtex_set set;
                if (dtex_decode_container(buf,(size_t)n,&set)==0 && set.count>0){
                    dtex_image *im = &set.items[0];   /* first texture in container */
                    tex.w = im->width; tex.h = im->height;
                    tex.rgba = (uint8_t*)malloc((size_t)im->width*im->height*4);
                    memcpy(tex.rgba, im->rgba, (size_t)im->width*im->height*4);
                    printf("tex   : %s  %dx%d %s\n", tex_path, im->width, im->height, im->source);
                    dtex_free_set(&set);
                } else fprintf(stderr, "warning: no texture decoded from %s\n", tex_path);
            }
            free(buf); fclose(f);
        }
    }

    /* ---- choose mode ---- */
    int has_uv = model_has_uvs(m);
    if (mode < 0){
        mode = (tex.rgba && has_uv) ? MODE_TEXTURED : MODE_SHADED;
    }
    if (mode == MODE_TEXTURED && (!tex.rgba || !has_uv)){
        fprintf(stderr, "note: textured mode requested but %s; falling back to shaded\n",
                !tex.rgba ? "no texture loaded" : "model has no usable UVs");
        mode = MODE_SHADED;
    }
    const char *mode_name = mode==MODE_TEXTURED?"textured":mode==MODE_WIRE?"wire":"shaded";
    printf("mode  : %s   uvs=%s   angle=%.1f elev=%.1f   %dx%d\n",
           mode_name, has_uv?"yes":"no/provisional", angle_deg, elev_deg, W, H);

    /* ---- auto-frame camera from bounding box ---- */
    v3 center = { (m->bbmin[0]+m->bbmax[0])*0.5f,
                  (m->bbmin[1]+m->bbmax[1])*0.5f,
                  (m->bbmin[2]+m->bbmax[2])*0.5f };
    v3 ext = { (m->bbmax[0]-m->bbmin[0])*0.5f,
               (m->bbmax[1]-m->bbmin[1])*0.5f,
               (m->bbmax[2]-m->bbmin[2])*0.5f };
    float radius = sqrtf(ext.x*ext.x + ext.y*ext.y + ext.z*ext.z);
    if (radius < 1e-4f) radius = 1.0f;

    float fovy = DEG2RAD(45.0f);
    float aspect = (float)W / (float)H;
    /* distance so the bounding sphere fits in the (tighter) vertical FOV */
    float fit = fminf(fovy, 2.0f*atanf(tanf(fovy*0.5f)*aspect));
    float dist = radius / sinf(fit * 0.5f) * 1.25f;   /* 25% margin */

    float az = DEG2RAD(angle_deg);
    float el = DEG2RAD(elev_deg);
    v3 eye = {
        center.x + dist * cosf(el) * sinf(az),
        center.y + dist * sinf(el),
        center.z + dist * cosf(el) * cosf(az)
    };
    v3 up = {0,1,0};
    mat4 view = mat_lookat(eye, center, up);
    mat4 proj = mat_persp(fovy, aspect, dist*0.01f + 0.01f, dist + radius*2.0f + 1.0f);
    mat4 mvp  = mat_mul(proj, view);   /* model = identity */

    /* ---- render ---- */
    Framebuffer fb = fb_create(W, H, 24, 26, 32);   /* dark slate background */
    RenderState rs;
    rs.mode = mode;
    rs.backface_cull = cull;
    rs.light_dir = v3norm((v3){0.35f, 0.55f, 0.75f});  /* view-space headlight-ish */
    rs.ambient = 0.22f;
    rs.tex = &tex;

    long tris_drawn = 0;
    for (int o = 0; o < m->nobjs; o++){
        const DocObject *ob = &m->objs[o];
        for (int s = 0; s < ob->nstrips; s++){
            const DocStrip *st = &ob->strips[s];
            for (int vtx = 0; vtx + 2 < st->nverts; vtx++){
                const DocVertex *p0 = &st->verts[vtx];
                const DocVertex *p1 = &st->verts[vtx+1];
                const DocVertex *p2 = &st->verts[vtx+2];
                RVert A = make_rvert(p0, &view, &mvp);
                RVert B = make_rvert(p1, &view, &mvp);
                RVert C = make_rvert(p2, &view, &mvp);
                if (vtx & 1){ RVert t=B; B=C; C=t; }   /* strip winding flip */
                raster_tri(&fb, &rs, A, B, C);
                tris_drawn++;
            }
        }
    }

    if (dtex_write_png(out_path, fb.rgba, fb.w, fb.h) != 0){
        fprintf(stderr, "PNG write failed: %s\n", out_path);
        fb_free(&fb); free(tex.rgba); doc_model_free(m);
        return 1;
    }
    printf("wrote : %s   (%ld triangles rasterized)\n", out_path, tris_drawn);

    fb_free(&fb);
    free(tex.rgba);
    doc_model_free(m);
    return 0;
}
