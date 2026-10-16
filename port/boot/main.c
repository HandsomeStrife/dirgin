/* =====================================================================
 * main.c  --  Dirge of Cerberus PC port, MILESTONE 1 headless-boot harness.
 *
 * Links every workstream into one program and runs the recompiled EE boot
 * path with rendering stubbed to the GS log backend:
 *   (a) mmap 32 MB guest RAM + 16 KB scratchpad (ee_runtime.h memory model);
 *   (b) load every PT_LOAD segment of the EE ELF (SLES_541.85) into guest RAM
 *       at (vaddr & 0x01FFFFFF), zeroing .bss;
 *   (c) set up argc/argv at the fixed boot-args address (0x1001C0) so crt0 and
 *       main FUN_00248528 see argv[0]="cdrom0:\SLES_541.85;1" (cdrom boot);
 *   (d) init hle (kernel/threads/vblank, PAL 50 Hz), sif (FILELIST.BIN+KEL.DAT),
 *       gs (log backend), vu1 (stub), pad (released);
 *   (e) start a guest thread at the ELF entry and run the cooperative
 *       scheduler for a bounded number of vblank frames or until trap/exit.
 *
 * Env knobs:  DIRGE_FRAMES=<n> (default 300), DIRGE_FAST=1 (unpaced vblanks),
 *             DIRGE_KEL_ORIGIN=<lba> (KEL.DAT disc-LBA base; default 0),
 *             DIRGE_TRACE=1 (verbose sif/state tracing).
 * ===================================================================== */
#define _GNU_SOURCE
#include "ee_runtime.h"
#include "hle.h"
#include "sif.h"
#include "gs/gs.h"
#include "vu1.h"
#include "pad.h"
#include "kel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

/* ---- boot-path / engine anchors (notes/engine-map.md) --------------- */
#define EE_ENTRY        0x00100000u   /* crt0 FUN_00100000                 */
#define BOOT_ARGS_ADDR  0x001001C0u   /* crt0 reads argc@here, argv@+4     */
#define DAT_STATE       0x004b8b2cu   /* scene-state int                   */
#define DAT_STATE_PREV  0x004b8b30u
#define DAT_MODEFLAGS   0x004b8980u   /* mode-flag bitmask                 */

#define PATH_ELF        "/srv/games/dirge-port/elf/SLES_541.85"
#define PATH_FILELIST   "/srv/games/dirge-port/extract/FILELIST.BIN"
#define PATH_KELDAT     "/srv/games/dirge-port/extract/KEL.DAT"

extern sif_dev *g_sif;                 /* set for boot/sif_glue.c           */
extern unsigned long g_vif1_qwords_seen;
extern void vu1_vif1_sink(void *user, const uint8_t *qwords, size_t nqw);
void vu0_macro_dump_stats(FILE *f);

/* ---- watchdog: dump the host (= recompiled func_ chain) backtrace ----
 * Hangs in the boot path are EE spin loops; the host call stack is the chain
 * of func_<addr> calls, so backtrace() names the spinning guest function. */
static ee_state *g_ee_dbg;   /* for watchdog guest-global dump */
static void on_watchdog(int sig){
    (void)sig;
    if (g_ee_dbg){
        fprintf(stderr,"[watchdog] guest globals: DAT_0046b34c(worker-ready)=%d "
                "DAT_004bce4c=%08x DAT_0046b348=%08x\n",
                (int)ee_lw(g_ee_dbg,0x0046b34c), ee_lw(g_ee_dbg,0x004bce4c),
                ee_lw(g_ee_dbg,0x0046b348));
    }
    void *bt[96]; int n = backtrace(bt, 96);
    const char msg[] = "\n[watchdog] boot appears stuck; host backtrace "
                       "(= guest func_ chain, innermost first):\n";
    ssize_t _w = write(2, msg, sizeof msg - 1); (void)_w;
    backtrace_symbols_fd(bt, n, 2);
    vu0_macro_dump_stats(stderr);
    _exit(99);
}

/* ---- tiny little-endian ELF32 reader -------------------------------- */
static uint32_t rd32(const uint8_t *p){ uint32_t v; memcpy(&v,p,4); return v; }
static uint16_t rd16(const uint8_t *p){ uint16_t v; memcpy(&v,p,2); return v; }

static int load_elf(ee_state *ee, const char *path, uint32_t *entry_out){
    FILE *f = fopen(path, "rb");
    if (!f){ perror("open ELF"); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz){ fclose(f); free(buf); return -1; }
    fclose(f);

    if (!(buf[0]==0x7f && buf[1]=='E' && buf[2]=='L' && buf[3]=='F')){
        fprintf(stderr, "not an ELF\n"); free(buf); return -1; }
    uint32_t e_entry = rd32(buf + 24);
    uint32_t e_phoff = rd32(buf + 28);
    uint16_t e_phentsize = rd16(buf + 42);
    uint16_t e_phnum = rd16(buf + 44);
    *entry_out = e_entry;

    printf("[boot] ELF entry=%08x phnum=%u\n", e_entry, e_phnum);
    for (uint16_t i = 0; i < e_phnum; i++){
        const uint8_t *ph = buf + e_phoff + (size_t)i * e_phentsize;
        uint32_t p_type   = rd32(ph + 0);
        uint32_t p_offset = rd32(ph + 4);
        uint32_t p_vaddr  = rd32(ph + 8);
        uint32_t p_filesz = rd32(ph + 16);
        uint32_t p_memsz  = rd32(ph + 20);
        if (p_type != 1 /*PT_LOAD*/) continue;
        uint32_t guest = p_vaddr & EE_RAM_MASK;
        if ((uint64_t)guest + p_memsz > EE_RAM_SIZE){
            fprintf(stderr, "[boot] segment %u vaddr=%08x memsz=%08x exceeds RAM\n",
                    i, p_vaddr, p_memsz); continue;
        }
        if (p_filesz) memcpy(ee->ram + guest, buf + p_offset, p_filesz);
        if (p_memsz > p_filesz) memset(ee->ram + guest + p_filesz, 0, p_memsz - p_filesz);
        printf("[boot]  PT_LOAD vaddr=%08x filesz=%08x memsz=%08x -> ram+%08x%s\n",
               p_vaddr, p_filesz, p_memsz, guest, (p_memsz>p_filesz)?" (+bss)":"");
    }
    free(buf);
    return 0;
}

/* ---- place argc/argv for the cdrom boot path ------------------------ */
static void setup_argv(ee_state *ee){
    const char *arg0 = "cdrom0:\\SLES_541.85;1";
    /* layout at 0x1001C0: [argc][argv0ptr][argv1ptr=0][... strings ...] */
    uint32_t argc_addr = BOOT_ARGS_ADDR;
    uint32_t argv_addr = BOOT_ARGS_ADDR + 4;      /* argv[] array          */
    uint32_t str_addr  = BOOT_ARGS_ADDR + 0x40;   /* string storage        */
    ee_sw(ee, argc_addr, 1);
    ee_sw(ee, argv_addr + 0, str_addr);            /* argv[0] -> string     */
    ee_sw(ee, argv_addr + 4, 0);                   /* argv[1] = NULL        */
    for (uint32_t i = 0; ; i++){
        ee_sb(ee, str_addr + i, (uint8_t)arg0[i]);
        if (!arg0[i]) break;
    }
    printf("[boot] argv[0]=\"%s\" @ %08x (argc@%08x)\n", arg0, str_addr, argc_addr);
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);     /* keep the boot narrative on crash */
    const char *e;
    int    frames   = (e=getenv("DIRGE_FRAMES")) ? atoi(e) : 300;
    int    fast     = getenv("DIRGE_FAST") != NULL;
    uint32_t kel_origin = (e=getenv("DIRGE_KEL_ORIGIN")) ? (uint32_t)strtoul(e,0,0) : 0;
    int    trace    = getenv("DIRGE_TRACE") != NULL;
    int    watchdog = (e=getenv("DIRGE_WATCHDOG")) ? atoi(e) : 0;
    if (watchdog > 0){ signal(SIGALRM, on_watchdog); alarm((unsigned)watchdog); }

    /* ---- (a) guest machine ------------------------------------------ */
    ee_state ee_storage; ee_state *ee = &ee_storage;
    memset(ee, 0, sizeof *ee);
    ee->ram = mmap(NULL, EE_RAM_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    ee->spr = mmap(NULL, EE_SPR_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ee->ram == MAP_FAILED || ee->spr == MAP_FAILED){ perror("mmap"); return 1; }
    g_ee_dbg = ee;

    /* ---- (b) load the EE ELF ---------------------------------------- */
    uint32_t entry = EE_ENTRY;
    if (load_elf(ee, PATH_ELF, &entry) != 0) return 1;

    /* ---- (c) boot args ---------------------------------------------- */
    setup_argv(ee);

    /* ---- (d) init + wire the HLE and backends ----------------------- */
    hle_ctx *h = hle_create(ee);              /* sets ee->host            */
    gs_state *gs = gs_create(gs_backend_log(), 640, 448);
    vu1_ctx  *vu = vu1_create();
    pad_state *pad = pad_create(ee);
    sif_dev  *sif = sif_create(ee);
    g_sif = sif;

    kel_archive kel;
    if (kel_open(&kel, PATH_FILELIST, PATH_KELDAT) != 0){
        fprintf(stderr, "[boot] kel_open failed (FILELIST.BIN / KEL.DAT)\n");
        /* continue: file loads will fail but CPU/kernel bring-up still useful */
    } else {
        printf("[boot] kel_open OK: %u entries, KEL.DAT=%s\n", kel.count, kel.dat_path);
    }
    sif_set_kel(sif, &kel, kel_origin);
    printf("[boot] kel_origin(LBA base)=%u (DIRGE_KEL_ORIGIN)\n", kel_origin);

    /* Prime the synthetic-IOP SIF handshake.  The EE's SIF CMD/RPC bring-up
     * (sceSifInitCmd FUN_00452d40, sceSifInitRpc FUN_00453410) busy-waits on
     * IOP responses (SMFLAG bit 0x20000; a SIFCMD reply) that a real IOP would
     * send.  We have no IOP, but our high-level sceSifCallRpc is fully HLE'd
     * (it bypasses the SIFCMD layer), so we just report "IOP already up": the
     * init routines then take their non-spinning branches.  Documented HLE
     * completion of the synthetic-IOP model. */
    sif_set_reg(sif, 0, 1);                 /* IOP main flag present         */
    sif_set_reg(sif, 1, 1);
    sif_set_reg(sif, 2, 1);                 /* IOP sub-buffer addr present   */
    sif_set_reg(sif, 4, 0x10000u|0x20000u); /* SMFLAG: EE+IOP SIF initialised*/
    sif_dev_reboot_iop(sif);                /* iop_ready + SMFLAG 0x40000    */

    hle_set_gs(h, gs);
    hle_set_sif(h, sif);
    hle_set_pad(h, pad);
    hle_set_vif1_sink(h, vu1_vif1_sink, vu);

    /* ---- (e) start guest thread at ELF entry, run bounded ----------- */
    int tid = hle_thread_create(h, entry, /*gp*/0, /*stack_top*/0x02000000u, 0);
    if (tid < 0){ fprintf(stderr, "[boot] failed to create boot thread\n"); return 1; }
    hle_thread_start(h, tid, /*arg*/0);

    printf("[boot] starting boot thread tid=%d at %08x; running up to %d frames%s\n",
           tid, entry, frames, fast ? " (fast/unpaced)" : " (paced)");

    uint32_t last_state = ee_lw(ee, DAT_STATE);
    uint32_t last_flags = ee_lw(ee, DAT_MODEFLAGS);
    printf("[boot] initial scene-state DAT_004b8b2c=%d  modeflags=%08x\n",
           (int)last_state, last_flags);

    int reached_loop = 0;
    for (int fr = 0; fr < frames; fr++){
        if (fast) hle_set_vblank_hz(h, 0.0);      /* override SetGsCrt pacing */
        uint64_t before = hle_vblank_count(h);
        hle_run(h, 1);
        uint64_t after = hle_vblank_count(h);
        if (after > before) reached_loop = 1;     /* a vblank was consumed    */

        uint32_t st = ee_lw(ee, DAT_STATE);
        uint32_t fl = ee_lw(ee, DAT_MODEFLAGS);
        if (st != last_state){
            printf("[boot] frame %d: scene-state %d -> %d (prev=%d)\n",
                   fr, (int)last_state, (int)st, (int)ee_lw(ee, DAT_STATE_PREV));
            last_state = st;
        }
        if (fl != last_flags){
            if (trace) printf("[boot] frame %d: modeflags %08x -> %08x\n", fr, last_flags, fl);
            last_flags = fl;
        }
        if (hle_exit_requested(h)){
            printf("[boot] frame %d: exit_requested (loadexec target=%08x)\n",
                   fr, hle_loadexec_target(h));
            break;
        }
    }

    printf("\n========== M1 BOOT RUN SUMMARY ==========\n");
    printf("vblank frames driven : %llu\n", (unsigned long long)hle_vblank_count(h));
    printf("reached per-frame loop: %s\n", reached_loop ? "YES" : "no");
    printf("final scene-state    : %d (prev %d)\n",
           (int)ee_lw(ee, DAT_STATE), (int)ee_lw(ee, DAT_STATE_PREV));
    printf("final modeflags      : %08x\n", ee_lw(ee, DAT_MODEFLAGS));
    printf("VIF1 qwords to VU1   : %lu\n", g_vif1_qwords_seen);
    printf("exit_requested       : %d\n", hle_exit_requested(h));
    vu0_macro_dump_stats(stdout);
    printf("=========================================\n");

    kel_close(&kel);
    gs_destroy(gs); vu1_destroy(vu); pad_destroy(pad);
    sif_destroy(sif); hle_destroy(h);
    munmap(ee->ram, EE_RAM_SIZE); munmap(ee->spr, EE_SPR_SIZE);
    return 0;
}
