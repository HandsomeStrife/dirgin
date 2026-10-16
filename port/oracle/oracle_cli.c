/* =====================================================================
 * oracle_cli.c  --  general command-line driver for the EE oracle.
 *
 * Set up guest memory, call ONE recompiled function, dump regions back out.
 * This is the reusable validator entry point: point it at any func_<addr>,
 * inject the inputs, read the outputs -- e.g. to diff a native/engine
 * reimplementation against the real recompiled code.
 *
 * Usage:
 *   oracle-cli [options] --call 0xADDR
 *
 * Options (repeatable where sensible):
 *   --elf PATH            override the EE ELF (default: game SLES_541.85)
 *   --io                  open FILELIST/KEL + prime SIF (for file-I/O funcs)
 *   --sp 0xADDR           set the guest stack pointer
 *   --write ADDR:FILE     load a host file into guest RAM at ADDR
 *   --write ADDR=HH..     write literal hex bytes at ADDR
 *   --alloc-file FILE     oracle_alloc + load FILE; prints the chosen addr
 *   --a0 V .. --a3 V      set $a0..$a3 (also --t0..--t3)
 *   --arg N:V             set arg N (0..3=a,4..7=t,>=8=stack)
 *   --reg IDX:V           set GPR by R_ index (decimal)
 *   --call 0xADDR         the function to run (required)
 *   --dump ADDR:LEN       hex-dump LEN bytes at ADDR after the call
 *   --dump-mat4 ADDR:N[:row|col]   dump N 4x4 matrices as JSON
 *
 * Values accept 0x-hex or decimal.
 * ===================================================================== */
#define _GNU_SOURCE
#include "oracle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t parse_u32(const char *s){ return (uint32_t)strtoul(s,0,0); }

static void do_write(oracle *o, const char *spec){
    /* ADDR:FILE  or  ADDR=HEX */
    char *colon = strchr(spec,':'); char *eq = strchr(spec,'=');
    if (eq && (!colon || eq<colon)){
        char *addr_s = strndup(spec, (size_t)(eq-spec));
        uint32_t addr = parse_u32(addr_s); free(addr_s);
        const char *hex = eq+1; size_t hl=strlen(hex);
        for (size_t i=0;i+1<hl;i+=2){
            char b[3]={hex[i],hex[i+1],0}; uint8_t v=(uint8_t)strtoul(b,0,16);
            oracle_write_mem(o, addr+(uint32_t)(i/2), &v, 1);
        }
        fprintf(stderr,"[cli] wrote %zu literal bytes at %08x\n", hl/2, addr);
    } else if (colon){
        char *addr_s = strndup(spec,(size_t)(colon-spec));
        uint32_t addr = parse_u32(addr_s); free(addr_s);
        const char *path = colon+1;
        FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"[cli] open %s\n",path); return; }
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        uint8_t *buf=malloc((size_t)sz); if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){fclose(f);free(buf);return;}
        fclose(f);
        oracle_write_mem(o, addr, buf, (size_t)sz); free(buf);
        fprintf(stderr,"[cli] loaded %ld bytes of %s at %08x\n", sz, path, addr);
    }
}

int main(int argc, char **argv){
    const char *elf=NULL; int want_io=0; uint32_t sp=0; int have_sp=0;
    uint32_t call_addr=0; int have_call=0;
    /* queue dumps so they run after the call */
    const char *dumps[64]; int ndump=0;
    const char *mats[64];  int nmat=0;

    oracle *o=NULL;
    /* two-pass: build machine first (need --elf/--io before writes) */
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--elf") && i+1<argc) elf=argv[++i];
        else if (!strcmp(argv[i],"--io")) want_io=1;
    }
    o = oracle_create(elf, want_io);
    if (!o){ fprintf(stderr,"[cli] oracle_create failed\n"); return 1; }

    static const char *tnames[4]={"--t0","--t1","--t2","--t3"};
    static const char *anames[4]={"--a0","--a1","--a2","--a3"};

    for (int i=1;i<argc;i++){
        const char *a=argv[i];
        if (!strcmp(a,"--elf")||!strcmp(a,"--io")){ if(!strcmp(a,"--elf")) i++; continue; }
        else if (!strcmp(a,"--sp") && i+1<argc){ sp=parse_u32(argv[++i]); have_sp=1; oracle_set_sp(o,sp); }
        else if (!strcmp(a,"--write") && i+1<argc) do_write(o, argv[++i]);
        else if (!strcmp(a,"--alloc-file") && i+1<argc){
            size_t sz; uint32_t base=oracle_load_file(o,argv[++i],&sz);
            fprintf(stderr,"[cli] alloc-file -> base=%08x size=%zu\n", base, sz);
        }
        else if (!strcmp(a,"--arg") && i+1<argc){
            char *c=strchr(argv[++i],':'); if(c){ *c=0; oracle_set_arg(o,atoi(argv[i]),parse_u32(c+1)); }
        }
        else if (!strcmp(a,"--reg") && i+1<argc){
            char *c=strchr(argv[++i],':'); if(c){ *c=0; oracle_set_reg(o,atoi(argv[i]),parse_u32(c+1)); }
        }
        else if (!strcmp(a,"--call") && i+1<argc){ call_addr=parse_u32(argv[++i]); have_call=1; }
        else if (!strcmp(a,"--dump") && i+1<argc){ if(ndump<64) dumps[ndump++]=argv[++i]; }
        else if (!strcmp(a,"--dump-mat4") && i+1<argc){ if(nmat<64) mats[nmat++]=argv[++i]; }
        else {
            int matched=0;
            for (int k=0;k<4;k++){ if(!strcmp(a,anames[k])&&i+1<argc){ oracle_set_arg(o,k,parse_u32(argv[++i])); matched=1; } }
            for (int k=0;k<4;k++){ if(!strcmp(a,tnames[k])&&i+1<argc){ oracle_set_arg(o,4+k,parse_u32(argv[++i])); matched=1; } }
            if (!matched) fprintf(stderr,"[cli] unknown/!arg: %s\n", a);
        }
    }
    (void)have_sp;

    if (!have_call){ fprintf(stderr,"[cli] no --call given\n"); oracle_destroy(o); return 2; }

    fprintf(stderr,"[cli] calling func_%08x ...\n", call_addr);
    int rc = oracle_call(o, call_addr);
    if (rc==0) fprintf(stderr,"[cli] returned OK, $v0=%08x\n", oracle_ret(o));
    else if (rc==-1) fprintf(stderr,"[cli] ERROR: %s\n", oracle_last_fault(o));
    else fprintf(stderr,"[cli] FAULT during call: %s\n", oracle_last_fault(o));

    for (int i=0;i<ndump;i++){
        char *c=strchr(dumps[i],':'); if(!c) continue;
        char *as=strndup(dumps[i],(size_t)(c-dumps[i])); uint32_t ad=parse_u32(as); free(as);
        uint32_t len=parse_u32(c+1);
        printf("---- dump %08x +%u ----\n", ad, len);
        oracle_dump_hex(o, ad, len, stdout);
    }
    for (int i=0;i<nmat;i++){
        /* ADDR:N[:row|col] */
        char tmp[64]; strncpy(tmp,mats[i],sizeof tmp-1); tmp[sizeof tmp-1]=0;
        char *p1=strchr(tmp,':'); if(!p1) continue; *p1=0;
        char *p2=strchr(p1+1,':');
        int row=0; if(p2){ *p2=0; row = !strcmp(p2+1,"row"); }
        uint32_t ad=parse_u32(tmp); int n=atoi(p1+1);
        printf("---- mat4 %08x x%d (%s) ----\n", ad, n, row?"row":"col");
        oracle_dump_mat4_json(o, ad, n, row, stdout);
    }

    oracle_destroy(o);
    return (rc==0)?0:3;
}
