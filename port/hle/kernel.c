/* =====================================================================
 * kernel.c  --  HLE EE kernel: cooperative scheduler, counting semaphores,
 * threads, INTC handlers, synthetic vblank source, and ee_syscall()
 * dispatch.  Implements the PS2 EE BIOS calls Dirge of Cerberus uses
 * (evidence: analysis/decomp libkernel wrappers at 0x00451ae0..0x00452310).
 *
 * Cooperative model: each guest thread owns a host ucontext (its own host
 * stack), so recompiled code may block mid-call in WaitSema/SleepThread and
 * the scheduler simply swapcontext()es away.  Because the register file in
 * ee_state is shared, it is snapshotted per switch (ee_regsave).
 * ===================================================================== */
#define _GNU_SOURCE
#include "hle_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The trampoline recovers the running ctx here (single host thread). */
static hle_ctx *g_cur_ctx;

/* SIF register model (sif/sif.c) -- weak so hle/ links without sif too. */
struct sif_dev;
void     sif_set_reg(struct sif_dev*, uint32_t, uint32_t) __attribute__((weak));
uint32_t sif_get_reg(struct sif_dev*, uint32_t)           __attribute__((weak));
void     sif_dev_reboot_iop(struct sif_dev*)              __attribute__((weak));

/* ------------------------------------------------------------------ *
 * register save/restore for context switches                         *
 * ------------------------------------------------------------------ */
static void regs_save(ee_state *ee, ee_regsave *s) {
    memcpy(s->r, ee->r, sizeof s->r);
    s->lo = ee->lo; s->hi = ee->hi; s->sa = ee->sa;
    memcpy(s->f, ee->f, sizeof s->f);
    s->fcr31 = ee->fcr31; s->fcr0 = ee->fcr0; s->pc = ee->pc;
}
static void regs_restore(ee_state *ee, const ee_regsave *s) {
    memcpy(ee->r, s->r, sizeof s->r);
    ee->lo = s->lo; ee->hi = s->hi; ee->sa = s->sa;
    memcpy(ee->f, s->f, sizeof s->f);
    ee->fcr31 = s->fcr31; ee->fcr0 = s->fcr0; ee->pc = s->pc;
    ee->r[0].ud[0] = ee->r[0].ud[1] = 0; /* $zero stays 0 */
}

/* ------------------------------------------------------------------ *
 * ready queue (FIFO, priority used only as a tie-stable ordering)    *
 * ------------------------------------------------------------------ */
static void ready_push(hle_ctx *h, int ti) {
    h->threads[ti].q_next = -1;
    h->threads[ti].state = TH_READY;
    if (h->ready_tail < 0) h->ready_head = h->ready_tail = ti;
    else { h->threads[h->ready_tail].q_next = ti; h->ready_tail = ti; }
}
static int ready_pop(hle_ctx *h) {
    int ti = h->ready_head;
    if (ti < 0) return -1;
    h->ready_head = h->threads[ti].q_next;
    if (h->ready_head < 0) h->ready_tail = -1;
    h->threads[ti].q_next = -1;
    return ti;
}

/* ------------------------------------------------------------------ *
 * thread context entry trampoline                                    *
 * ------------------------------------------------------------------ */
static void thread_trampoline(void) {
    hle_ctx *h = g_cur_ctx;
    hle_thread *t = &h->threads[h->current];
    if (t->is_native) {
        t->nfn(h, t->narg);
    } else {
        ee_func_t f = ee_lookup(t->entry);
        if (!f) {
            fprintf(stderr, "[hle] thread %d: entry %08x not in function table\n",
                    t->tid, t->entry);
        } else {
            /* $a0 = start argument, $sp/$gp as the thread params asked. */
            h->ee->r[R_A0].ud[0] = (int32_t)t->arg;
            if (t->stack_top) h->ee->r[R_SP].ud[0] = (int32_t)t->stack_top;
            if (t->gp)        h->ee->r[R_GP].ud[0] = (int32_t)t->gp;
            f(h->ee);
        }
    }
    /* Falling off the end == ExitThread.  uc_link returns us to sched_uc;
     * the scheduler sees state==RUNNING and reaps it. */
    t->state = TH_EXITED;
}

/* ------------------------------------------------------------------ *
 * low-level switch primitives                                        *
 * ------------------------------------------------------------------ */
static void run_thread(hle_ctx *h, int ti) {
    hle_thread *t = &h->threads[ti];
    g_cur_ctx = h;
    regs_restore(h->ee, &t->rs);
    h->current = ti;
    t->state = TH_RUNNING;
    swapcontext(&h->sched_uc, &t->uc);   /* -> into thread; returns on yield/exit */
    h->current = -1;
}
/* Called from inside a running thread to give control back to scheduler. */
static void block_current(hle_ctx *h, th_state why) {
    hle_thread *t = &h->threads[h->current];
    regs_save(h->ee, &t->rs);
    t->state = why;
    swapcontext(&t->uc, &h->sched_uc); /* -> scheduler; resumes here later */
    /* resumed: run_thread already restored our regs + set RUNNING */
}

/* ------------------------------------------------------------------ *
 * thread table management                                            *
 * ------------------------------------------------------------------ */
static int alloc_thread(hle_ctx *h) {
    for (int i = 1; i < HLE_MAX_THREADS; i++)      /* idx 0 reserved=none */
        if (h->threads[i].state == TH_FREE) return i;
    return -1;
}
static int make_ucontext(hle_ctx *h, hle_thread *t) {
    t->stack = malloc(HLE_STACK_BYTES);
    if (!t->stack) return -1;
    if (getcontext(&t->uc) != 0) return -1;
    t->uc.uc_stack.ss_sp   = t->stack;
    t->uc.uc_stack.ss_size = HLE_STACK_BYTES;
    t->uc.uc_link          = &h->sched_uc;
    makecontext(&t->uc, thread_trampoline, 0);
    return 0;
}

int hle_thread_create(hle_ctx *h, uint32_t entry, uint32_t gp,
                      uint32_t stack_top, int priority) {
    int ti = alloc_thread(h);
    if (ti < 0) return -1;
    hle_thread *t = &h->threads[ti];
    memset(t, 0, sizeof *t);
    t->tid = ti; t->priority = priority; t->wait_sema = -1;
    t->entry = entry; t->gp = gp; t->stack_top = stack_top;
    t->state = TH_DORMANT;
    if (make_ucontext(h, t) != 0) { t->state = TH_FREE; return -1; }
    return ti;
}
int hle_thread_create_native(hle_ctx *h, void (*fn)(hle_ctx*, void*),
                             void *arg, int priority) {
    int ti = alloc_thread(h);
    if (ti < 0) return -1;
    hle_thread *t = &h->threads[ti];
    memset(t, 0, sizeof *t);
    t->tid = ti; t->priority = priority; t->wait_sema = -1;
    t->is_native = 1; t->nfn = fn; t->narg = arg;
    t->state = TH_DORMANT;
    if (make_ucontext(h, t) != 0) { t->state = TH_FREE; return -1; }
    return ti;
}
int hle_thread_start(hle_ctx *h, int tid, uint32_t arg) {
    if (tid <= 0 || tid >= HLE_MAX_THREADS) return -1;
    hle_thread *t = &h->threads[tid];
    if (t->state != TH_DORMANT) return -1;
    t->arg = arg;
    ready_push(h, tid);
    return 0;
}
int  hle_thread_id(const hle_ctx *h){ return h->current > 0 ? h->threads[h->current].tid : 0; }
void hle_thread_exit(hle_ctx *h){
    hle_thread *t = &h->threads[h->current];
    t->state = TH_EXITED;
    swapcontext(&t->uc, &h->sched_uc);   /* never returns */
}
void hle_thread_sleep(hle_ctx *h){
    hle_thread *t = &h->threads[h->current];
    if (t->wakeup_pending) { t->wakeup_pending--; return; } /* latched wakeup */
    block_current(h, TH_SLEEP);
}
void hle_thread_wakeup(hle_ctx *h, int tid){
    if (tid <= 0 || tid >= HLE_MAX_THREADS) return;
    hle_thread *t = &h->threads[tid];
    if (t->state == TH_SLEEP) ready_push(h, tid);
    else t->wakeup_pending++;             /* wake before sleep: latch */
}
void hle_thread_yield(hle_ctx *h){
    if (h->current < 0) return;
    ready_push(h, h->current);            /* tail of ready */
    block_current(h, TH_READY);           /* state overwritten by ready_push->RUNNING on resume */
}

/* Cooperative PRIORITY PREEMPTION: the EE RTOS is priority-preemptive, but our
 * scheduler is cooperative/FIFO.  Call this at scheduling points (StartThread,
 * ChangeThreadPriority, ...) so that when a READY thread outranks the running
 * one we switch to it, as the hardware would.  PS2: lower number = higher
 * priority.  Without this the boot's worker-thread handshakes deadlock
 * (FUN_00243f00 busy-waits on a flag a higher-priority worker must set). */
static int higher_prio_ready(hle_ctx *h){
    if (h->current <= 0) return 0;
    int cp = h->threads[h->current].priority;
    for (int ti = h->ready_head; ti >= 0; ti = h->threads[ti].q_next)
        if (h->threads[ti].priority < cp) return 1;
    return 0;
}
static void maybe_preempt(hle_ctx *h){ if (higher_prio_ready(h)) hle_thread_yield(h); }

/* ------------------------------------------------------------------ *
 * counting semaphores                                                *
 * ------------------------------------------------------------------ */
static void sema_enqueue(hle_ctx *h, hle_sema *s, int ti) {
    h->threads[ti].q_next = -1;
    if (s->wait_tail < 0) s->wait_head = s->wait_tail = ti;
    else { h->threads[s->wait_tail].q_next = ti; s->wait_tail = ti; }
}
static int sema_dequeue(hle_ctx *h, hle_sema *s) {
    int ti = s->wait_head;
    if (ti < 0) return -1;
    s->wait_head = h->threads[ti].q_next;
    if (s->wait_head < 0) s->wait_tail = -1;
    return ti;
}

int hle_sema_create(hle_ctx *h, int init_count, int max_count) {
    for (int i = 0; i < HLE_MAX_SEMAS; i++) {
        hle_sema *s = &h->semas[i];
        if (!s->in_use) {
            s->in_use = 1;
            s->count = init_count;
            s->max_count = (max_count > 0) ? max_count : 0x7fffffff;
            s->wait_head = s->wait_tail = -1;
            return i;
        }
    }
    return -1;
}
int hle_sema_delete(hle_ctx *h, int sid) {
    if (sid < 0 || sid >= HLE_MAX_SEMAS || !h->semas[sid].in_use) return -1;
    /* release any waiters so they don't dangle (PS2 wakes them w/ error) */
    hle_sema *s = &h->semas[sid];
    int ti; while ((ti = sema_dequeue(h, s)) >= 0) ready_push(h, ti);
    s->in_use = 0;
    return 0;
}
int hle_sema_signal(hle_ctx *h, int sid) {
    if (sid < 0 || sid >= HLE_MAX_SEMAS || !h->semas[sid].in_use) return -1;
    hle_sema *s = &h->semas[sid];
    int ti = sema_dequeue(h, s);
    if (ti >= 0) { ready_push(h, ti); return s->count; }   /* direct handoff */
    if (s->count < s->max_count) s->count++;
    return s->count;
}
int hle_sema_wait(hle_ctx *h, int sid) {
    if (sid < 0 || sid >= HLE_MAX_SEMAS || !h->semas[sid].in_use) return -1;
    hle_sema *s = &h->semas[sid];
    if (s->count > 0) { s->count--; return 0; }
    /* block current thread */
    hle_thread *t = &h->threads[h->current];
    t->wait_sema = sid;
    sema_enqueue(h, s, h->current);
    block_current(h, TH_BLOCKED_SEMA);
    t->wait_sema = -1;
    return 0;
}
int hle_sema_poll(hle_ctx *h, int sid) {
    if (sid < 0 || sid >= HLE_MAX_SEMAS || !h->semas[sid].in_use) return -1;
    hle_sema *s = &h->semas[sid];
    if (s->count > 0) { s->count--; return s->count; }
    return -1;
}
int hle_sema_count(const hle_ctx *h, int sid) {
    if (sid < 0 || sid >= HLE_MAX_SEMAS || !h->semas[sid].in_use) return -1;
    return h->semas[sid].count;
}

/* ------------------------------------------------------------------ *
 * INTC handlers + synthetic vblank                                   *
 * ------------------------------------------------------------------ */
int hle_intc_add_native(hle_ctx *h, int cause, hle_intc_fn fn, void *arg) {
    if (cause < 0 || cause >= 16) return -1;
    for (int i = 0; i < HLE_MAX_INTC; i++) {
        if (!h->intc[cause][i].used) {
            h->intc[cause][i].used = 1;
            h->intc[cause][i].is_native = 1;
            h->intc[cause][i].nfn = fn;
            h->intc[cause][i].narg = arg;
            return cause * HLE_MAX_INTC + i;
        }
    }
    return -1;
}
static int intc_add_guest(hle_ctx *h, int cause, uint32_t handler, uint32_t arg) {
    if (cause < 0 || cause >= 16) return -1;
    for (int i = 0; i < HLE_MAX_INTC; i++) {
        if (!h->intc[cause][i].used) {
            h->intc[cause][i].used = 1;
            h->intc[cause][i].is_native = 0;
            h->intc[cause][i].guest_handler = handler;
            h->intc[cause][i].guest_arg = arg;
            return cause * HLE_MAX_INTC + i;
        }
    }
    return -1;
}

/* Run all handlers registered on `cause`.  Native handlers call straight in;
 * guest handlers (iSignalSema wrappers etc.) are invoked with $a0=cause and
 * $a1=arg, around a save/restore of the shared register file. */
void hle_run_guest_intc(hle_ctx *h, int cause) {
    if (cause < 0 || cause >= 16) return;
    ee_regsave saved; int have_saved = 0;
    for (int i = 0; i < HLE_MAX_INTC; i++) {
        hle_intc_slot *sl = &h->intc[cause][i];
        if (!sl->used) continue;
        if (sl->is_native) {
            sl->nfn(h, sl->narg);
        } else {
            ee_func_t f = ee_lookup(sl->guest_handler);
            if (!f) continue;
            if (!have_saved) { regs_save(h->ee, &saved); have_saved = 1; }
            h->ee->r[R_A0].ud[0] = (int32_t)cause;
            h->ee->r[R_A1].ud[0] = (int32_t)sl->guest_arg;
            f(h->ee);
        }
    }
    if (have_saved) regs_restore(h->ee, &saved);
}

void hle_vblank_tick(hle_ctx *h) {
    h->vblank_count++;
    h->vsync_flag = 1;
    h->intc_stat |= (1u << INTC_VBLANK_START);
    /* unless masked by the GS IMR, deliver the vblank interrupt chain */
    hle_run_guest_intc(h, INTC_VBLANK_START);
    hle_run_guest_intc(h, INTC_VBLANK_END);
    /* a tick is a frame boundary: present whatever the GS has (if wired) */
    extern void gs_flip(struct gs_state *);  /* gs.h, resolved at link */
    if (h->gs) gs_flip(h->gs);
}

void   hle_set_vblank_hz(hle_ctx *h, double hz){ h->vblank_hz = hz; }
double hle_get_vblank_hz(const hle_ctx *h){ return h->vblank_hz; }
uint64_t hle_vblank_count(const hle_ctx *h){ return h->vblank_count; }

/* ------------------------------------------------------------------ *
 * scheduler main loop                                                *
 * ------------------------------------------------------------------ */
static int any_blocked(const hle_ctx *h) {
    for (int i = 1; i < HLE_MAX_THREADS; i++) {
        th_state s = h->threads[i].state;
        if (s == TH_BLOCKED_SEMA || s == TH_SLEEP) return 1;
    }
    return 0;
}
static void pace_vblank(hle_ctx *h) {
    if (h->vblank_hz <= 0.0) return;      /* fast mode (tests): no sleep   */
    long ns = (long)(1e9 / h->vblank_hz);
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (!h->paced_armed) { h->paced_next = now; h->paced_armed = 1; }
    h->paced_next.tv_nsec += ns;
    while (h->paced_next.tv_nsec >= 1000000000L) {
        h->paced_next.tv_nsec -= 1000000000L; h->paced_next.tv_sec++;
    }
    /* don't chase the past if we fell behind */
    if (h->paced_next.tv_sec < now.tv_sec ||
        (h->paced_next.tv_sec == now.tv_sec && h->paced_next.tv_nsec < now.tv_nsec))
        h->paced_next = now;
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &h->paced_next, NULL);
}

uint64_t hle_run(hle_ctx *h, uint64_t max_vblanks) {
    uint64_t start = h->vblank_count;
    h->paced_armed = 0;                   /* fresh pacing baseline per run */
    getcontext(&h->sched_uc);             /* establish swap target         */
    for (;;) {
        int ti = ready_pop(h);
        if (ti >= 0) {
            run_thread(h, ti);
            hle_thread *t = &h->threads[ti];
            if (t->state == TH_RUNNING || t->state == TH_EXITED) {
                /* returned off the end OR explicit exit: reap */
                t->state = TH_EXITED;
                free(t->stack); t->stack = NULL;
                t->state = TH_FREE;
            } else if (t->state == TH_READY) {
                /* a yield re-queued it already via ready_push */
            }
            continue;
        }
        if (h->exit_requested) break;
        if (!any_blocked(h)) break;        /* nothing runnable, no waiter   */
        if (max_vblanks && (h->vblank_count - start) >= max_vblanks) break;

        /* Nothing is ready but threads are blocked.  The only event source
         * left in a cooperative machine is the vblank, so drive one (paced
         * to real time when a video mode set an hz).  */
        pace_vblank(h);
        hle_vblank_tick(h);

        /* If the tick released nobody, every remaining thread is blocked on
         * something a vblank can't satisfy -> genuine quiescence/deadlock.
         * Stop rather than spin (bounded runs already returned above).      */
        if (h->ready_head < 0) break;
    }
    return h->vblank_count - start;
}

int      hle_exit_requested(const hle_ctx *h){ return h->exit_requested; }
uint32_t hle_loadexec_target(const hle_ctx *h){ return h->loadexec_target; }

/* ------------------------------------------------------------------ *
 * lifecycle + wiring                                                 *
 * ------------------------------------------------------------------ */
hle_ctx *hle_create(ee_state *ee) {
    hle_ctx *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->ee = ee;
    ee->host = h;
    h->current = -1;
    h->ready_head = h->ready_tail = -1;
    h->video_mode = 2;             /* default NTSC until SetGsCrt         */
    h->vblank_hz = 0.0;            /* fast mode until a test opts into RT  */
    for (int i = 0; i < HLE_MAX_THREADS; i++) h->threads[i].state = TH_FREE;
    return h;
}
void hle_destroy(hle_ctx *h) {
    if (!h) return;
    for (int i = 0; i < HLE_MAX_THREADS; i++) free(h->threads[i].stack);
    if (h->ee && h->ee->host == h) h->ee->host = NULL;
    free(h);
}
void hle_set_gs (hle_ctx *h, struct gs_state *gs){ h->gs = gs; }
void hle_set_sif(hle_ctx *h, struct sif_dev *sif){ h->sif = sif; }
void hle_set_pad(hle_ctx *h, struct pad_state *pad){ h->pad = pad; }
void hle_set_vif1_sink(hle_ctx *h, hle_vif1_fn cb, void *user){ h->vif1_cb=cb; h->vif1_user=user; }

/* ================================================================== *
 * ee_syscall() -- the EE BIOS syscall trap target (ee_runtime.h §6). *
 * The number is taken from $v1 (how libkernel invokes it); the i-     *
 * prefixed interrupt variants arrive as sign-extended negatives and   *
 * route to the same handler (cooperative HLE has no interrupt context  *
 * distinction).  Evidence: analysis/decomp 0x00451ae0..0x00452310.    *
 * ================================================================== */
static int32_t syscall_num(ee_state *ee, uint32_t code) {
    int32_t n = (int32_t)code;
    if (n == 0) n = EE_GETS32(ee, R_V1);  /* li $v1,num ; syscall 0 */
    return n;
}

void ee_syscall(ee_state *ee, uint32_t code) {
    hle_ctx *h = (hle_ctx*)ee->host;
    int32_t n = syscall_num(ee, code);
    uint32_t a0 = EE_GET32(ee, R_A0), a1 = EE_GET32(ee, R_A1),
             a2 = EE_GET32(ee, R_A2), a3 = EE_GET32(ee, R_A3);
    int32_t ret = 0;

    switch (n) {
    /* ---- cache / sync (no-ops in a flat memory model) -------------- */
    case 0x64: /* FlushCache */            ret = 0; break;
    case 0x3c: /* SetupThread(gp,stack,stack_size,args) -> returns the initial
                * stack pointer (= stack + stack_size).  Returning 0 (the old
                * value) left the whole program running with $sp=0; crt0
                * FUN_00100000 does `$sp = SetupThread(...)`, so a real stack
                * top is required.  (Integration bug fix, WS-K.) */
        ret = (int32_t)(a1 + a2); break;
    case 0x3d: /* SetupHeap   */           ret = 0; break;
    case 0x3e: /* EndOfHeap   */           ret = 0; break;

    /* ---- interrupt enable/disable (track a flag; cooperative=no-op) - */
    case 0x14: case 0xffffffe6: /* _EnableIntc / i */   ret = 1; break;
    case 0x15: case 0xffffffe5: /* _DisableIntc / i */  ret = 1; break;
    case 0x16: case 0xffffffe4: /* _EnableDmac / i */   ret = 1; break;
    case 0x17: case 0xffffffe3: /* _DisableDmac / i */  ret = 1; break;

    /* ---- INTC/DMAC handler registration ---------------------------- */
    case 0x10: /* AddIntcHandler(cause,handler,next,arg) */
        ret = intc_add_guest(h, (int)a0, a1, a3); break;
    case 0x11: /* RemoveIntcHandler(cause,id) */ ret = 0; break;
    case 0x12: /* AddDmacHandler(chan,handler,next,arg) -- tracked, M1 unused */
        ret = 0; break;
    case 0x13: /* RemoveDmacHandler */ ret = 0; break;

    /* ---- threads --------------------------------------------------- */
    case 0x20: { /* CreateThread(ThreadParam*) */
        uint32_t entry = ee_lw(ee, a0 + 0x04);  /* func   */
        uint32_t stack = ee_lw(ee, a0 + 0x08);  /* stack  */
        uint32_t ssz   = ee_lw(ee, a0 + 0x0c);  /* size   */
        uint32_t gp    = ee_lw(ee, a0 + 0x10);  /* gp     */
        int32_t  prio  = (int32_t)ee_lw(ee, a0 + 0x14);
        uint32_t stop  = stack + ssz;           /* stack grows down       */
        ret = hle_thread_create(h, entry, gp, stop, prio);
        break; }
    case 0x21: /* DeleteThread(id) */ {
        if ((int)a0 > 0 && (int)a0 < HLE_MAX_THREADS) {
            h->threads[a0].state = TH_FREE;
            free(h->threads[a0].stack); h->threads[a0].stack = NULL;
        }
        ret = 0; break; }
    case 0x22: /* StartThread(id,arg) */
        ret = hle_thread_start(h, (int)a0, a1);
        /* Cooperative PRIORITY PREEMPTION: the EE RTOS switches immediately to
         * a newly-ready thread of higher priority.  Our scheduler is otherwise
         * FIFO (prio not honored), which deadlocks the boot's worker handshake
         * (FUN_00243f00 busy-waits on DAT_0046b34c that the just-started, higher
         * priority worker FUN_00243b20 sets).  So if we made a higher-priority
         * thread ready, yield to it now.  PS2: lower number = higher priority. */
        if (ret == 0) maybe_preempt(h);
        break;
    case 0x24: /* ExitDeleteThread */   hle_thread_exit(h); return;
    case 0x25: /* TerminateThread(id) */ {
        if ((int)a0 > 0 && (int)a0 < HLE_MAX_THREADS)
            h->threads[a0].state = TH_DORMANT;
        ret = 0; break; }
    case 0x29: /* ChangeThreadPriority(id,prio) */
        if ((int)a0 > 0 && (int)a0 < HLE_MAX_THREADS) h->threads[a0].priority = (int)a1;
        ret = 0; maybe_preempt(h); break;   /* lowering own prio may expose a higher-prio waiter */
    case 0x2b: case 0xffffffd0: /* RotateThreadReadyQueue(prio) */
        hle_thread_yield(h); ret = 0; break;
    case 0x2f: /* GetThreadId */ ret = hle_thread_id(h); break;
    case 0x30: /* ReferThreadStatus(id,status*) */ {
        /* minimal: report RUNNING/READY state code at +0x00 */
        if (a1) ee_sw(ee, a1 + 0x00,
                      (a0 == (uint32_t)hle_thread_id(h)) ? 0x01 /*RUN*/ : 0x02 /*READY*/);
        ret = 0; break; }
    case 0x32: /* SleepThread */ hle_thread_sleep(h); ret = 0; break;
    case 0x33: case 0xffffffcc: /* WakeupThread / iWakeupThread */
        hle_thread_wakeup(h, (int)a0); ret = 0; break;
    case 0xffffffc8: /* iSuspendThread */ ret = 0; break;

    /* ---- semaphores ------------------------------------------------ */
    case 0x40: { /* CreateSema(SemaParam*) -- init@+4, max@+8 (evidence
                    FUN_00241c58: writes count at +4, max field at +8) */
        int init = (int)ee_lw(ee, a0 + 0x04);
        int max  = (int)ee_lw(ee, a0 + 0x08);
        ret = hle_sema_create(h, init, max);
        break; }
    case 0x41: /* DeleteSema(id) */ ret = hle_sema_delete(h, (int)a0); break;
    case 0x42: case 0xffffffbd: /* SignalSema / iSignalSema */
        ret = hle_sema_signal(h, (int)a0); break;
    case 0x44: /* WaitSema(id) */ ret = hle_sema_wait(h, (int)a0); break;
    case 0x45: /* PollSema(id) */ ret = hle_sema_poll(h, (int)a0); break;
    case 0x47: /* ReferSemaStatus(id,status*) -- count at +0x0c
                  (evidence FUN_00241a80 reads +0x0c) */ {
        int c = hle_sema_count(h, (int)a0);
        if (a1 && c >= 0) {
            ee_sw(ee, a1 + 0x00, (uint32_t)c);     /* count (dup, friendly) */
            ee_sw(ee, a1 + 0x0c, (uint32_t)c);     /* count @ +0x0c         */
        }
        ret = (c >= 0) ? (int)a0 : -1; break; }

    /* ---- graphics / video ------------------------------------------ */
    case 2: /* SetGsCrt(interlace,mode,field) */
        h->video_mode = (int)a1;
        /* mode 3 = PAL (50 Hz), everything else NTSC (~59.94 Hz).  Sets the
         * nominal vblank rate; a harness that wants fast (unpaced) vblanks
         * for logic tests simply never calls SetGsCrt (hz stays 0). */
        h->vblank_hz = (a1 == 3) ? 50.0 : (60000.0 / 1001.0);
        ret = 0; break;
    case 0x70: case 0xffffff90: /* GsGetIMR / iGsGetIMR */ ret = (int)h->gs_imr; break;
    case 0x71: case 0xffffff8f: /* GsPutIMR / iGsPutIMR */ {
        int32_t old = (int)h->gs_imr; h->gs_imr = a0; ret = old; break; }
    case 0x73: /* SetVSyncFlag(ptr,ptr) */ h->vsync_flag = 0; ret = 0; break;

    /* ---- program control ------------------------------------------- */
    case 4: /* Exit */                 h->exit_requested = 1; return;
    case 6: /* _LoadExecPS2(file,argc,argv) */
        h->exit_requested = 1; h->loadexec_target = a0; return;
    case 7: /* ExecPS2 */              h->exit_requested = 1; h->loadexec_target = a0; return;

    /* ---- misc BIOS the boot path pokes at -------------------------- */
    case 0x4b: /* GetOsdConfigParam */  if (a0) ee_sw(ee, a0, 0); ret = 0; break;
    case 0x6f: /* GetOsdConfigParam2 */ if (a0) ee_sw(ee, a0, 0); ret = 0; break;
    case 0x74: /* SetSyscall(RFU116) */ ret = 0; break;
    case 0x5a: case 0x5b: ret = 0; break; /* RFU090/091 */
    case 0x7c: /* Deci2Call (debug EE) */ ret = 0; break;

    /* ---- SIF DMA / register syscalls (back the IOP-ready handshake) --- */
    case 0x6b: /* sceSifStopDma */ ret = 0; break;
    case 0x76: /* sceSifDmaStat(id) -> 0 = complete */ ret = 0; break;
    case 0x77: case 0xffffff89: /* sceSifSetDma / isceSifSetDma */
        ret = 1; break;            /* nonzero = queued ok (reboot checks it) */
    case 0x78: case 0xffffff88: /* sceSifSetDChain / isceSifSetDChain */
        ret = 1; break;
    case 0x79: /* sceSifSetReg(idx,val) */
        if (sif_set_reg && h->sif) sif_set_reg((struct sif_dev*)h->sif, a0, a1);
        ret = 0; break;
    case 0x7a: /* sceSifGetReg(idx) */
        ret = (sif_get_reg && h->sif) ? (int)sif_get_reg((struct sif_dev*)h->sif, a0) : 0;
        break;

    default:
        fprintf(stderr, "[hle] UNHANDLED syscall %d (0x%x) a0=%08x a1=%08x\n",
                n, (unsigned)n, a0, a1);
        ret = 0; break;
    }
    (void)a2; (void)a3;
    EE_SET32(ee, R_V0, (uint32_t)ret);
}

void ee_break(ee_state *ee, uint32_t code) {
    (void)ee; fprintf(stderr, "[hle] BREAK code=%u\n", code);
}
void ee_trap_unimpl(ee_state *ee, uint32_t pc, uint32_t raw, const char *mnem) {
    (void)ee; fprintf(stderr, "[hle] FATAL unimplemented op @%08x raw=%08x (%s)\n",
                      pc, raw, mnem ? mnem : "?");
    abort();
}
