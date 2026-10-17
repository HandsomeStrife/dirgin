/* render_room_native.c -- the ENGINE-NATIVE whole-room textured render.
 *
 * Unlike render_env_textured.c (which consumes DMSH meshes + a .list emitted by
 * the Python exporter), this drives the render entirely from the native C stack:
 *   - geometry : doc_env (native BMD VIF decode)         ../assets/models/doc_env
 *   - binding  : doc_room_binding (the material loader)  ../engine/scene/material_loader
 *   - textures : libdtex                                 ../assets/textures
 *   - render   : renderer_gl (PRELIT + MODULATE, EGL)    ./renderer_gl
 *
 * It unions all 13 z143 chunks (shared world frame), maps each decoded packet to
 * its recovered FileCode via the loader, merges globally by FileCode, decodes each
 * texture, and renders the whole room in one GL world. This is the Python
 * export_room_bound.py pipeline, now fully in native code.
 *
 *   render_room_native [-m MANIFESTDIR] [-x EXTRACTDIR] [-o OUTDIR]
 *
 * Inputs (extracted game data) are untrusted: read-only, never executed.
 */
#include "doc_env.h"
#include "material_loader.h"
#include "dtex.h"
#include "renderer_gl.h"
#include "gfx_context.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXFC 64

static const char *CHUNKS[] = {
    "068f0012","068f0112","068f0212","068f0312","068f0412","068f0512","068f0612",
    "068f0712","068f0912","068f0a12","068f0b12","068f0c12","068f0d12"
};
#define NCHUNK ((int)(sizeof CHUNKS / sizeof CHUNKS[0]))

/* a growable RVertex soup per FileCode (or the unbound bucket) */
typedef struct {
    char      fc[9];          /* "" == unbound */
    RVertex  *v;
    size_t    n, cap;
    int       abe;
    unsigned char *tex; int tw, th;
} Bucket;

static Bucket *find_bucket(Bucket *bk, int *nb, const char *fc) {
    for (int i = 0; i < *nb; i++) if (strcmp(bk[i].fc, fc) == 0) return &bk[i];
    if (*nb >= MAXFC) return NULL;
    Bucket *b = &bk[(*nb)++];
    memset(b, 0, sizeof *b);
    snprintf(b->fc, sizeof b->fc, "%s", fc);
    return b;
}

static void push_tri(Bucket *b, const float P[3][3], const float UV[3][2],
                     const unsigned char C[3][4]) {
    /* flat face normal (area-weighted), skip degenerate */
    float e1[3] = { P[1][0]-P[0][0], P[1][1]-P[0][1], P[1][2]-P[0][2] };
    float e2[3] = { P[2][0]-P[0][0], P[2][1]-P[0][1], P[2][2]-P[0][2] };
    float nx = e1[1]*e2[2]-e1[2]*e2[1], ny = e1[2]*e2[0]-e1[0]*e2[2], nz = e1[0]*e2[1]-e1[1]*e2[0];
    float l = sqrtf(nx*nx+ny*ny+nz*nz);
    if (l < 1e-20f) return;
    nx/=l; ny/=l; nz/=l;
    if (b->n + 3 > b->cap) {
        size_t nc = b->cap ? b->cap*2 : 1024;
        RVertex *nv = (RVertex *)realloc(b->v, nc * sizeof *nv);
        if (!nv) return;
        b->v = nv; b->cap = nc;
    }
    for (int k = 0; k < 3; k++) {
        RVertex *w = &b->v[b->n++];
        w->px=P[k][0]; w->py=P[k][1]; w->pz=P[k][2];
        w->nx=nx; w->ny=ny; w->nz=nz;
        w->u=UV[k][0]; w->v=UV[k][1];
        w->r=C[k][0]/255.0f; w->g=C[k][1]/255.0f; w->b=C[k][2]/255.0f; w->a=C[k][3]/255.0f;
    }
}

/* decode the largest image of a TXD container via libdtex */
static int tex_load_largest(const char *path, unsigned char **rgba, int *w, int *h) {
    FILE *f = fopen(path, "rb"); if (!f) return 1;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if (n<=0){fclose(f);return 1;}
    unsigned char *buf=(unsigned char*)malloc((size_t)n);
    if(!buf||fread(buf,1,(size_t)n,f)!=(size_t)n){free(buf);fclose(f);return 1;}
    fclose(f);
    dtex_set set;
    if (dtex_decode_container(buf,(size_t)n,&set)!=0){free(buf);return 1;}
    int best=-1; long bestpx=-1;
    for (int i=0;i<set.count;i++){ long px=(long)set.items[i].width*set.items[i].height;
        if (px>bestpx){bestpx=px;best=i;} }
    int rc=1;
    if (best>=0){ dtex_image *im=&set.items[best];
        *w=im->width; *h=im->height; *rgba=(unsigned char*)malloc((size_t)*w**h*4);
        if (*rgba){ memcpy(*rgba, im->rgba, (size_t)*w**h*4); rc=0; } }
    dtex_free_set(&set); free(buf);
    return rc;
}

static unsigned char *read_file(const char *path, size_t *sz){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if(n<=0){fclose(f);return NULL;}
    unsigned char *b=(unsigned char*)malloc((size_t)n);
    if(!b||fread(b,1,(size_t)n,f)!=(size_t)n){free(b);fclose(f);return NULL;}
    fclose(f); *sz=(size_t)n; return b;
}

int main(int argc, char **argv) {
    const char *mdir = "../assets/models/out/z143_loader";
    const char *xdir = "/srv/games/dirge-port/extract/_KEL/noPath";
    const char *outdir = "out";
    float dist_override = -1.f, el_override = 10.f;   /* -dist N => interior turntable */
    const char *only = NULL;                          /* -only CID => render one chunk */
    const char *skip = NULL;                          /* -skip "cid,cid" => exclude chunks */
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"-m")&&i+1<argc) mdir=argv[++i];
        else if (!strcmp(argv[i],"-x")&&i+1<argc) xdir=argv[++i];
        else if (!strcmp(argv[i],"-o")&&i+1<argc) outdir=argv[++i];
        else if (!strcmp(argv[i],"-dist")&&i+1<argc) dist_override=(float)atof(argv[++i]);
        else if (!strcmp(argv[i],"-el")&&i+1<argc) el_override=(float)atof(argv[++i]);
        else if (!strcmp(argv[i],"-only")&&i+1<argc) only=argv[++i];
        else if (!strcmp(argv[i],"-skip")&&i+1<argc) skip=argv[++i];
    }

    Bucket bk[MAXFC]; int nb=0;
    int tot_pk=0, tot_bound=0;

    for (int ci=0; ci<NCHUNK; ci++) {
        if (only && strcmp(only,CHUNKS[ci])!=0) continue;
        if (skip && strstr(skip,CHUNKS[ci])) continue;
        char cpath[1024]; snprintf(cpath,sizeof cpath,"%s/%s.bin",xdir,CHUNKS[ci]);
        size_t sz; unsigned char *buf=read_file(cpath,&sz);
        if (!buf){ fprintf(stderr,"skip %s: no file\n",CHUNKS[ci]); continue; }
        long pay=doc_env_find_payload(buf,sz);
        DocEnvPackets ep;
        if (pay<0 || doc_env_decode(buf,sz,pay,&ep)!=0){ fprintf(stderr,"skip %s: decode\n",CHUNKS[ci]); free(buf); continue; }

        char mpath[1024]; snprintf(mpath,sizeof mpath,"%s/%s.manifest",mdir,CHUNKS[ci]);
        DocRoomBinding rb; char err[256]={0};
        int have_bind = (doc_room_binding_load(mpath,&rb,err,sizeof err)==0);
        if (have_bind && rb.npackets != ep.n){
            fprintf(stderr,"%s: packet mismatch bind=%d geom=%d\n",CHUNKS[ci],rb.npackets,ep.n);
            doc_room_binding_free(&rb); have_bind=0;
        }

        for (int i=0;i<ep.n;i++){
            DocEnvPacket *pk=&ep.pk[i];
            const char *fc="";                       /* unbound by default */
            if (have_bind){
                int32_t s=rb.expect_slot[i];
                if (s>=0){ const char *f=doc_room_binding_slot_filecode(&rb,s); if (f) fc=f; }
            }
            Bucket *b=find_bucket(bk,&nb,fc);
            if (!b) continue;
            if (pk->abe) b->abe=1;
            if (fc[0]) tot_bound++;
            int idx[4096*3];
            int nt=doc_env_packet_tris(pk, idx, 4096);
            for (int t=0;t<nt;t++){
                int a=idx[3*t],c=idx[3*t+1],d=idx[3*t+2];
                float P[3][3]={{pk->pos[3*a],pk->pos[3*a+1],pk->pos[3*a+2]},
                               {pk->pos[3*c],pk->pos[3*c+1],pk->pos[3*c+2]},
                               {pk->pos[3*d],pk->pos[3*d+1],pk->pos[3*d+2]}};
                float UV[3][2]={{pk->uv[2*a],pk->uv[2*a+1]},{pk->uv[2*c],pk->uv[2*c+1]},{pk->uv[2*d],pk->uv[2*d+1]}};
                unsigned char C[3][4]={{pk->col[4*a],pk->col[4*a+1],pk->col[4*a+2],pk->col[4*a+3]},
                                       {pk->col[4*c],pk->col[4*c+1],pk->col[4*c+2],pk->col[4*c+3]},
                                       {pk->col[4*d],pk->col[4*d+1],pk->col[4*d+2],pk->col[4*d+3]}};
                push_tri(b,P,UV,C);
            }
            tot_pk++;
        }
        if (have_bind) doc_room_binding_free(&rb);
        doc_env_free(&ep); free(buf);
    }

    /* world bbox + per-bucket textures */
    float rmin[3]={1e30f,1e30f,1e30f}, rmax[3]={-1e30f,-1e30f,-1e30f};
    size_t tottri=0;
    for (int i=0;i<nb;i++){
        for (size_t k=0;k<bk[i].n;k++){ RVertex *w=&bk[i].v[k];
            float p[3]={w->px,w->py,w->pz};
            for (int j=0;j<3;j++){ if(p[j]<rmin[j])rmin[j]=p[j]; if(p[j]>rmax[j])rmax[j]=p[j]; } }
        tottri += bk[i].n/3;
        if (bk[i].fc[0]){
            char tp[1024]; snprintf(tp,sizeof tp,"%s/%s.bin",xdir,bk[i].fc);
            if (tex_load_largest(tp,&bk[i].tex,&bk[i].tw,&bk[i].th))
                fprintf(stderr,"warn: tex decode failed for %s\n",bk[i].fc);
        }
    }
    printf("native room: %d chunks, %d FileCodes, %d/%d packets bound, %zu tris, bbox X[%.0f,%.0f] Y[%.0f,%.0f] Z[%.0f,%.0f]\n",
           NCHUNK, nb, tot_bound, tot_pk, tottri, rmin[0],rmax[0],rmin[1],rmax[1],rmin[2],rmax[2]);
    if (nb==0 || tottri==0){ fprintf(stderr,"nothing to render\n"); return 2; }

    const int W=1280,H=960; char cerr[256]={0};
    gfx_context *ctx=gfx_context_create(W,H,cerr,sizeof cerr);
    if (!ctx){ fprintf(stderr,"GL init failed: %s\n",cerr); return 4; }
    printf("gl    : %s\n", gfx_context_describe(ctx));

    rgl_params p; memset(&p,0,sizeof p);
    p.width=W; p.height=H; p.bilinear=1; p.backface_cull=0;
    p.rig=rgl_default_lightrig(); p.alpha_ref=-1.0f;

    rgl_scene_draw *draws=(rgl_scene_draw*)calloc((size_t)nb,sizeof *draws);
    int ndraws=0;
    for (int i=0;i<nb;i++){
        if (bk[i].n==0) continue;
        draws[ndraws].verts=bk[i].v; draws[ndraws].vcount=bk[i].n;
        draws[ndraws].shade_mode=RGL_SHADE_PRELIT; draws[ndraws].tfx=RGL_TFX_MODULATE;
        draws[ndraws].tex_rgba=bk[i].tex; draws[ndraws].tex_w=bk[i].tw; draws[ndraws].tex_h=bk[i].th;
        ndraws++;
    }

    struct View { char name[16]; float az,el,zoom; };
    struct View ext[]={ {"hero",40.f,18.f,1.25f},{"wide",62.f,26.f,1.55f},{"low",18.f,9.f,1.30f} };
    struct View pano[6];
    struct View *views; int nviews;
    if (dist_override>0){                        /* interior turntable: 6 azimuths */
        for (int i=0;i<6;i++){ snprintf(pano[i].name,sizeof pano[i].name,"pano_%03d",i*60);
            pano[i].az=(float)(i*60); pano[i].el=el_override; pano[i].zoom=dist_override; }
        views=pano; nviews=6;
    } else { views=ext; nviews=3; }
    int fail=0;
    for (int i=0;i<nviews;i++){
        p.angle_deg=views[i].az; p.elev_deg=views[i].el;
        unsigned char *img=NULL; char rerr[256]={0};
        int rc=rgl_render_scene(draws,ndraws,rmin,rmax,views[i].zoom,&p,&img,rerr,sizeof rerr);
        if (rc!=0||!img){ fprintf(stderr,"render %s failed: %s\n",views[i].name,rerr); fail=1; continue; }
        char path[1024]; snprintf(path,sizeof path,"%s/%s_%s.png",outdir,only?only:"room_native",views[i].name);
        if (dtex_write_png(path,img,W,H)==0) printf("wrote %s (az=%.0f el=%.0f dist=%.2f)\n",path,views[i].az,views[i].el,views[i].zoom);
        free(img);
    }
    free(draws);
    for (int i=0;i<nb;i++){ free(bk[i].v); free(bk[i].tex); }
    gfx_context_destroy(ctx);
    return fail;
}
