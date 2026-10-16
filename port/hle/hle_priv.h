/* =====================================================================
 * hle_priv.h  --  internal HLE state, shared between kernel.c and mmio.c.
 * NOT part of any public contract; WS-K private.
 * ===================================================================== */
#ifndef DIRGE_HLE_PRIV_H
#define DIRGE_HLE_PRIV_H

#include <ucontext.h>
#include <time.h>
#include "hle.h"
#include "ee_mmio_regs.h"

#define HLE_MAX_THREADS   64
#define HLE_MAX_SEMAS     512
#define HLE_MAX_INTC      16     /* handlers per cause chain */
#define HLE_STACK_BYTES   (256 * 1024)

/* Snapshot of the EE register file for a cooperative context switch.
 * Only one guest thread runs at a time, but each carries its own GPR/FPU
 * values across a blocking kernel call, so we save/restore on every switch. */
typedef struct {
    ee_reg   r[32];
    ee_reg   lo, hi;
    uint32_t sa;
    uint32_t f[32];
    uint32_t fcr31, fcr0, pc;
} ee_regsave;

typedef enum {
    TH_FREE = 0, TH_DORMANT, TH_READY, TH_RUNNING,
    TH_BLOCKED_SEMA, TH_SLEEP, TH_EXITED
} th_state;

typedef struct hle_thread {
    th_state   state;
    int        tid;            /* 1-based public id                       */
    int        priority;
    ucontext_t uc;
    void      *stack;
    ee_regsave rs;

    /* guest-thread params */
    uint32_t   entry, gp, stack_top, arg;
    int        is_native;
    void     (*nfn)(hle_ctx*, void*);
    void      *narg;

    int        wait_sema;      /* sema id if BLOCKED_SEMA, else -1        */
    int        wakeup_pending; /* SleepThread/WakeupThread race latch     */

    int        q_next;         /* intrusive queue link (thread index)     */
} hle_thread;

typedef struct hle_sema {
    int in_use;
    int count;
    int max_count;
    int wait_head, wait_tail;  /* thread-index FIFO, -1 empty             */
} hle_sema;

typedef struct hle_intc_slot {
    int      used;
    int      is_native;
    uint32_t guest_handler;    /* guest addr (ee_lookup)                  */
    uint32_t guest_arg;
    hle_intc_fn nfn;
    void    *narg;
} hle_intc_slot;

struct hle_ctx {
    ee_state *ee;
    struct gs_state  *gs;
    struct sif_dev   *sif;
    struct pad_state *pad;
    hle_vif1_fn vif1_cb; void *vif1_user;

    hle_thread threads[HLE_MAX_THREADS];
    int        current;        /* index of running thread, -1 in scheduler */
    ucontext_t sched_uc;
    int        ready_head, ready_tail;

    hle_sema   semas[HLE_MAX_SEMAS];

    hle_intc_slot intc[16][HLE_MAX_INTC];
    uint32_t   intc_stat, intc_mask;

    /* vblank / video */
    double     vblank_hz;
    uint64_t   vblank_count;
    uint32_t   gs_imr;
    int        vsync_flag;
    int        video_mode;     /* SetGsCrt mode: 2=NTSC 3=PAL             */

    /* shutdown */
    int        exit_requested;
    uint32_t   loadexec_target;

    /* run bound bookkeeping + real-time pacing state */
    uint64_t   run_max_vblanks;
    struct timespec paced_next;
    int        paced_armed;

    /* DMAC shadow register file: channels 0x8000..0xDFFF (one word each),
     * indexed by (addr - 0x10008000)/4; plus common regs tracked by addr. */
    uint32_t   dmac_chan[0x6000/4];
    uint32_t   dmac_ctrl, dmac_stat, dmac_rbor, dmac_rbsr, dmac_sqwc;

    /* GS privileged shadow (fed to gs_write_display) */
    uint64_t   gs_pmode, gs_smode2, gs_dispfb1, gs_display1,
               gs_dispfb2, gs_display2, gs_bgcolor, gs_extdata, gs_csr;
};

/* kernel.c internals used by mmio.c */
void     hle_run_guest_intc(hle_ctx *h, int cause);  /* run handlers now  */

/* mmio.c entry (called from ee_mmio_* hooks). */
void     hle_mmio_store(hle_ctx *h, uint32_t addr, int width, uint64_t v,
                        const ee_reg *q);
uint64_t hle_mmio_load (hle_ctx *h, uint32_t addr, int width);

#endif /* DIRGE_HLE_PRIV_H */
