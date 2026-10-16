/* backend_log.c -- headless stub backend.
 *
 * Implements gs_backend by printing every decoded draw call, VRAM transfer
 * and display/flip to stdout.  Needs no GPU and no window system, so it runs
 * on this headless server and proves the command-parsing half end to end.
 * The real OpenGL/Vulkan/software backends will implement the same vtable.
 */
#include "gs.h"
#include <stdio.h>

static const char *prim_name(enum gs_prim_type p) {
    switch (p) {
    case GS_PRIM_POINT:      return "POINT";
    case GS_PRIM_LINE:       return "LINE";
    case GS_PRIM_LINE_STRIP: return "LINE_STRIP";
    case GS_PRIM_TRI:        return "TRIANGLE";
    case GS_PRIM_TRI_STRIP:  return "TRI_STRIP";
    case GS_PRIM_TRI_FAN:    return "TRI_FAN";
    case GS_PRIM_SPRITE:     return "SPRITE";
    default:                 return "INVALID";
    }
}

static int log_init(void *self, int w, int h) {
    (void)self;
    printf("[gs/log] init %dx%d\n", w, h);
    return 0;
}

static void log_draw(void *self, const gs_draw *d) {
    (void)self;
    const gs_tex0  *t = gs_cur_tex0(d->st);
    const gs_alpha *a = gs_cur_alpha(d->st);
    printf("[gs/log] %s  iip=%u tme=%u abe=%u fst=%u verts=%d\n",
           prim_name(d->prim), d->iip, d->tme, d->abe, d->fst, d->nverts);
    for (int i = 0; i < d->nverts; i++) {
        const gs_vertex *v = &d->verts[i];
        if (v->uv_mode)
            printf("           v%d (%.1f,%.1f) z=%u rgba=%02x%02x%02x%02x uv=(%u,%u)\n",
                   i, v->x, v->y, v->z, v->r, v->g, v->b, v->a, v->u, v->v);
        else
            printf("           v%d (%.1f,%.1f) z=%u rgba=%02x%02x%02x%02x st=(%.3f,%.3f)\n",
                   i, v->x, v->y, v->z, v->r, v->g, v->b, v->a, v->s, v->t);
    }
    if (d->tme)
        printf("           tex: tbp0=0x%x tbw=%u psm=0x%x size=%ux%u tcc=%u cbp=0x%x\n",
               t->tbp0, t->tbw, t->psm, 1u << t->tw, 1u << t->th, t->tcc, t->cbp);
    if (d->abe)
        printf("           alpha: (A=%u B=%u C=%u D=%u) FIX=%u\n",
               a->A, a->B, a->C, a->D, a->FIX);
}

static void log_transfer(void *self, const gs_tex_transfer *x) {
    (void)self;
    const char *dir = x->dir == 0 ? "HOST->LOCAL" :
                      x->dir == 1 ? "LOCAL->HOST" :
                      x->dir == 2 ? "LOCAL->LOCAL" : "DEACT";
    printf("[gs/log] XFER %s  dst(bp=0x%x bw=%u psm=0x%x @%u,%u) %ux%u  %zu bytes\n",
           dir, x->dbp, x->dbw, x->dpsm, x->dsax, x->dsay,
           x->rrw, x->rrh, x->data_len);
}

static void log_display(void *self, const gs_display *disp) {
    (void)self;
    printf("[gs/log] DISPLAY pmode=0x%llx dispfb1=0x%llx display1=0x%llx\n",
           (unsigned long long)disp->pmode,
           (unsigned long long)disp->dispfb1,
           (unsigned long long)disp->display1);
}

static void log_flip(void *self)     { (void)self; printf("[gs/log] FLIP\n"); }
static void log_shutdown(void *self) { (void)self; printf("[gs/log] shutdown\n"); }

static gs_backend g_log = {
    .name = "log",
    .self = NULL,
    .init = log_init,
    .draw = log_draw,
    .transfer = log_transfer,
    .display = log_display,
    .flip = log_flip,
    .shutdown = log_shutdown,
};

const gs_backend *gs_backend_log(void) { return &g_log; }
