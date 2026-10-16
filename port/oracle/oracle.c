/* =====================================================================
 * oracle.c  --  implementation of the reusable EE oracle (see oracle.h).
 *
 * Reuses, UNMODIFIED, the recompiled EE corpus (recomp/generated_all) and
 * the workstream runtime (hle/ + sif/ + cop2/ + gs/ + vu/ + input/ +
 * src/kel.c) plus boot/{runtime_hooks,sif_glue,vu1_stub}.c for the COP0 /
 * indirect-call / SIF-glue / VU1-stub seams.  Nothing under port/ other than
 * this directory is edited; everything else is linked as-is.
 * ===================================================================== */
#define _GNU_SOURCE
#include "oracle.h"
#include "hle.h"
#include "sif.h"
#include "gs/gs.h"
#include "vu1.h"
#include "pad.h"
#include "kel.h"

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>

/* externs provided by boot/sif_glue.c + boot/vu1_stub.c (linked in). */
extern sif_dev *g_sif;
extern void vu1_vif1_sink(void *user, const uint8_t *qwords, size_t nqw);

/* Defaults; overridable via env in oracle_create(). */
#define DEF_ELF      "/srv/games/dirge-port/elf/SLES_541.85"
#define DEF_FILELIST "/srv/games/dirge-port/extract/FILELIST.BIN"
#define DEF_KELDAT   "/srv/games/dirge-port/extract/KEL.DAT"

/* Guest memory layout for the oracle arena + stack.  The ELF image + .bss of
 * SLES_541.85 ends well under 0x00800000 (code ~0x1xxxxx..0x45xxxx, globals
 * to ~0x4e0000).  We give the injection/output arena the window
 * [ARENA_BASE, ARENA_TOP) and put the call stack above it, all inside the
 * 32 MB RAM so EE_RAM_MASK addressing stays valid. */
#define ARENA_BASE   0x00800000u
#define ARENA_TOP    0x01E00000u
#define STACK_TOP    0x01FFFF00u   /* 16-byte-ish aligned, below 32 MB end   */

struct oracle {
    ee_state   ee;
    hle_ctx   *h;
    gs_state  *gs;
    vu1_ctx   *vu;
    pad_state *pad;
    sif_dev   *sif;
    kel_archive kel;
    int        kel_open_ok;
    uint32_t   arena;      /* bump pointer                               */
    uint32_t   sp;         /* stack pointer used by calls                */
    char       fault[128];
};

/* ---- ELF32 loader (mirrors boot/main.c load_elf) -------------------- */
static uint32_t rd32(const uint8_t *p){ uint32_t v; memcpy(&v,p,4); return v; }
static uint16_t rd16(const uint8_t *p){ uint16_t v; memcpy(&v,p,2); return v; }

static int load_elf(ee_state *ee, const char *path){
    FILE *f = fopen(path, "rb");
    if (!f){ fprintf(stderr,"[oracle] open ELF '%s' failed\n", path); return -1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(buf); return -1; }
    fclose(f);
    if (!(buf[0]==0x7f&&buf[1]=='E'&&buf[2]=='L'&&buf[3]=='F')){
        fprintf(stderr,"[oracle] not an ELF\n"); free(buf); return -1; }
    uint32_t e_phoff=rd32(buf+28); uint16_t e_phentsize=rd16(buf+42), e_phnum=rd16(buf+44);
    for (uint16_t i=0;i<e_phnum;i++){
        const uint8_t *ph=buf+e_phoff+(size_t)i*e_phentsize;
        if (rd32(ph+0)!=1) continue;                 /* PT_LOAD only */
        uint32_t off=rd32(ph+4), va=rd32(ph+8), fsz=rd32(ph+16), msz=rd32(ph+20);
        uint32_t g=va & EE_RAM_MASK;
        if ((uint64_t)g+msz > EE_RAM_SIZE) continue;
        if (fsz) memcpy(ee->ram+g, buf+off, fsz);
        if (msz>fsz) memset(ee->ram+g+fsz, 0, msz-fsz);
    }
    free(buf);
    return 0;
}

/* ---- fault guard ---------------------------------------------------- */
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_guarding;
static struct oracle *g_active;
static void on_fault(int sig, siginfo_t *si, void *uc){
    (void)uc;
    if (g_guarding && g_active){
        snprintf(g_active->fault, sizeof g_active->fault,
                 "signal %d (%s) at guest/host addr ~%p",
                 sig, sig==SIGSEGV?"SIGSEGV":sig==SIGBUS?"SIGBUS":"?",
                 si?si->si_addr:NULL);
        g_guarding = 0;
        siglongjmp(g_jmp, 1);
    }
    /* not guarding: restore default and re-raise */
    signal(sig, SIG_DFL);
    raise(sig);
}
static void install_fault_handler(void){
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL);
}

/* ---- lifecycle ------------------------------------------------------ */
oracle *oracle_create(const char *elf_path, int want_io){
    oracle *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    ee_state *ee = &o->ee;
    ee->ram = mmap(NULL, EE_RAM_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    ee->spr = mmap(NULL, EE_SPR_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ee->ram==MAP_FAILED || ee->spr==MAP_FAILED){
        fprintf(stderr,"[oracle] mmap failed\n"); free(o); return NULL; }

    const char *elf = elf_path ? elf_path
                    : (getenv("DIRGE_ELF") ? getenv("DIRGE_ELF") : DEF_ELF);
    if (load_elf(ee, elf)!=0){
        munmap(ee->ram,EE_RAM_SIZE); munmap(ee->spr,EE_SPR_SIZE); free(o); return NULL; }

    /* Wire the same HLE machine the boot harness uses, so a called function
     * that touches MMIO / VU0 / SIF behaves identically.  No boot loop. */
    o->h   = hle_create(ee);                     /* sets ee->host */
    o->gs  = gs_create(gs_backend_log(), 640, 448);
    o->vu  = vu1_create();
    o->pad = pad_create(ee);
    o->sif = sif_create(ee);
    g_sif  = o->sif;

    if (want_io){
        const char *fl = getenv("DIRGE_FILELIST"); if(!fl) fl=DEF_FILELIST;
        const char *kd = getenv("DIRGE_KELDAT");   if(!kd) kd=DEF_KELDAT;
        if (kel_open(&o->kel, fl, kd)==0){
            o->kel_open_ok = 1;
            uint32_t ko = getenv("DIRGE_KEL_ORIGIN") ?
                          (uint32_t)strtoul(getenv("DIRGE_KEL_ORIGIN"),0,0) : 0;
            sif_set_kel(o->sif, &o->kel, ko);
        } else {
            fprintf(stderr,"[oracle] (want_io) kel_open failed; continuing\n");
            sif_set_kel(o->sif, NULL, 0);
        }
        sif_set_reg(o->sif,0,1); sif_set_reg(o->sif,1,1); sif_set_reg(o->sif,2,1);
        sif_set_reg(o->sif,4,0x10000u|0x20000u);
        sif_dev_reboot_iop(o->sif);
    } else {
        sif_set_kel(o->sif, NULL, 0);
    }

    hle_set_gs(o->h,o->gs); hle_set_sif(o->h,o->sif); hle_set_pad(o->h,o->pad);
    hle_set_vif1_sink(o->h, vu1_vif1_sink, o->vu);

    o->arena = ARENA_BASE;
    o->sp    = STACK_TOP;
    ee->r[R_SP].sd[0] = (int32_t)o->sp;
    ee->r[R_RA].sd[0] = 0;                 /* C return edge replaces $ra */

    g_active = o;
    install_fault_handler();
    return o;
}

void oracle_destroy(oracle *o){
    if (!o) return;
    if (o->kel_open_ok) kel_close(&o->kel);
    if (o->gs) gs_destroy(o->gs);
    if (o->vu) vu1_destroy(o->vu);
    if (o->pad) pad_destroy(o->pad);
    if (o->sif) sif_destroy(o->sif);
    if (o->h) hle_destroy(o->h);
    munmap(o->ee.ram, EE_RAM_SIZE); munmap(o->ee.spr, EE_SPR_SIZE);
    if (g_active==o) g_active=NULL;
    free(o);
}

ee_state *oracle_ee(oracle *o){ return &o->ee; }

/* ---- guest memory --------------------------------------------------- */
void oracle_write_mem(oracle *o, uint32_t addr, const void *src, size_t n){
    memcpy(o->ee.ram + (ee_canon(addr)&EE_RAM_MASK), src, n);
}
void oracle_read_mem(oracle *o, uint32_t addr, void *dst, size_t n){
    memcpy(dst, o->ee.ram + (ee_canon(addr)&EE_RAM_MASK), n);
}
uint32_t oracle_lw(oracle *o, uint32_t addr){ return ee_lw(&o->ee, addr); }
void     oracle_sw(oracle *o, uint32_t addr, uint32_t v){ ee_sw(&o->ee, addr, v); }

uint32_t oracle_alloc(oracle *o, size_t n){
    uint32_t a = (o->arena + 0xFu) & ~0xFu;
    if ((uint64_t)a + n > ARENA_TOP) return 0;
    o->arena = a + (uint32_t)n;
    return a;
}
void oracle_reset_arena(oracle *o){ o->arena = ARENA_BASE; }

uint32_t oracle_load_file(oracle *o, const char *path, size_t *size_out){
    FILE *f = fopen(path,"rb"); if(!f){ fprintf(stderr,"[oracle] open '%s'\n",path); return 0; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint32_t base = oracle_alloc(o,(size_t)sz);
    if (!base){ fclose(f); return 0; }
    if (fread(o->ee.ram+(base&EE_RAM_MASK),1,(size_t)sz,f)!=(size_t)sz){ fclose(f); return 0; }
    fclose(f);
    if (size_out) *size_out=(size_t)sz;
    return base;
}

/* ---- register / call setup ------------------------------------------ */
void oracle_set_arg(oracle *o, int i, uint32_t val){
    static const int areg[8] = { R_A0,R_A1,R_A2,R_A3,R_T0,R_T1,R_T2,R_T3 };
    if (i>=0 && i<8) o->ee.r[areg[i]].sd[0] = (int32_t)val;
    else ee_sw(&o->ee, o->sp + (uint32_t)(i-8)*4u, val);
}
void oracle_set_reg(oracle *o, int reg_index, uint64_t val){
    if (reg_index>0 && reg_index<32) o->ee.r[reg_index].ud[0]=val;
}
uint64_t oracle_get_reg(oracle *o, int reg_index){
    return (reg_index>=0&&reg_index<32)? o->ee.r[reg_index].ud[0] : 0;
}
uint32_t oracle_ret(oracle *o){ return EE_GET32(&o->ee, R_V0); }
void oracle_set_sp(oracle *o, uint32_t sp){ o->sp=sp; o->ee.r[R_SP].sd[0]=(int32_t)sp; }

int oracle_call(oracle *o, uint32_t addr){
    ee_func_t f = ee_lookup(addr);
    if (!f){ snprintf(o->fault,sizeof o->fault,"addr %08x not a recompiled func",addr); return -1; }
    o->ee.r[R_SP].sd[0] = (int32_t)o->sp;   /* ensure stack set */
    o->fault[0]=0;
    g_active = o;
    if (sigsetjmp(g_jmp,1)==0){
        g_guarding = 1;
        f(&o->ee);
        g_guarding = 0;
        return 0;
    }
    /* longjmp'd back from on_fault: o->fault populated */
    g_guarding = 0;
    return -2;
}
const char *oracle_last_fault(oracle *o){ return o->fault; }

/* ---- dumping -------------------------------------------------------- */
void oracle_dump_hex(oracle *o, uint32_t addr, size_t n, FILE *f){
    const uint8_t *p = o->ee.ram + (ee_canon(addr)&EE_RAM_MASK);
    for (size_t i=0;i<n;i+=16){
        fprintf(f,"%08x: ", addr+(uint32_t)i);
        for (size_t j=0;j<16;j++){
            if (i+j<n) fprintf(f,"%02x",p[i+j]); else fprintf(f,"  ");
            if (j&1) fputc(' ',f);
        }
        fputc(' ',f);
        for (size_t j=0;j<16 && i+j<n;j++){
            uint8_t c=p[i+j]; fputc((c>=0x20&&c<0x7f)?(int)c:'.', f);
        }
        fputc('\n',f);
    }
}

static float gf(oracle *o, uint32_t a){ uint32_t b=ee_lw(&o->ee,a); float v; memcpy(&v,&b,4); return v; }

void oracle_dump_mat4_json(oracle *o, uint32_t addr, int count, int row_major, FILE *f){
    fprintf(f,"{\n");
    for (int m=0;m<count;m++){
        uint32_t base = addr + (uint32_t)m*0x40u;
        fprintf(f,"  \"%d\": [", m);
        for (int r=0;r<4;r++){
            fprintf(f,"[");
            for (int c=0;c<4;c++){
                uint32_t off = row_major ? (uint32_t)(r*4+c)*4u : (uint32_t)(c*4+r)*4u;
                fprintf(f,"%.6g%s", (double)gf(o,base+off), c<3?", ":"");
            }
            fprintf(f,"]%s", r<3?", ":"");
        }
        fprintf(f,"]%s\n", m<count-1?",":"");
    }
    fprintf(f,"}\n");
}
