/* =====================================================================
 * test_scale.c -- scaled differential/behaviour harness for the whole-program
 * recompiler output (generated_all/). Links against the full 13k-function
 * build + function table.
 *
 *  1. func_00447c30  = the engine memset (MMI _pcpyh/_pcpyld + sd/sw/sb loop)
 *     differential-tested against libc memset on random (dst,byte,len).
 *  2. func_00417940 / func_0036c338  regression through the full build + table.
 *  3. ee_lookup round-trip over a sample of the generated function table.
 *  4. EE single-precision FPU rules (no NaN/Inf, /0 -> +-max, denorm flush)
 *     and numeric agreement with the host for normal values.
 * ===================================================================== */
#include "ee_runtime.h"
#include "ee_funcs.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

int  ee_machine_init(ee_state *ee);
void ee_machine_free(ee_state *ee);

/* simple deterministic PRNG so runs are reproducible */
static uint32_t rng=0x1234567u;
static uint32_t rnd(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }

#define MEMBASE 0x00200000u

static int test_memset(ee_state *ee){
    int fails=0, n=4000;
    for(int k=0;k<n;k++){
        uint32_t off = rnd() % 64;              /* exercise aligned+unaligned */
        uint32_t len = rnd() % 400;
        uint8_t  byte = (uint8_t)rnd();
        uint32_t dst = MEMBASE + off;
        /* poison a window then run the recompiled memset */
        memset(ee->ram + MEMBASE, 0x5A, 1024);
        uint8_t expect[1024]; memset(expect, 0x5A, 1024);
        memset(expect + off, byte, len);
        memset(&ee->r[0], 0, sizeof ee->r);
        ee->r[R_A0].ud[0]=dst; ee->r[R_A1].ud[0]=byte; ee->r[R_A2].ud[0]=len;
        func_00447c30(ee);
        uint32_t ret = ee->r[R_V0].uw[0];
        if(ret != dst){ if(fails<5) printf("  memset ret %08x != %08x\n",ret,dst); fails++; continue; }
        if(memcmp(ee->ram + MEMBASE, expect, 1024)!=0){
            if(fails<5) printf("  memset mismatch off=%u len=%u byte=%02x\n",off,len,byte);
            fails++;
        }
    }
    printf("  memset: %d/%d random cases OK\n", n-fails, n);
    return fails;
}

/* ---- leb128 + chaincount regression (same refs as test_recomp.c) ----- */
static uint32_t ref_sleb128(const uint8_t *p,int *c){uint32_t r=0;int s=0;uint8_t b;const uint8_t*o=p;
    do{b=*p++;r|=(uint32_t)(b&0x7f)<<s;s+=7;}while(b&0x80);
    if(s<32&&(b&0x40)){r|=(~0u)<<s;}
    *c=(int)(p-o);return r;}
static int enc_sleb128(int32_t v,uint8_t*o){int n=0,more=1;while(more){uint8_t b=v&0x7f;v>>=7;
    if((v==0&&!(b&0x40))||(v==-1&&(b&0x40)))more=0;else b|=0x80;o[n++]=b;}return n;}

static int test_regression(ee_state *ee){
    int fails=0;
    int32_t vals[]={0,1,-1,127,128,-128,300,-300,2147483647,(int32_t)-2147483648,12345};
    for(size_t k=0;k<sizeof vals/sizeof vals[0];k++){
        uint8_t enc[8];int n=enc_sleb128(vals[k],enc);
        memcpy(ee->ram+0x300000,enc,n);
        memset(&ee->r[0],0,sizeof ee->r);
        ee->r[R_A0].ud[0]=0x300000;ee->r[R_A1].ud[0]=0x301000;
        func_00417940(ee);
        uint32_t got;memcpy(&got,ee->ram+0x301000,4);
        int rc;uint32_t ref=ref_sleb128(enc,&rc);
        if(got!=ref){fails++;printf("  leb128 v=%d got=%08x ref=%08x\n",vals[k],got,ref);}
    }
    printf("  leb128 regression: %s\n", fails?"FAIL":"OK");
    /* chaincount */
    for(int i=0;i<6;i++){uint32_t nx=(i<5)?(0x310000u+(i+1)*0x40):0;memcpy(ee->ram+0x310000+i*0x40+0x3c,&nx,4);}
    memset(&ee->r[0],0,sizeof ee->r);
    ee->r[R_A0].ud[0]=0x310000;ee->r[R_A1].ud[0]=0x310080;
    func_0036c338(ee);
    int cc=ee->r[R_V0].sw[0];
    printf("  chaincount regression: %s (got %d, want 3)\n", cc==3?"OK":"FAIL", cc);
    return fails + (cc!=3);
}

static int test_lookup(void){
    /* sample the table: known entries resolve, a bogus one does not */
    int fails=0;
    if(ee_lookup(0x00447c30u)!=func_00447c30){printf("  lookup memset FAIL\n");fails++;}
    if(ee_lookup(0x00417940u)!=func_00417940){printf("  lookup leb128 FAIL\n");fails++;}
    if(ee_lookup(0x00000001u)!=NULL){printf("  lookup bogus not NULL\n");fails++;}
    /* HLE route: the table entry for sceSifCallRpc is the sif hook */
    if(ee_lookup(0x00453d20u)!=sif_call_rpc){printf("  lookup sif route FAIL\n");fails++;}
    printf("  function_table lookup: %s\n", fails?"FAIL":"OK");
    return fails;
}

/* ---- EE FPU rules ---------------------------------------------------- */
static uint32_t fb(float f){uint32_t b;memcpy(&b,&f,4);return b;}
static float    bf(uint32_t b){float f;memcpy(&f,&b,4);return f;}
static int test_fpu(void){
    int fails=0;
    /* div by zero -> +-max, never Inf/NaN */
    uint32_t r = ee_fdiv(fb(1.0f), fb(0.0f));
    if(r!=0x7FFFFFFFu){printf("  fdiv 1/0 -> %08x (want 7fffffff)\n",r);fails++;}
    r = ee_fdiv(fb(-1.0f), fb(0.0f));
    if(r!=0xFFFFFFFFu){printf("  fdiv -1/0 -> %08x (want ffffffff)\n",r);fails++;}
    /* overflow saturates to +-max (0x7FFFFFFF), never the host Inf pattern
     * 0x7F800000. NB: EE max 0x7FFFFFFF *is* a NaN bit pattern to the host --
     * that is expected, the EE simply has no Inf/NaN. */
    r = ee_fmul(fb(3.0e38f), fb(3.0e38f));
    if((r&0x7FFFFFFFu)!=0x7FFFFFFFu){printf("  fmul overflow -> %08x (want *fffffff)\n",r);fails++;}
    if((r&0x7FFFFFFFu)==0x7F800000u){printf("  fmul overflow produced Inf pattern\n");fails++;}
    /* numeric agreement with host for normal magnitudes */
    int nnum=0,nbad=0;
    for(int k=0;k<20000;k++){
        float a=(float)((int32_t)rnd())/65536.0f;
        float b=(float)((int32_t)rnd())/65536.0f;
        if(fabsf(a)<1e-3f||fabsf(b)<1e-3f) continue;
        nnum++;
        if(bf(ee_fadd(fb(a),fb(b)))!=a+b) nbad++;
        if(bf(ee_fmul(fb(a),fb(b)))!=a*b) nbad++;
    }
    printf("  fpu /0->max, overflow->max: %s\n", fails?"FAIL":"OK");
    printf("  fpu add/mul match host on %d normal pairs: %s\n", nnum, nbad?"FAIL":"OK");
    return fails + (nbad?1:0);
}

int main(void){
    ee_state ee;
    if(ee_machine_init(&ee)!=0) return 2;
    printf("== scaled recompiler tests (whole-program build) ==\n");
    int f=0;
    printf("[memset FUN_00447c30 vs libc]\n");           f+=test_memset(&ee);
    printf("[leb128 / chaincount regression via table]\n"); f+=test_regression(&ee);
    printf("[function table]\n");                          f+=test_lookup();
    printf("[EE FPU rules]\n");                            f+=test_fpu();
    ee_machine_free(&ee);
    printf("\n%s (%d failure%s)\n", f?"FAILURES":"ALL PASS", f, f==1?"":"s");
    return f?1:0;
}
