/* Numeric regression test for EE COP2 VOPMULA/VOPMSUB cross product.
 * Guards the 2026-10-15 fix (vopmsub was subtracting the same products VOPMULA
 * added -> always 0; now subtracts swapped products -> fd = fs x ft).
 * Build & run from repo root:
 *   gcc -Iport/recomp -o /tmp/t port/cop2/test_vu0_cross.c port/cop2/vu0_macro.c -lm && /tmp/t
 * Exit 0 = PASS. Before the fix, cases 1-3 FAIL (all lanes read 0). */
#include "ee_runtime.h"
#include <stdio.h>
#include <math.h>

/* raw-word builders: ft bits20..16, fs bits15..11, fd bits10..6 */
static uint32_t word(int ft,int fs,int fd){ return (ft<<16)|(fs<<11)|(fd<<6); }

static int fail=0;
static void check(const char*name,float got,float exp){
    float d=fabsf(got-exp);
    int ok=d<=1e-4f*(1.0f+fabsf(exp));
    if(!ok) fail=1;
    printf("  %-18s got=%.5f exp=%.5f %s\n",name,got,exp,ok?"OK":"<<< FAIL");
}

static void cross(ee_state*ee,int fs,int ft,int fd){
    ee_cop2_vopmula(ee, word(ft,fs,0));
    ee_cop2_vopmsub(ee, word(ft,fs,fd));
}

int main(void){
    ee_state ee; ee_vu0 *v=ee_vu0_state(&ee);
    /* case 1: (1,0,0) x (0,1,0) = (0,0,1) */
    v->vf[1][0]=1; v->vf[1][1]=0; v->vf[1][2]=0;
    v->vf[2][0]=0; v->vf[2][1]=1; v->vf[2][2]=0;
    v->vf[3][0]=v->vf[3][1]=v->vf[3][2]=-99;
    cross(&ee,1,2,3);
    printf("case1 (1,0,0)x(0,1,0):\n");
    check("x",v->vf[3][0],0); check("y",v->vf[3][1],0); check("z",v->vf[3][2],1);
    /* case 2: (2,3,4) x (5,6,7) = (-3,6,-3) */
    v->vf[1][0]=2; v->vf[1][1]=3; v->vf[1][2]=4;
    v->vf[2][0]=5; v->vf[2][1]=6; v->vf[2][2]=7;
    cross(&ee,1,2,3);
    printf("case2 (2,3,4)x(5,6,7):\n");
    check("x",v->vf[3][0],-3); check("y",v->vf[3][1],6); check("z",v->vf[3][2],-3);
    /* case 3: in-place fd==fs : (0,0,1)x(1,0,0)=(0,1,0) */
    v->vf[5][0]=0; v->vf[5][1]=0; v->vf[5][2]=1;
    v->vf[6][0]=1; v->vf[6][1]=0; v->vf[6][2]=0;
    cross(&ee,5,6,5);              /* fd aliases fs */
    printf("case3 in-place (0,0,1)x(1,0,0):\n");
    check("x",v->vf[5][0],0); check("y",v->vf[5][1],1); check("z",v->vf[5][2],0);
    printf(fail?"RESULT: FAIL\n":"RESULT: PASS\n");
    return fail;
}
