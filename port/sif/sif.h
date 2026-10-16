/* =====================================================================
 * sif.h  --  SIF / IOP HLE for the Dirge of Cerberus port (WS-K).
 *
 * The retail game does ALL file IO as SIF RPC to the IOP (there is no EE-
 * side sceCd; see engine-map §2).  We HLE the IOP side: the RPC servers the
 * engine binds and calls are serviced locally against the native KEL.DAT
 * loader (src/kel.c) and the host filesystem, and the IOPRP reboot becomes a
 * noop that reports "ready".
 *
 * HLE boundary: the SIF *library* functions (sceSifCallRpc FUN_00453d20,
 * bind FUN_004535d8, sceSifRebootIop FUN_004517f0, SyncIop FUN_00451958,
 * sceSifSetReg/GetReg, SifWriteBackDCache FUN_00453360) are hardware-adjacent
 * glue.  The recomp owner should route those guest functions to the entry
 * points below rather than recompiling them.
 *
 * Evidence for the RPC arg layouts:
 *   CD read  : FUN_00446af0 builds {LBA@+0, count@+4, dst@+8, mode@+0xc..}
 *              and calls sceSifCallRpc(server=0x494c90, func#1, send@0x493bc0,
 *              size 0x18).
 *   fileio   : FUN_00457c90 calls sceSifCallRpc(server=0x509b80, func#5,
 *              send@0x509c00, size 0x30).
 * ===================================================================== */
#ifndef DIRGE_SIF_H
#define DIRGE_SIF_H

#include <stdint.h>
#include <stddef.h>
#include "ee_runtime.h"
#include "kel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RPC server identifiers (from the decompiled call sites). */
#define SIF_RPC_CD      0x494c90u   /* func#1 = sector read from KEL.DAT   */
#define SIF_RPC_FILEIO  0x509b80u   /* func#5 = path-based file IO         */

typedef struct sif_dev sif_dev;

/* Create the HLE IOP.  `ee` is where guest-memory copies land.  The KEL
 * archive and its disc origin are supplied via sif_set_kel(). */
sif_dev *sif_create(ee_state *ee);
void     sif_destroy(sif_dev *s);

/* Bind the native loader.  `kel_origin` is the disc LBA (sector) at which
 * KEL.DAT begins, i.e. the engine's DAT_004df920 base: the CD-read RPC
 * supplies ABSOLUTE disc LBAs, and (lba - kel_origin) is the sector within
 * KEL.DAT.  Pass 0 if LBAs are already KEL.DAT-relative. */
void sif_set_kel(sif_dev *s, kel_archive *kel, uint32_t kel_origin);

/* IOPRP reboot / sync -- noop that flips the ready flag (FUN_004517f0/958).
 * NOTE: renamed from sif_reboot_iop to avoid colliding with the EE-hook of
 * the same name (different signature) declared in ee_runtime.h; the boot
 * glue (boot/sif_glue.c) bridges the EE hook to this device function. */
void sif_dev_reboot_iop(sif_dev *s);
int  sif_iop_ready (const sif_dev *s);

/* ---- core CD read: raw sectors -> guest memory ---------------------- *
 * Copies `count` * 0x800 bytes from KEL.DAT starting at disc LBA `lba`
 * into guest RAM at `dst_ee_addr`.  Returns bytes copied, or -1.  This is
 * the primitive the engine's sector math (FUN_0024de38) ultimately needs:
 * it delivers the RAW archive bytes (the EE then zlib-inflates if the entry
 * was compressed -- kel.c does that inflate for us when we go by entry). */
long sif_cd_read(sif_dev *s, uint32_t lba, uint32_t count, uint32_t dst_ee_addr);

/* ---- RPC entry (HLE of sceSifCallRpc) ------------------------------- *
 * Dispatches the known servers.  `send_addr`/`recv_addr` are GUEST
 * addresses of the argument/result blocks.  Returns 0 on success (<0 err).
 * On the CD server it reads {LBA,count,dst} from the send block and calls
 * sif_cd_read; on the fileio server it services the path op (best-effort,
 * see sif.c).  If a completion semaphore id is given (>=0) it is signalled
 * via the HLE kernel to mirror the async RPC callback. */
/* NOTE: renamed from sif_call_rpc to avoid colliding with the EE-hook of the
 * same name (void sif_call_rpc(ee_state*)) declared in ee_runtime.h. */
int sif_dev_call_rpc(sif_dev *s, uint32_t server, int func,
                 uint32_t send_addr, uint32_t send_size,
                 uint32_t recv_addr, uint32_t recv_size,
                 int completion_sema);

/* ---- fileio helper (cdrom0:/pfs/host paths -> kel or host fs) -------- *
 * Resolve `path` (leading "cdrom0:/" / "host:" / "pfs0:" stripped) against
 * the KEL archive virtual paths first, then the host filesystem, read its
 * bytes into guest RAM at dst.  Returns bytes read, or -1. */
long sif_fileio_read_path(sif_dev *s, const char *path, uint32_t dst_ee_addr,
                          uint32_t max_bytes);

/* ---- SBUS register handshake (routed from hle/mmio.c) --------------- */
uint64_t sif_sbus_load (sif_dev *s, uint32_t addr);
void     sif_sbus_store(sif_dev *s, uint32_t addr, uint64_t v);

/* ---- SIF register file (backs sceSifSetReg/GetReg syscalls 0x79/0x7a) *
 * Index may carry bit31 (read/modify flag on HW); we mask it.  Register 4
 * is SMFLAG: once the IOP is "rebooted" its ready bit (0x40000) reads back,
 * which is what SyncIop (FUN_00451958) polls. */
void     sif_set_reg(sif_dev *s, uint32_t idx, uint32_t val);
uint32_t sif_get_reg(sif_dev *s, uint32_t idx);

#ifdef __cplusplus
}
#endif
#endif /* DIRGE_SIF_H */
