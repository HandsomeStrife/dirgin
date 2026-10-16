/* =====================================================================
 * hle.h  --  HLE kernel + I/O layer public interface (WS-K).
 *
 * This is the layer that implements the trap targets the recompiler
 * contract (ee_runtime.h) declares but does NOT define:
 *     ee_syscall(), ee_mmio_store{32,64,128}(), ee_mmio_load{32,64}()
 * plus a cooperative thread + counting-semaphore kernel, a synthetic
 * vblank source that paces the frame loop's WaitSema, an INTC/DMAC model,
 * and the bridge to gs/ (GIF), vu/ (VIF1) and sif/ (file RPC) + input/.
 *
 * ee->host points at an `hle_ctx`.  hle_create() allocates it and wires it
 * to an ee_state; the syscall/MMIO hooks recover it via ee->host.
 * ===================================================================== */
#ifndef DIRGE_HLE_H
#define DIRGE_HLE_H

#include <stdint.h>
#include <stddef.h>
#include "ee_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hle_ctx hle_ctx;

/* Forward decls of the things we bridge to (opaque to callers of hle.h). */
struct gs_state;
struct sif_dev;
struct pad_state;

/* ---- lifecycle ------------------------------------------------------ */
hle_ctx *hle_create(ee_state *ee);
void     hle_destroy(hle_ctx *h);

/* Wire optional backends (any may be NULL; calls become no-ops/logs). */
void hle_set_gs  (hle_ctx *h, struct gs_state *gs);
void hle_set_sif (hle_ctx *h, struct sif_dev *sif);
void hle_set_pad (hle_ctx *h, struct pad_state *pad);

/* VIF1 display-list sink.  The DMAC VIF1 chain, once assembled from the
 * ring, is handed to this callback (WS-V plugs VU1 in here).  Each call is
 * one DMAtag's referenced qwords (VIFcodes + unpack data). */
typedef void (*hle_vif1_fn)(void *user, const uint8_t *qwords, size_t nqw);
void hle_set_vif1_sink(hle_ctx *h, hle_vif1_fn cb, void *user);

/* ---- video mode / pacing -------------------------------------------- */
/* Set by SetGsCrt; 0 = run vblanks as fast as the scheduler needs them
 * (tests), >0 = pace to real time at this Hz (PAL=50, NTSC~59.94). */
void   hle_set_vblank_hz(hle_ctx *h, double hz);
double hle_get_vblank_hz(const hle_ctx *h);
uint64_t hle_vblank_count(const hle_ctx *h);

/* ---- cooperative kernel: threads ------------------------------------ */
/* Create a guest thread from a guest entry address (resolved via
 * ee_lookup).  Returns an HLE thread id (>0), or <0 on error. */
int  hle_thread_create(hle_ctx *h, uint32_t entry, uint32_t gp,
                       uint32_t stack_top, int priority);
/* Create a thread that runs a NATIVE C function -- used by the standalone
 * harness (no recompiled corpus) to exercise the scheduler/sema. */
int  hle_thread_create_native(hle_ctx *h, void (*fn)(hle_ctx*, void*),
                              void *arg, int priority);
int  hle_thread_start(hle_ctx *h, int tid, uint32_t arg);  /* make ready   */
int  hle_thread_id(const hle_ctx *h);                      /* current tid  */
void hle_thread_exit(hle_ctx *h);                          /* end current  */
void hle_thread_sleep(hle_ctx *h);                         /* SleepThread  */
void hle_thread_wakeup(hle_ctx *h, int tid);               /* WakeupThread */
void hle_thread_yield(hle_ctx *h);                         /* RotateReady  */

/* Run the scheduler until every thread has exited, Exit() was called, or
 * the machine is otherwise quiescent (nothing runnable and no vblank
 * waiter).  `max_vblanks` bounds the synthetic vblank source so a test
 * cannot spin forever (0 = unbounded). Returns number of vblanks driven. */
uint64_t hle_run(hle_ctx *h, uint64_t max_vblanks);

/* Deliver exactly one synthetic vblank right now (advances counter, runs
 * the registered INTC vblank handlers, i.e. iSignalSema on the vblank
 * semaphore).  Exposed for fine-grained tests. */
void hle_vblank_tick(hle_ctx *h);

/* ---- cooperative kernel: counting semaphores ------------------------ */
int  hle_sema_create(hle_ctx *h, int init_count, int max_count);
int  hle_sema_delete(hle_ctx *h, int sid);
int  hle_sema_signal(hle_ctx *h, int sid);   /* Signal / iSignal          */
int  hle_sema_wait  (hle_ctx *h, int sid);   /* blocks current thread     */
int  hle_sema_poll  (hle_ctx *h, int sid);   /* non-blocking; <0 if would block */
int  hle_sema_count (const hle_ctx *h, int sid);

/* ---- INTC: register a native vblank/interrupt handler (harness) ----- */
typedef void (*hle_intc_fn)(hle_ctx *h, void *arg);
int  hle_intc_add_native(hle_ctx *h, int cause, hle_intc_fn fn, void *arg);

/* ---- shutdown status ------------------------------------------------ */
int  hle_exit_requested(const hle_ctx *h);   /* Exit()/_LoadExecPS2 seen  */
uint32_t hle_loadexec_target(const hle_ctx *h);

#ifdef __cplusplus
}
#endif
#endif /* DIRGE_HLE_H */
