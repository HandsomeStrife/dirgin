/* room_live.c -- the recovered z143 room rendered through the LIVE native engine
 * FRAME LOOP (doc_engine_frame), not a standalone render call.
 *
 * The existing convergence capstone (native/capstone) drives the GPU from a FROZEN
 * COPY of the engine (engine_snapshot/). This instead links the LIVE doc_engine
 * (libdoc_engine.a: the real frame loop + state machine + render-queue + material
 * subsystem) and drives it: each doc_engine_frame() locks the frame, runs the state
 * dispatch + object list, calls our DocRenderer.submit_scene (where we draw the
 * recovered room through renderer_gl), then present (FBO -> PNG). The room content
 * is the native stack: doc_env geometry + doc_room_binding + libdtex textures.
 *
 * Proves the shipping engine's frame loop renders real decoded content end to end.
 *
 *   room_live [-m MANIFESTDIR] [-x EXTRACTDIR] [-o OUTDIR] [-frames N] [-dist f]
 */
#include "frame_loop.h"       /* DocEngine, DocRenderer, doc_engine_frame (live) */
#include "scene_graph.h"      /* DocRenderQueue */
#include "engine_state.h"     /* engine_hooks_default */
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
    "068f0712","068f0912","068f0a12","068f0b12","068f0c12","068f0d12" };
#define NCHUNK ((int)(sizeof CHUNKS / sizeof CHUNKS[0]))

typedef struct { char fc[9]; RVertex *v; size_t n, cap; unsigned char *tex; int tw, th; } Bucket;

typedef struct {
    gfx_context    *ctx;
    rgl_scene_draw *draws; int ndraws;
    Bucket         *bk; int nb;
    float           rmin[3], rmax[3];
    rgl_params      rp;
    const char     *outdir;
    float           dist;
    int             frame_index;
    unsigned char  *pending;
    DocEngine      *engine;
} RoomApp;

/* ---- room loading (native stack): doc_env geometry + loader binding + textures -- */
static Bucket *find_bucket(Bucket *bk, int *nb, const char *fc) {
    for (int i = 0; i < *nb; i++) if (!strcmp(bk[i].fc, fc)) return &bk[i];
    if (*nb >= MAXFC) return NULL;
    Bucket *b = &bk[(*nb)++]; memset(b, 0, sizeof *b); snprintf(b->fc, 9, "%s", fc); return b;
}
static void push_tri(Bucket *b, const float P[3][3], const float UV[3][2], const unsigned char C[3][4]) {
    float e1[3]={P[1][0]-P[0][0],P[1][1]-P[0][1],P[1][2]-P[0][2]};
    float e2[3]={P[2][0]-P[0][0],P[2][1]-P[0][1],P[2][2]-P[0][2]};
    float nx=e1[1]*e2[2]-e1[2]*e2[1],ny=e1[2]*e2[0]-e1[0]*e2[2],nz=e1[0]*e2[1]-e1[1]*e2[0];
    float l=sqrtf(nx*nx+ny*ny+nz*nz); if(l<1e-20f) return; nx/=l;ny/=l;nz/=l;
    if(b->n+3>b->cap){ size_t nc=b->cap?b->cap*2:1024; RVertex*nv=realloc(b->v,nc*sizeof*nv); if(!nv)return; b->v=nv;b->cap=nc; }
    for(int k=0;k<3;k++){ RVertex*w=&b->v[b->n++];
        w->px=P[k][0];w->py=P[k][1];w->pz=P[k][2]; w->nx=nx;w->ny=ny;w->nz=nz;
        w->u=UV[k][0];w->v=UV[k][1]; w->r=C[k][0]/255.f;w->g=C[k][1]/255.f;w->b=C[k][2]/255.f;w->a=C[k][3]/255.f; }
}
static int tex_load_largest(const char*path,unsigned char**rgba,int*w,int*h){
    FILE*f=fopen(path,"rb"); if(!f)return 1; fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    if(n<=0){fclose(f);return 1;} unsigned char*buf=malloc((size_t)n);
    if(!buf||fread(buf,1,(size_t)n,f)!=(size_t)n){free(buf);fclose(f);return 1;} fclose(f);
    dtex_set set; if(dtex_decode_container(buf,(size_t)n,&set)!=0){free(buf);return 1;}
    int best=-1;long bp=-1; for(int i=0;i<set.count;i++){long px=(long)set.items[i].width*set.items[i].height; if(px>bp){bp=px;best=i;}}
    int rc=1; if(best>=0){dtex_image*im=&set.items[best];*w=im->width;*h=im->height;*rgba=malloc((size_t)*w**h*4);
        if(*rgba){memcpy(*rgba,im->rgba,(size_t)*w**h*4);rc=0;}} dtex_free_set(&set);free(buf);return rc;
}
static unsigned char *read_file(const char*p,size_t*sz){FILE*f=fopen(p,"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);if(n<=0){fclose(f);return NULL;}
    unsigned char*b=malloc((size_t)n);if(!b||fread(b,1,(size_t)n,f)!=(size_t)n){free(b);fclose(f);return NULL;}fclose(f);*sz=(size_t)n;return b;}

static int load_room(RoomApp *a, const char *mdir, const char *xdir) {
    a->bk = calloc(MAXFC, sizeof(Bucket)); a->nb = 0;
    int bound=0, pk_total=0;
    for (int ci=0; ci<NCHUNK; ci++) {
        char cpath[1024]; snprintf(cpath,sizeof cpath,"%s/%s.bin",xdir,CHUNKS[ci]);
        size_t sz; unsigned char *buf=read_file(cpath,&sz); if(!buf) continue;
        long pay=doc_env_find_payload(buf,sz); DocEnvPackets ep;
        if(pay<0||doc_env_decode(buf,sz,pay,&ep)!=0){ free(buf); continue; }
        char mpath[1024]; snprintf(mpath,sizeof mpath,"%s/%s.manifest",mdir,CHUNKS[ci]);
        DocRoomBinding rb; char err[256]={0};
        int have=(doc_room_binding_load(mpath,&rb,err,sizeof err)==0);
        if(have && rb.npackets!=ep.n){ doc_room_binding_free(&rb); have=0; }
        for(int i=0;i<ep.n;i++){ DocEnvPacket*pk=&ep.pk[i]; const char*fc="";
            if(have){ int32_t s=rb.expect_slot[i]; if(s>=0){const char*f=doc_room_binding_slot_filecode(&rb,s); if(f)fc=f;} }
            Bucket*b=find_bucket(a->bk,&a->nb,fc); if(!b)continue; if(fc[0])bound++;
            int idx[4096*3]; int nt=doc_env_packet_tris(pk,idx,4096);
            for(int t=0;t<nt;t++){ int x=idx[3*t],y=idx[3*t+1],z=idx[3*t+2];
                float P[3][3]={{pk->pos[3*x],pk->pos[3*x+1],pk->pos[3*x+2]},{pk->pos[3*y],pk->pos[3*y+1],pk->pos[3*y+2]},{pk->pos[3*z],pk->pos[3*z+1],pk->pos[3*z+2]}};
                float UV[3][2]={{pk->uv[2*x],pk->uv[2*x+1]},{pk->uv[2*y],pk->uv[2*y+1]},{pk->uv[2*z],pk->uv[2*z+1]}};
                unsigned char C[3][4]={{pk->col[4*x],pk->col[4*x+1],pk->col[4*x+2],pk->col[4*x+3]},{pk->col[4*y],pk->col[4*y+1],pk->col[4*y+2],pk->col[4*y+3]},{pk->col[4*z],pk->col[4*z+1],pk->col[4*z+2],pk->col[4*z+3]}};
                push_tri(b,P,UV,C); }
            pk_total++;
        }
        if(have) doc_room_binding_free(&rb);
        doc_env_free(&ep);
        free(buf);
    }
    for(int k=0;k<3;k++){a->rmin[k]=1e30f;a->rmax[k]=-1e30f;}
    for(int i=0;i<a->nb;i++){ for(size_t k=0;k<a->bk[i].n;k++){RVertex*w=&a->bk[i].v[k];
            float p[3]={w->px,w->py,w->pz}; for(int j=0;j<3;j++){if(p[j]<a->rmin[j])a->rmin[j]=p[j];if(p[j]>a->rmax[j])a->rmax[j]=p[j];}}
        if(a->bk[i].fc[0]){ char tp[1024]; snprintf(tp,sizeof tp,"%s/%s.bin",xdir,a->bk[i].fc);
            tex_load_largest(tp,&a->bk[i].tex,&a->bk[i].tw,&a->bk[i].th); } }
    a->draws=calloc((size_t)(a->nb>0?a->nb:1),sizeof(rgl_scene_draw)); a->ndraws=0;
    for(int i=0;i<a->nb;i++){ if(!a->bk[i].n)continue;
        a->draws[a->ndraws].verts=a->bk[i].v; a->draws[a->ndraws].vcount=a->bk[i].n;
        a->draws[a->ndraws].shade_mode=RGL_SHADE_PRELIT; a->draws[a->ndraws].tfx=RGL_TFX_MODULATE;
        a->draws[a->ndraws].tex_rgba=a->bk[i].tex; a->draws[a->ndraws].tex_w=a->bk[i].tw; a->draws[a->ndraws].tex_h=a->bk[i].th; a->ndraws++; }
    printf("room loaded (native): %d FileCodes, %d/%d packets bound\n", a->nb, bound, pk_total);
    return a->ndraws>0 ? 0 : 1;
}

/* ---- DocRenderer seam: the frame loop calls these ---------------------------- */
static void cap_begin(void *c){ (void)c; }
static void cap_end(void *c){ (void)c; }
static void cap_submit(void *c, DocRenderQueue *q){
    RoomApp *a=(RoomApp*)c; (void)q;              /* the loop populated q just before */
    a->rp.angle_deg = 20.f + a->frame_index*30.f; /* turntable: one azimuth per frame */
    a->rp.elev_deg  = 10.f;
    unsigned char *img=NULL; char err[256]={0};
    int rc=rgl_render_scene(a->draws,a->ndraws,a->rmin,a->rmax,a->dist,&a->rp,&img,err,sizeof err);
    a->pending = (rc==0)?img:NULL;
    if(rc) fprintf(stderr,"submit: render failed: %s\n",err);
}
static void cap_present(void *c, int mode){
    RoomApp *a=(RoomApp*)c; if(!a->pending) return;
    char path[1024]; snprintf(path,sizeof path,"%s/room_live_%02d.png",a->outdir,a->frame_index);
    if(dtex_write_png(path,a->pending,a->rp.width,a->rp.height)==0)
        printf("  frame %d (mode %d) -> %s  [drawn by doc_engine_frame]\n",a->frame_index,mode,path);
    free(a->pending); a->pending=NULL; a->frame_index++;
}
static int cap_pending(void *c){ return ((RoomApp*)c)->pending!=NULL; }

int main(int argc, char **argv){
    const char *mdir="../assets/models/out/z143_loader_heur";
    const char *xdir="/srv/games/dirge-port/extract/_KEL/noPath";
    RoomApp a; memset(&a,0,sizeof a); a.outdir="out"; a.dist=1.3f;
    int frames=3;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-m")&&i+1<argc)mdir=argv[++i];
        else if(!strcmp(argv[i],"-x")&&i+1<argc)xdir=argv[++i];
        else if(!strcmp(argv[i],"-o")&&i+1<argc)a.outdir=argv[++i];
        else if(!strcmp(argv[i],"-frames")&&i+1<argc)frames=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-dist")&&i+1<argc)a.dist=(float)atof(argv[++i]);
    }
    if(load_room(&a,mdir,xdir)!=0){ fprintf(stderr,"room load failed\n"); return 2; }

    const int W=1280,H=960; char cerr[256]={0};
    a.ctx=gfx_context_create(W,H,cerr,sizeof cerr);
    if(!a.ctx){ fprintf(stderr,"GL init failed: %s\n",cerr); return 4; }
    printf("gl   : %s\n", gfx_context_describe(a.ctx));
    a.rp.width=W; a.rp.height=H; a.rp.bilinear=1; a.rp.backface_cull=0;
    a.rp.rig=rgl_default_lightrig(); a.rp.alpha_ref=-1.0f;

    /* wire the LIVE engine: default hooks + our render seam */
    DocRenderer   rnd = doc_renderer_default();
    rnd.ctx=&a; rnd.begin_frame=cap_begin; rnd.submit_scene=cap_submit;
    rnd.end_frame=cap_end; rnd.present=cap_present; rnd.pending_query=cap_pending;
    EngineHooks   eh  = engine_hooks_default();
    DocFrameHooks fh  = doc_frame_hooks_default();
    DocRenderQueue rq; memset(&rq,0,sizeof rq);
    DocEngine eng; a.engine=&eng;
    doc_engine_init(&eng,&eh,&rnd,&fh,&rq);

    printf("driving the LIVE engine frame loop for %d frames...\n",frames);
    doc_engine_frame_preamble(&eng);
    for(int f=0; f<frames; f++){
        DocFrameResult r = doc_engine_frame(&eng);
        if(r==DOC_FRAME_STOP){ printf("  frame loop exit at %d\n",f); break; }
    }
    printf("done: %d frames presented by the live engine.\n", a.frame_index);

    for(int i=0;i<a.nb;i++){ free(a.bk[i].v); free(a.bk[i].tex); }
    free(a.bk); free(a.draws); gfx_context_destroy(a.ctx);
    return 0;
}
