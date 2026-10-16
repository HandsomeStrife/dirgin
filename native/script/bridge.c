/* bridge.c - native bridge table. See bridge.h. */
#include "bridge.h"
#include "bridge_engine.h"   /* bridge_engine_install() */
#include "bridge_api_gen.h"   /* BRIDGE_API[], BRIDGE_API_COUNT */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv (DIRGE_NO_ENGINE test seam) */

void dirge_engine_init(DirgeEngine *e){
    memset(e,0,sizeof *e);
    e->difficulty=1;     /* NORMAL */
    e->is_tutorial=0;    /* exercise the full non-tutorial calc path */
    /* bring up the real native engine subsystems the bridge drives */
    doc_actor_manager_init(&e->actor_mgr, e->actor_pool, DIRGE_ACTOR_POOL);
    doc_camera_init(&e->camera);
    doc_light_init(&e->lights);
    engine_state_init(&e->estate);
}

static DirgeEngine *ENG(jvm *vm){ return (DirgeEngine*)vm->user; }
static int clampi(int x){ return (x<0)?0:(x>255?255:x); }

/* ---- real pure implementations ---------------------------------------- */
static jval n_isTutorial(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)a;(void)n;(void)d; jval r; r.i=ENG(vm)->is_tutorial; return r; }
static jval n_evt_getflag(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.i=(n>=1)?ENG(vm)->event_flags[clampi(a[0].i)]:0; return r; }
static jval n_evt_setflag(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; if(n>=2) ENG(vm)->event_flags[clampi(a[0].i)]=a[1].i; jval r; r.l=0; return r; }
static jval n_zone_getflag(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.i=(n>=1)?ENG(vm)->zone_flags[clampi(a[0].i)]:0; return r; }
static jval n_zone_setflag(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; if(n>=2) ENG(vm)->zone_flags[clampi(a[0].i)]=a[1].i; jval r; r.l=0; return r; }
static jval n_difficulty(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)a;(void)n;(void)d; jval r; r.i=ENG(vm)->difficulty; return r; }
static jval n_getSection(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.i=(n>=1)?ENG(vm)->section_stat[clampi(a[0].i)]:0; return r; }
static jval n_setSection(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; if(n>=2) ENG(vm)->section_stat[clampi(a[0].i)]=a[1].i; jval r; r.l=0; return r; }
static jval n_getTotal(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; jval r; r.i=(n>=1)?ENG(vm)->total_stat[clampi(a[0].i)]:0; return r; }
static jval n_setTotal(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)d; if(n>=2) ENG(vm)->total_stat[clampi(a[0].i)]=a[1].i; jval r; r.l=0; return r; }
static jval n_clearSectionAll(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)k;(void)a;(void)n;(void)d; memset(ENG(vm)->section_stat,0,sizeof ENG(vm)->section_stat); jval r; r.l=0; return r; }
static jval n_math_sin(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)vm;(void)k;(void)d; jval r; r.f=(n>=1)?sinf(a[0].f):0.0f; return r; }
static jval n_math_cos(jvm *vm,const char *k,const jval *a,int n,const char *d){
    (void)vm;(void)k;(void)d; jval r; r.f=(n>=1)?cosf(a[0].f):1.0f; return r; }

void bridge_install(jvm *vm, DirgeEngine *eng){
    vm->user=eng;
    /* 1) register every API method as a default logging stub */
    jvm_register_stub_table(vm, BRIDGE_API, BRIDGE_API_COUNT);

    /* 2) override the trivially-pure ones with faithful implementations */
    jvm_register_native(vm,"fake/KerberosEvent.isTutorial:()Z",                 n_isTutorial);
    jvm_register_native(vm,"fake/KerberosEvent.getflag:(I)B",                   n_evt_getflag);
    jvm_register_native(vm,"fake/KerberosEvent.setflag:(IB)V",                  n_evt_setflag);
    jvm_register_native(vm,"fake/KerberosZone.getflag:(I)B",                    n_zone_getflag);
    jvm_register_native(vm,"fake/KerberosZone.setflag:(IB)V",                   n_zone_setflag);
    jvm_register_native(vm,"fake/KerberosIntermission.getGameDifficultyLevel:()I", n_difficulty);
    jvm_register_native(vm,"fake/KerberosIntermission.getSectionStat:(I)I",     n_getSection);
    jvm_register_native(vm,"fake/KerberosIntermission.setSectionStat:(II)V",    n_setSection);
    jvm_register_native(vm,"fake/KerberosIntermission.getTotalStat:(I)I",       n_getTotal);
    jvm_register_native(vm,"fake/KerberosIntermission.setTotalStat:(II)V",      n_setTotal);
    jvm_register_native(vm,"fake/KerberosIntermission.clearSectionStatAll:()V", n_clearSectionAll);
    jvm_register_native(vm,"fake/base/KerberosMath.sin:(F)F",                   n_math_sin);
    jvm_register_native(vm,"fake/base/KerberosMath.cos:(F)F",                   n_math_cos);

    /* 3) override the subset whose engine subsystem is ported with handlers
     *    that drive the REAL native engine (DocActor/DocCamera/DocLight/
     *    EngineState). Window/Sound/Effect/GlobalMap stay logging stubs.
     *    DIRGE_NO_ENGINE=1 skips this (mutation/test seam: with it set, the
     *    engine-backed classes fall back to logging stubs and bridge_test's
     *    engine-state assertions must go red - proving they are load-bearing). */
    if(getenv("DIRGE_NO_ENGINE")==NULL) bridge_engine_install(vm, eng);
}
