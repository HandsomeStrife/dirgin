/* =====================================================================
 * sif.c  --  HLE IOP: file-read RPC servers backed by the native KEL.DAT
 * loader + host filesystem; IOPRP reboot = noop that reports ready.
 * ===================================================================== */
#define _GNU_SOURCE
#include "sif.h"
#include "hle.h"
#include "ee_mmio_regs.h"   /* SIF_SMFLAG (WS-K register map) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 0x800u

struct sif_dev {
    ee_state    *ee;
    kel_archive *kel;
    uint32_t     kel_origin;     /* disc LBA where KEL.DAT begins          */
    int          iop_ready;
    uint32_t     reg[32];        /* SIF register file (SMFLAG etc.)        */
};

sif_dev *sif_create(ee_state *ee) {
    sif_dev *s = calloc(1, sizeof *s);
    if (s) s->ee = ee;
    return s;
}
void sif_destroy(sif_dev *s) { free(s); }

void sif_set_kel(sif_dev *s, kel_archive *kel, uint32_t kel_origin) {
    s->kel = kel; s->kel_origin = kel_origin;
}

void sif_dev_reboot_iop(sif_dev *s) {
    /* "rom0:UDNL IOPRP.IMG" (FUN_00451990) -> on PC this is a noop; we just
     * mark the IOP ready so SyncIop (bit 0x40000 of SMFLAG) succeeds. */
    s->iop_ready = 1;
    s->reg[4] |= 0x40000;
}
int sif_iop_ready(const sif_dev *s) { return s->iop_ready; }

void sif_set_reg(sif_dev *s, uint32_t idx, uint32_t val) {
    s->reg[(idx & 0x7fffffffu) & 31u] = val;
}
uint32_t sif_get_reg(sif_dev *s, uint32_t idx) {
    uint32_t i = (idx & 0x7fffffffu) & 31u;
    uint32_t v = s->reg[i];
    if (i == 4 && s->iop_ready) v |= 0x40000;   /* SMFLAG ready bit        */
    return v;
}

/* ------------------------------------------------------------------ *
 * core: raw sector read from KEL.DAT -> guest RAM                    *
 * ------------------------------------------------------------------ */
long sif_cd_read(sif_dev *s, uint32_t lba, uint32_t count, uint32_t dst_ee_addr) {
    if (getenv("DIRGE_TRACE"))
        fprintf(stderr,"[sif] CD-READ lba=%u count=%u dst=%08x (kel_origin=%u)\n",
                lba, count, dst_ee_addr, s->kel_origin);
    if (!s->kel || !s->kel->dat_path[0]) return -1;
    if (lba < s->kel_origin) return -1;
    uint64_t byte_off = (uint64_t)(lba - s->kel_origin) * SECTOR;
    uint64_t nbytes   = (uint64_t)count * SECTOR;

    FILE *f = fopen(s->kel->dat_path, "rb");
    if (!f) { perror("sif_cd_read: open KEL.DAT"); return -1; }
    if (fseeko(f, (off_t)byte_off, SEEK_SET) != 0) { fclose(f); return -1; }

    /* copy straight into guest memory at dst (bounded by RAM) */
    uint8_t *dst = ee_host_ptr(s->ee, ee_canon(dst_ee_addr));
    uint32_t guest = ee_canon(dst_ee_addr) & EE_RAM_MASK;
    if ((uint64_t)guest + nbytes > EE_RAM_SIZE) nbytes = EE_RAM_SIZE - guest;
    size_t got = fread(dst, 1, (size_t)nbytes, f);
    fclose(f);
    return (long)got;
}

/* ------------------------------------------------------------------ *
 * fileio: resolve a path against KEL virtual paths, then host fs     *
 * ------------------------------------------------------------------ */
static const char *strip_device(const char *path) {
    const char *p = path;
    const char *colon = strchr(p, ':');
    if (colon) p = colon + 1;          /* drop "cdrom0:" / "host:" / "pfs0:" */
    while (*p == '/' || *p == '\\') p++;
    return p;
}
long sif_fileio_read_path(sif_dev *s, const char *path, uint32_t dst_ee_addr,
                          uint32_t max_bytes) {
    const char *vp = strip_device(path);
    if (getenv("DIRGE_TRACE"))
        fprintf(stderr,"[sif] FILEIO-READ path='%s' -> '%s' dst=%08x\n", path, vp, dst_ee_addr);
    uint8_t *dst = ee_host_ptr(s->ee, ee_canon(dst_ee_addr));

    /* 1) KEL virtual path (decompresses via kel.c if the entry is zlib). */
    if (s->kel) {
        const kel_entry *e = kel_find_path(s->kel, vp);
        if (e) {
            uint8_t *buf = NULL;
            long n = kel_read(s->kel, e, &buf);
            if (n < 0) return -1;
            uint32_t guest = ee_canon(dst_ee_addr) & EE_RAM_MASK;
            uint64_t cap = (uint64_t)EE_RAM_SIZE - guest;
            uint64_t cp = (uint64_t)n;
            if (max_bytes && cp > max_bytes) cp = max_bytes;
            if (cp > cap) cp = cap;
            memcpy(dst, buf, (size_t)cp);
            free(buf);
            return (long)cp;
        }
    }
    /* 2) host filesystem fallback. */
    FILE *f = fopen(vp, "rb");
    if (!f) return -1;
    uint32_t guest = ee_canon(dst_ee_addr) & EE_RAM_MASK;
    uint64_t cap = (uint64_t)EE_RAM_SIZE - guest;
    if (max_bytes && cap > max_bytes) cap = max_bytes;
    size_t got = fread(dst, 1, (size_t)cap, f);
    fclose(f);
    return (long)got;
}

/* ------------------------------------------------------------------ *
 * RPC dispatch (HLE of sceSifCallRpc)                                *
 * ------------------------------------------------------------------ */
int sif_dev_call_rpc(sif_dev *s, uint32_t server, int func,
                 uint32_t send_addr, uint32_t send_size,
                 uint32_t recv_addr, uint32_t recv_size,
                 int completion_sema) {
    (void)send_size; (void)recv_addr; (void)recv_size;
    int rc = -1;

    if (server == SIF_RPC_CD && func == 1) {
        /* arg block {LBA@+0, count@+4, dst@+8} (FUN_00446af0 @0x493bc0). */
        uint32_t lba   = ee_lw(s->ee, send_addr + 0x00);
        uint32_t count = ee_lw(s->ee, send_addr + 0x04);
        uint32_t dst   = ee_lw(s->ee, send_addr + 0x08);
        long got = sif_cd_read(s, lba, count, dst);
        rc = (got >= 0) ? 0 : -1;
    } else if (server == SIF_RPC_FILEIO && func == 5) {
        /* Best-effort: the fileio command block (0x30 bytes @0x509c00) is an
         * IOP fileio protocol; for M1 we service a read when the block's
         * first word points at a NUL-terminated path in guest RAM and the
         * 2nd/3rd words are dst/len.  Flagged in the report as partial. */
        uint32_t pathp = ee_lw(s->ee, send_addr + 0x00);
        uint32_t dst   = ee_lw(s->ee, send_addr + 0x04);
        uint32_t len   = ee_lw(s->ee, send_addr + 0x08);
        char path[256]; size_t i = 0;
        for (; i < sizeof(path) - 1; i++) {
            char c = (char)ee_lbu(s->ee, pathp + (uint32_t)i);
            path[i] = c; if (!c) break;
        }
        path[i] = 0;
        long got = sif_fileio_read_path(s, path, dst, len);
        rc = (got >= 0) ? 0 : -1;
    } else if (server == 0x506440u) {
        /* The game's custom IOP filesystem RPC module (its own FS driver, loaded
         * from IOPRP.IMG), used for ALL real file I/O in this build (open=0,
         * read=0x13, stat=0xff, ...).  It is NOT implemented for M1: faking
         * success would be dishonest (no data transfers), so we give a CLEAN
         * rejection -- recv (accepted flag) = 0, which the callers (e.g.
         * FUN_0044e030) treat as a graceful "operation failed" rather than
         * diverging on an uninitialised buffer.  recv=garbage (the old unhandled
         * path) is what left dispatch tables full of wild pointers.
         * TODO(M1-finish): back this server with src/kel.c (resolve path ->
         * KEL.DAT read) to deliver real FILELIST.BIN + asset bytes. */
        ee_sw(s->ee, recv_addr, 0u);                 /* request rejected (clean) */
        rc = 0;                                       /* transport ok, op failed  */
    } else if (server == 0x5048a8u) {
        /* disc-info RPC (sid 0x592, FUN_004464c8 recv 0x10): report a PS2 DVD
         * present (recv+0xc != 0xff/0xfe) so the region/mode branch is normal. */
        for (int k = 0; k < 4; k++) ee_sw(s->ee, recv_addr + 4u*k, 0);
        ee_sb(s->ee, recv_addr + 0x0c, 0x14);        /* CdDiskType ~ PS2 DVD */
        rc = 0;
    } else if (server == 0x506500u) {
        ee_sw(s->ee, recv_addr, 1u);                 /* sid 0x3: accepted */
        rc = 0;
    } else {
        fprintf(stderr, "[sif] unhandled RPC server=%08x func=%d\n", server, func);
    }

    /* mirror the async RPC completion callback: signal the sema the EE
     * WaitSema's on (sceSifCallRpc mode 0 path). */
    if (completion_sema >= 0 && s->ee && s->ee->host)
        hle_sema_signal((hle_ctx*)s->ee->host, completion_sema);
    return rc;
}

/* ------------------------------------------------------------------ *
 * SBUS MMIO handshake (routed from hle/mmio.c)                       *
 * ------------------------------------------------------------------ */
uint64_t sif_sbus_load(sif_dev *s, uint32_t addr) {
    if (!s) return (addr == SIF_SMFLAG) ? 0x40000 : 0;
    if (addr == SIF_SMFLAG) return s->iop_ready ? 0x40000 : 0;
    return 0;
}
void sif_sbus_store(sif_dev *s, uint32_t addr, uint64_t v) {
    (void)s; (void)addr; (void)v;   /* EE->IOP doorbells: noop for M1 */
}
