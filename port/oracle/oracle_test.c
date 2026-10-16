/* =====================================================================
 * oracle_test.c  --  self-test proving the oracle actually runs real
 * recompiled EE code and reads back correct results.
 *
 * Two pure, deterministic functions (no scheduler / no blocking):
 *   (1) FUN_00447c30  -- the engine's memset(dst, val, n)  [MMI pcpyh/pcpyld]
 *   (2) FUN_0026138c  -- 4x4 matrix * vec4 via VU0 macro vmulabc/vmaddabc/
 *                        vmaddbc  (the SAME ops the bone-palette builder uses)
 *
 * If both pass, the harness can inject data, call a func_<addr>, run VU0
 * macro math, and read the result -- which is the whole contract.
 * ===================================================================== */
#include "oracle.h"
#include <stdio.h>
#include <string.h>

static uint32_t fbits(float f){ uint32_t b; memcpy(&b,&f,4); return b; }
static float    bfloat(uint32_t b){ float f; memcpy(&f,&b,4); return f; }

static int test_memset(oracle *o){
    uint32_t dst = oracle_alloc(o, 256);
    uint8_t fill[256]; memset(fill,0xAA,sizeof fill);
    oracle_write_mem(o,dst,fill,256);
    oracle_set_arg(o,0,dst);
    oracle_set_arg(o,1,0x55);
    oracle_set_arg(o,2,64);
    int rc = oracle_call(o, 0x00447c30);
    if (rc){ printf("  memset: CALL FAILED rc=%d (%s)\n",rc,oracle_last_fault(o)); return 1; }
    uint8_t out[256]; oracle_read_mem(o,dst,out,256);
    int bad=0;
    for (int i=0;i<64;i++)   if(out[i]!=0x55) bad++;
    for (int i=64;i<256;i++) if(out[i]!=0xAA) bad++;
    printf("  memset FUN_00447c30: %s ($v0=%08x, first=%02x tail=%02x)\n",
           bad?"FAIL":"PASS", oracle_ret(o), out[0], out[64]);
    return bad?1:0;
}

/* write a column-major 4x4 at guest addr: col c, row r at (c*4+r)*4 */
static void wmat(oracle *o, uint32_t a, const float m[16]){
    for (int i=0;i<16;i++) oracle_sw(o, a+(uint32_t)i*4, fbits(m[i]));
}
static void wvec(oracle *o, uint32_t a, float x,float y,float z,float w){
    oracle_sw(o,a+0,fbits(x)); oracle_sw(o,a+4,fbits(y));
    oracle_sw(o,a+8,fbits(z)); oracle_sw(o,a+12,fbits(w));
}

static int test_matvec(oracle *o){
    /* column-major: col0,col1,col2 = identity basis; col3 = translation */
    float m[16] = {
        1,0,0,0,   /* col0 */
        0,1,0,0,   /* col1 */
        0,0,1,0,   /* col2 */
        5,6,7,1    /* col3 = translation (5,6,7) */
    };
    uint32_t outa=oracle_alloc(o,16), mata=oracle_alloc(o,64), veca=oracle_alloc(o,16);
    wmat(o,mata,m);
    wvec(o,veca, 2.0f,3.0f,4.0f,1.0f);
    oracle_set_arg(o,0,outa);
    oracle_set_arg(o,1,mata);
    oracle_set_arg(o,2,veca);
    int rc=oracle_call(o,0x0026138c);
    if (rc){ printf("  matvec: CALL FAILED rc=%d (%s)\n",rc,oracle_last_fault(o)); return 1; }
    float r[4];
    for (int i=0;i<4;i++) r[i]=bfloat(oracle_lw(o,outa+(uint32_t)i*4));
    /* expect (2+5, 3+6, 4+7, 1) = (7,9,11,1) */
    int ok = (r[0]==7.0f && r[1]==9.0f && r[2]==11.0f && r[3]==1.0f);
    printf("  matvec FUN_0026138c: %s  result=(%.3f, %.3f, %.3f, %.3f) expect (7,9,11,1)\n",
           ok?"PASS":"FAIL", r[0],r[1],r[2],r[3]);
    return ok?0:1;
}

int main(void){
    printf("== oracle self-test ==\n");
    oracle *o = oracle_create(NULL, /*want_io=*/0);
    if (!o){ printf("oracle_create FAILED\n"); return 1; }
    int fails=0;
    fails += test_memset(o);
    fails += test_matvec(o);
    printf("== %s ==\n", fails?"SELF-TEST FAILURES":"ALL SELF-TESTS PASS");
    oracle_destroy(o);
    return fails?1:0;
}
