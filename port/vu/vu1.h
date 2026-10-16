/* ============================================================================
 * vu1.h  --  VU1 microcode engine interface for the Dirge of Cerberus PC port
 *            (workstream WS-V).  SKETCH / PROVISIONAL.
 *
 * The PS2 renders geometry on Vector Unit 1 (VU1): a SIMD coprocessor with
 * 16 KB of micro-instruction memory, 16 KB of VU data memory (VuMem1), 32x
 * 128-bit float registers (vf), 16x 16-bit integer registers (vi), and an EFU
 * (elementary-function unit: DIV/SQRT/RSQRT, and the P-register ops ESADD/
 * ELENG/ESIN/...).  It is fed by VIF1, which unpacks DMA data into VuMem and
 * uploads microprograms (MPG), and emits GIF packets to the GS via XGKICK.
 *
 * This header defines the seam that WS-G (GS backend) and WS-R (EE recompiler)
 * use to drive VU1.  The engine behind it may be an INTERPRETER or a
 * RECOMPILER (see notes/ for the recommendation); callers must not care which.
 *
 * Coordinate / fixed-point note: transformed screen XY leave VU1 as GS 12.4
 * fixed point (produced by FTOI4 in every microprogram), i.e. subpixel/16.
 * ==========================================================================*/
#ifndef DIRGE_VU1_H
#define DIRGE_VU1_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Fixed sizes of the real hardware ------------------------------------ */
#define VU1_MICROMEM_BYTES  (16 * 1024)   /* 2048 x 64-bit instructions      */
#define VU1_DATAMEM_BYTES   (16 * 1024)   /* 1024 x 128-bit qwords (VuMem1)  */
#define VU1_VF_COUNT        32            /* vf00..vf31 (vf00 = {0,0,0,1})   */
#define VU1_VI_COUNT        16            /* vi00..vi15 (vi00 = 0)           */

/* A VU quadword: 4x 32-bit lanes (x,y,z,w). Interpreted as float OR int
 * depending on the op that produced/consumes it. */
typedef union { float f[4]; uint32_t u[4]; int32_t s[4]; } vu_qword;

/* ------------------------------------------------------------------------- *
 * VU1 context.  One instance = the VU1 "device".  The backend owns the
 * concrete layout; this is the public view WS-G/WS-R see.
 * ------------------------------------------------------------------------- */
typedef struct vu1_ctx vu1_ctx;

vu1_ctx *vu1_create(void);
void     vu1_destroy(vu1_ctx *);
void     vu1_reset(vu1_ctx *);

/* ------------------------------------------------------------------------- *
 * (1) UPLOAD MICROPROGRAM  (hardware: VIF1 MPG code).
 *     dst_instr = micro-instruction index to load at (VIF MPG 'loadaddr'),
 *     e.g. the overlays in this title load at instr 0, 0x100, 0x200 ...
 *     (byte addrs 0x0,0x800,0x1000 -> /8).  'src' points at raw 64-bit VU
 *     instruction pairs (LOWER word first, then UPPER, little-endian), exactly
 *     as carved from .vutext / runtime MPG packets.  Overlays are swapped in
 *     and out, so this may be called many times per frame.
 *
 *     A recompiler backend keys its translated-program cache on a hash of the
 *     uploaded bytes (programs can also arrive at runtime from asset data that
 *     is NOT in the ELF -- see notes/); an interpreter just memcpy's.
 * ------------------------------------------------------------------------- */
void vu1_upload_micro(vu1_ctx *, uint32_t dst_instr,
                      const void *src, size_t n_instr);

/* ------------------------------------------------------------------------- *
 * (2) UPLOAD DATA  (hardware: VIF1 UNPACK into VuMem1).
 *     The real VIF applies an unpack format (V4-32, V4-16, V3-32, ...),
 *     masking, cycle (CL/WL), and the double-buffer TOP offset.  WS-G's
 *     VIF1 front-end decodes VIFcodes and resolves them to already-expanded
 *     qwords here; 'dst_qword' is the VuMem qword index.
 *
 *     Double buffering: input data lands in one half of VuMem and the micro-
 *     program reads it via XTOP (which returns the current TOP base).  Call
 *     vu1_set_top() before vu1_run() to present the buffer the VIF filled.
 * ------------------------------------------------------------------------- */
void vu1_upload_data(vu1_ctx *, uint32_t dst_qword,
                     const vu_qword *src, size_t n_qwords);
void vu1_set_top(vu1_ctx *, uint32_t top_qword);    /* value XTOP returns     */
void vu1_set_itop(vu1_ctx *, uint32_t itop);        /* value XITOP returns    */

/* ------------------------------------------------------------------------- *
 * (3) RUN  (hardware: VIF1 MSCAL / MSCALF, or EE _vcallms -> but that's VU0).
 *     Starts the microprogram at 'entry_instr' and runs until it executes an
 *     instruction with the [E] end bit (every program here ends on NOP[E]).
 *     entry_instr is usually 0 or an offset selected from the program's
 *     branch-dispatch table at its head.
 * ------------------------------------------------------------------------- */
void vu1_run(vu1_ctx *, uint32_t entry_instr);

/* ------------------------------------------------------------------------- *
 * (4) COLLECT GIF OUTPUT  (hardware: XGKICK vi<n> streams a GIF packet whose
 *     qword address is in that vi register straight to the GIF).
 *
 *     The backend captures every XGKICK during vu1_run() and reports the
 *     (address,length) of each emitted GIF packet in VuMem.  WS-G consumes
 *     these as its real draw stream (reinterpreting GIFtags + GS A+D regs on
 *     the host GPU).  This is the primary output of VU1 for rendering.
 *
 *     Callback is invoked in XGKICK order.  'gif_qwords' points into VuMem1;
 *     valid only until the next vu1_run()/upload on this ctx.
 * ------------------------------------------------------------------------- */
typedef void (*vu1_xgkick_fn)(void *user,
                              const vu_qword *gif_qwords, size_t n_qwords);
void vu1_set_xgkick_cb(vu1_ctx *, vu1_xgkick_fn, void *user);

/* Direct VuMem access (debug / readback / host inspection). */
vu_qword *vu1_datamem(vu1_ctx *);                   /* VU1_DATAMEM_BYTES      */

#ifdef __cplusplus
}
#endif
#endif /* DIRGE_VU1_H */
