/* =====================================================================
 * test_hle.c  --  standalone harness for the WS-K HLE layer.
 *
 * Exercises the layer WITHOUT the recompiled game, against synthetic drivers
 * and the real GS backend + native KEL.DAT loader:
 *   (A) counting semaphore shared across two cooperative threads (ping-pong);
 *   (B) synthetic vblank drives a guest INTC handler (AddIntcHandler ->
 *       SignalSema) that releases a WaitSema'ing thread, paced at ~50 Hz;
 *   (C) SIF CD-read RPC HLE delivers bytes byte-identical to kel.c, both via
 *       the low-level sector read and via the {LBA,count,dst} RPC arg block;
 *       plus the fileio path read and the IOPRP-reboot ready handshake;
 *   (D) an EE MMIO store that kicks the GIF DMA channel is parsed by the GS.
 *
 * We supply the recomp-side symbols the contract leaves to generated code
 * (ee_machine_init/free, ee_lookup, ee_call_indirect) with tiny stand-ins so
 * the real syscall/thread/intc code paths run.
 * ===================================================================== */
#define _GNU_SOURCE
#include "ee_runtime.h"
#include "hle.h"
#include "ee_mmio_regs.h"
#include "sif.h"
#include "pad.h"
#include "../gs/gs.h"
#include "kel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/* ---- recomp-side stand-ins (generated in the real build) ------------ */
int ee_machine_init(ee_state *ee) {
    memset(ee, 0, sizeof *ee);
    ee->ram = mmap(NULL, EE_RAM_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    ee->spr = mmap(NULL, EE_SPR_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    return (ee->ram==MAP_FAILED||ee->spr==MAP_FAILED) ? -1 : 0;
}
void ee_machine_free(ee_state *ee) {
    if (ee->ram && ee->ram!=MAP_FAILED) munmap(ee->ram, EE_RAM_SIZE);
    if (ee->spr && ee->spr!=MAP_FAILED) munmap(ee->spr, EE_SPR_SIZE);
}

/* tiny guest function table */
#define MAX_FN 16
static struct { uint32_t addr; ee_func_t fn; } g_fns[MAX_FN];
static int g_nfn;
static void register_fn(uint32_t addr, ee_func_t fn){ g_fns[g_nfn].addr=addr; g_fns[g_nfn].fn=fn; g_nfn++; }
ee_func_t ee_lookup(uint32_t a){ for(int i=0;i<g_nfn;i++) if(g_fns[i].addr==a) return g_fns[i].fn; return NULL; }
void ee_call_indirect(ee_state *ee, uint32_t a){ ee_func_t f=ee_lookup(a); if(f) f(ee); }

static double now_sec(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec*1e-9; }

/* ============================= TEST A ============================= */
/* Two cooperative threads ping-pong through two counting semaphores: the
 * producer signals semB then blocks on semA; the consumer blocks on semB
 * then signals semA.  Proves cross-thread block/wake over the sema model. */
static int A_p_count, A_c_count;
static int A_semA, A_semB, A_rounds;
static hle_ctx *A_h;
static void A_producer(hle_ctx *h, void *arg){ (void)arg;
    for (int i=0;i<A_rounds;i++){ hle_sema_signal(h, A_semB); hle_sema_wait(h, A_semA); A_p_count++; }
}
static void A_consumer(hle_ctx *h, void *arg){ (void)arg;
    for (int i=0;i<A_rounds;i++){ hle_sema_wait(h, A_semB); A_c_count++; hle_sema_signal(h, A_semA); }
}

static int test_A(void){
    printf("== (A) counting semaphore across two cooperative threads ==\n");
    ee_state ee; ee_machine_init(&ee);
    hle_ctx *h = hle_create(&ee);
    A_h=h; A_rounds=8; A_p_count=A_c_count=0;
    A_semA = hle_sema_create(h, 0, 1);
    A_semB = hle_sema_create(h, 0, 1);
    int tp = hle_thread_create_native(h, A_producer, NULL, 0);
    int tc = hle_thread_create_native(h, A_consumer, NULL, 0);
    hle_thread_start(h, tp, 0);
    hle_thread_start(h, tc, 0);
    hle_run(h, 100);
    int ok = (A_p_count==A_rounds) && (A_c_count==A_rounds);
    printf("   producer rounds=%d consumer rounds=%d (expected %d)  %s\n",
           A_p_count, A_c_count, A_rounds, ok?"OK":"**FAIL**");
    (void)tp; (void)tc;
    hle_destroy(h); ee_machine_free(&ee);
    return ok?0:1;
}

/* ============================= TEST B ============================= */
/* A guest INTC handler (as the recompiled game would provide): reads the
 * sema id from its $a1 arg and SignalSema's it via the real syscall path. */
#define B_HANDLER_ADDR 0x00401000u
static void B_guest_vblank_handler(ee_state *ee){
    uint32_t sema = EE_GET32(ee, R_A1);     /* arg passed at AddIntcHandler */
    ee->r[R_A0].ud[0] = sema;
    ee->r[R_V1].ud[0] = 0x42;               /* SignalSema */
    ee_syscall(ee, 0);
}
static int B_sema, B_got, B_target;
static void B_waiter(hle_ctx *h, void *arg){ (void)arg;
    for (int i=0;i<B_target;i++){ hle_sema_wait(h, B_sema); B_got++; }
}
static int test_B(void){
    printf("== (B) synthetic vblank -> guest INTC handler -> WaitSema release (paced 50 Hz) ==\n");
    ee_state ee; ee_machine_init(&ee);
    hle_ctx *h = hle_create(&ee);
    register_fn(B_HANDLER_ADDR, B_guest_vblank_handler);
    B_sema = hle_sema_create(h, 0, 1);
    B_target = 50; B_got = 0;

    /* AddIntcHandler(cause=VBLANK_START, handler, next=0, arg=sema) via syscall */
    ee.r[R_A0].ud[0]=INTC_VBLANK_START; ee.r[R_A1].ud[0]=B_HANDLER_ADDR;
    ee.r[R_A2].ud[0]=0; ee.r[R_A3].ud[0]=(uint32_t)B_sema; ee.r[R_V1].ud[0]=0x10;
    ee_syscall(&ee, 0);

    /* SetGsCrt(interlace=1, mode=3=PAL, field=0) -> pace at 50 Hz */
    ee.r[R_A0].ud[0]=1; ee.r[R_A1].ud[0]=3; ee.r[R_A2].ud[0]=0; ee.r[R_V1].ud[0]=2;
    ee_syscall(&ee, 0);

    int tw = hle_thread_create_native(h, B_waiter, NULL, 0);
    hle_thread_start(h, tw, 0);

    double t0 = now_sec();
    uint64_t vb = hle_run(h, 200);
    double dt = now_sec() - t0;

    double hz = hle_get_vblank_hz(h);
    int ok = (B_got==B_target) && (vb>=(uint64_t)B_target) && (dt>0.80 && dt<1.40);
    printf("   released=%d/%d  vblanks=%llu  hz=%.3f  elapsed=%.3fs (expect ~1.0s)  %s\n",
           B_got, B_target, (unsigned long long)vb, hz, dt, ok?"OK":"**FAIL**");
    hle_destroy(h); ee_machine_free(&ee);
    return ok?0:1;
}

/* ============================= TEST C ============================= */
static const char *FL_DEFAULT = "/srv/games/dirge-port/extract/FILELIST.BIN";
static const char *DAT_DEFAULT = "/srv/games/dirge-port/extract/KEL.DAT";

static int test_C(const char *flpath, const char *datpath){
    printf("== (C) SIF CD-read RPC HLE vs kel.c (byte-exact) + fileio + IOP reboot ==\n");
    kel_archive a;
    if (kel_open(&a, flpath, datpath)!=0){ printf("   SKIP: cannot open %s / %s\n", flpath, datpath); return 0; }

    /* find a stored-raw entry (usize==csize) with a path, not the last one,
     * and small enough to compare quickly. */
    const kel_entry *e = NULL;
    for (uint32_t i=0;i<a.count;i++){
        const kel_entry *c=&a.entries[i];
        if (c->path[0] && c->usize==c->csize && c->usize>2048 && c->usize<0x40000){ e=c; break; }
    }
    if (!e){ printf("   SKIP: no suitable stored-raw entry\n"); kel_close(&a); return 0; }
    printf("   using %s code=%08x pos=%x size=%x\n", e->path, e->filecode, e->pos, e->usize);

    uint8_t *ref=NULL; long rn = kel_read(&a, e, &ref);
    if (rn<0){ printf("   **FAIL** kel_read\n"); kel_close(&a); return 1; }

    ee_state ee; ee_machine_init(&ee);
    hle_ctx *h = hle_create(&ee);
    sif_dev *s = sif_create(&ee);
    sif_set_kel(s, &a, /*kel_origin=*/0);   /* LBAs are KEL.DAT-relative here */
    hle_set_sif(h, s);

    int fails=0;
    uint32_t count = (e->csize + 0x7ff)/0x800;   /* whole sectors */

    /* (C1) low-level sector read into guest RAM */
    uint32_t dst1 = 0x00200000u;
    long got1 = sif_cd_read(s, e->pos, count, dst1);
    int ok1 = (got1 >= (long)e->csize) &&
              (memcmp(ee.ram+dst1, ref, e->csize)==0);
    printf("   C1 sif_cd_read      -> %ld bytes, first %x match kel.c: %s\n",
           got1, e->usize, ok1?"OK":"**FAIL**"); fails += !ok1;

    /* (C2) via the RPC arg block {LBA@+0,count@+4,dst@+8} (FUN_00446af0 shape) */
    uint32_t argblk = 0x00100000u, dst2 = 0x00280000u;
    ee_sw(&ee, argblk+0x00, e->pos);
    ee_sw(&ee, argblk+0x04, count);
    ee_sw(&ee, argblk+0x08, dst2);
    int rc = sif_call_rpc(s, SIF_RPC_CD, 1, argblk, 0x18, 0, 0, /*sema*/-1);
    int ok2 = (rc==0) && (memcmp(ee.ram+dst2, ref, e->csize)==0);
    printf("   C2 sif_call_rpc #1  -> rc=%d, first %x match kel.c: %s\n",
           rc, e->usize, ok2?"OK":"**FAIL**"); fails += !ok2;

    /* (C3) fileio path read (decompresses via kel.c for compressed entries) */
    uint32_t dst3 = 0x00300000u;
    long got3 = sif_fileio_read_path(s, e->path, dst3, e->usize);
    int ok3 = (got3==(long)e->usize) && (memcmp(ee.ram+dst3, ref, e->usize)==0);
    printf("   C3 fileio read      -> %ld bytes match kel.c: %s\n", got3, ok3?"OK":"**FAIL**");
    fails += !ok3;

    /* (C4) IOPRP reboot -> IOP ready handshake (SMFLAG bit 0x40000) */
    int before = sif_iop_ready(s);
    sif_reboot_iop(s);
    uint32_t smflag = sif_get_reg(s, 4);
    int ok4 = (!before) && sif_iop_ready(s) && (smflag & 0x40000);
    printf("   C4 IOP reboot ready -> SMFLAG=%08x: %s\n", smflag, ok4?"OK":"**FAIL**");
    fails += !ok4;

    free(ref);
    hle_destroy(h); ee_machine_free(&ee); sif_destroy(s); kel_close(&a);
    return fails?1:0;
}

/* ============================= TEST D ============================= */
static int test_D(void){
    printf("== (D) EE MMIO store kicks GIF DMA channel -> GS parses the packet ==\n");
    ee_state ee; ee_machine_init(&ee);
    hle_ctx *h = hle_create(&ee);
    gs_state *gs = gs_create(gs_backend_log(), 640, 448);
    hle_set_gs(h, gs);

    /* Build a 2-qword GIF A+D packet in guest RAM: write FRAME_1 (reg 0x4c)
     * with fbp=0x50, fbw=10.  GIFtag: NLOOP=1 EOP=1 NREG=1 REGS[0]=A+D(0xE). */
    uint32_t pkt = 0x00200000u;
    uint64_t tag_lo = 1u | (1u<<15) | ((uint64_t)1<<60);
    uint64_t tag_hi = 0xEull;
    uint64_t ad_lo  = 0x50u | (10u<<16);     /* FRAME value */
    uint64_t ad_hi  = 0x4cull;               /* FRAME_1 register addr */
    ee_sd(&ee, pkt+0,  tag_lo); ee_sd(&ee, pkt+8,  tag_hi);
    ee_sd(&ee, pkt+16, ad_lo);  ee_sd(&ee, pkt+24, ad_hi);

    /* Program the GIF DMAC channel and kick it (FUN_00442100: CHCR=0x101). */
    ee_sw(&ee, DMAC_CH_GIF+DMAC_REG_MADR, pkt);
    ee_sw(&ee, DMAC_CH_GIF+DMAC_REG_QWC,  2);
    ee_sw(&ee, DMAC_CH_GIF+DMAC_REG_CHCR, 0x101);   /* STR | normal */

    const gs_frame *fr = gs_cur_frame(gs);
    uint32_t chcr_after = ee_lw(&ee, DMAC_CH_GIF+DMAC_REG_CHCR);
    int ok = (fr && fr->fbp==0x50) && !(chcr_after & 0x100);
    printf("   GS FRAME.fbp=%x (expect 50), CHCR STR cleared=%d  %s\n",
           fr?fr->fbp:0xffffffff, !(chcr_after&0x100), ok?"OK":"**FAIL**");

    gs_destroy(gs); hle_destroy(h); ee_machine_free(&ee);
    return ok?0:1;
}

int main(int argc, char **argv){
    const char *fl  = argc>1 ? argv[1] : FL_DEFAULT;
    const char *dat = argc>2 ? argv[2] : DAT_DEFAULT;
    int fails = 0;
    fails += test_A();
    fails += test_B();
    fails += test_C(fl, dat);
    fails += test_D();
    printf("\n%s  (%d failing group%s)\n", fails?"FAILURES":"ALL PASS",
           fails, fails==1?"":"s");
    return fails?1:0;
}
