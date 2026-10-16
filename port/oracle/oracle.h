/* =====================================================================
 * oracle.h  --  REUSABLE EXECUTABLE ORACLE for the Dirge of Cerberus port.
 * ---------------------------------------------------------------------
 * A thin library around the recompiled EE runtime (recomp/ee_runtime.h +
 * recomp/generated_all + hle/ + sif/ + cop2/ + gs/ + vu/ + input/) that lets
 * a host program use the real, statically-recompiled game code as an ORACLE:
 *
 *     1. create a guest machine (32 MB RAM + 16 KB scratchpad) and load the
 *        EE ELF's PT_LOAD segments (so every game global / .data pointer is
 *        in place, exactly as boot/main.c does);
 *     2. INJECT known bytes anywhere in guest RAM (e.g. a model buffer);
 *     3. set up the EE argument registers ($a0..$a3, stack args) and CALL any
 *        recompiled function  func_<addr>(ee_state*)  synchronously;
 *     4. READ / DUMP a region of guest memory back out (hex or raw).
 *
 * This is the same machine the M1 boot harness builds, minus the boot loop:
 * the HLE kernel/SIF/GS/VU/pad are all wired, so a called function that does
 * MMIO, SIF RPC or VU0-macro math behaves as it does in the booting build.
 * (Functions that BLOCK on a semaphore need the cooperative scheduler and a
 * guest thread; the direct oracle_call() path is for non-blocking, compute /
 * parse style functions.  See README.md.)
 *
 * It is DELIBERATELY general: `oracle-cli` exposes call+inject+dump from the
 * command line so this same tool validates the native/engine decomposition
 * against the oracle later, not just the bone-matrix dump it was built for.
 * ===================================================================== */
#ifndef DIRGE_ORACLE_H
#define DIRGE_ORACLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "ee_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oracle oracle;

/* ---- lifecycle ------------------------------------------------------ */
/* Create the guest machine and load the EE ELF.  `elf_path` NULL -> the
 * built-in default (the game's SLES_541.85).  `want_io` != 0 also opens
 * FILELIST.BIN / KEL.DAT and primes the synthetic-IOP SIF handshake (needed
 * only if a called function performs file I/O); 0 keeps it minimal/fast.
 * Returns NULL on failure (message on stderr). */
oracle *oracle_create(const char *elf_path, int want_io);
void    oracle_destroy(oracle *o);

/* The underlying ee_state, for callers that want full register access. */
ee_state *oracle_ee(oracle *o);

/* ---- guest memory --------------------------------------------------- */
void oracle_write_mem(oracle *o, uint32_t addr, const void *src, size_t n);
void oracle_read_mem (oracle *o, uint32_t addr, void *dst, size_t n);
uint32_t oracle_lw(oracle *o, uint32_t addr);
void     oracle_sw(oracle *o, uint32_t addr, uint32_t v);

/* Bump-allocate `n` bytes of free high guest RAM (16-byte aligned), well
 * above the ELF image and below the stack.  Returns the guest address, or 0
 * if exhausted.  A simple arena; oracle_reset_arena() rewinds it. */
uint32_t oracle_alloc(oracle *o, size_t n);
void     oracle_reset_arena(oracle *o);

/* Load a host file into a freshly oracle_alloc()'d guest region.  Returns the
 * guest base address (and *size_out the byte count), or 0 on failure. */
uint32_t oracle_load_file(oracle *o, const char *path, size_t *size_out);

/* ---- register / call setup ------------------------------------------ */
/* Set an integer argument register.  i: 0..3 -> $a0..$a3; 4..7 -> $t0..$t3;
 * i>=8 is written to the guest stack at $sp + (i-8)*4 (EABI stack args).
 * Values are written as 32-bit, sign-extended into the 64-bit low half. */
void oracle_set_arg(oracle *o, int i, uint32_t val);
void oracle_set_reg(oracle *o, int reg_index, uint64_t val);   /* R_* index  */
uint64_t oracle_get_reg(oracle *o, int reg_index);
uint32_t oracle_ret(oracle *o);        /* $v0 after a call                   */

/* Set the guest stack pointer used by calls (default: high-RAM stack top). */
void oracle_set_sp(oracle *o, uint32_t sp);

/* Call the recompiled function at guest address `addr` (resolved through the
 * generated function table).  Registers/stack must already be set up.  On
 * return $v0 holds the result.  Returns 0 on success, -1 if `addr` is not a
 * known recompiled function.  A longjmp fault guard catches a hard
 * dereference fault inside the call and returns -2 (see oracle_last_fault).*/
int oracle_call(oracle *o, uint32_t addr);

/* After a -2 return, a short description of the fault (signal, approx addr). */
const char *oracle_last_fault(oracle *o);

/* ---- dumping -------------------------------------------------------- */
/* Hex-dump `n` bytes at guest `addr` to `f` (xxd-ish, with guest offsets). */
void oracle_dump_hex(oracle *o, uint32_t addr, size_t n, FILE *f);

/* Interpret `count` consecutive 0x40-byte blocks at `addr` as 4x4 float
 * matrices and write them to `f` as JSON { "<i>": [[...4],[...4],[...4],
 * [...4]], ... }.  `row_major` selects the row/column interpretation. */
void oracle_dump_mat4_json(oracle *o, uint32_t addr, int count,
                           int row_major, FILE *f);

#ifdef __cplusplus
}
#endif
#endif /* DIRGE_ORACLE_H */
