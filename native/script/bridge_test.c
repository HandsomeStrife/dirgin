/*
 * bridge_test.c - demonstrates a SCRIPT driving the REAL native engine through
 * the bridge, and asserts the native engine state actually changed.
 *
 * It hand-builds one bytecode method `drive()` (a minimal real .class method:
 * raw JVM bytecode + a resolved constant pool, exactly as a decrypted script
 * resolves to) that, entirely through native-bridge dispatch:
 *   (a) new KerberosActor(); actor.load("vincent");
 *       actor.setpos(new KerberosVector(10,20,30));   -> DocActor
 *   (b) new KerberosCamera().move(1,2,3);             -> DocCamera
 *   (c) KerberosLight.set_directionvector(0, new KerberosVector(0,-1,0));
 *                                                     -> DocLightSet
 *   (d) KerberosEvent.setMainMode(5, true);           -> EngineState mode-flag
 *       zone = new KerberosZone(); zone.setflag(7,3); return zone.getflag(7);
 *                                                     -> engine flag store
 *
 * After running it we read the REAL engine subsystems out of DirgeEngine and
 * assert each one holds exactly what the script set (and differs from the
 * engine defaults it started at). Exit 0 iff every assertion holds.
 */
#include "jvm.h"
#include "classfile.h"
#include "bridge.h"
#include "bridge_engine.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails=0;
static void ok(const char *name,int cond){
    printf("  [%s] %s\n", cond?"PASS":"FAIL", name);
    if(!cond) fails++;
}
static int feq(float a,float b){ return fabsf(a-b) < 1e-4f; }

int main(void){
    printf("== bridge <-> native engine integration test ==\n");

    /* ---- constant pool (index 0 unused, like the JVM) ---------------- */
    static cf_cp_item cp[25];
    memset(cp,0,sizeof cp);
    #define CLS(i,c)        do{ cp[i].kind=CF_CP_CLASS; cp[i].u.cls=(c); }while(0)
    #define MREF(i,c,nm,ds) do{ cp[i].kind=CF_CP_METHODREF; cp[i].u.ref.cls=(c); cp[i].u.ref.name=(nm); cp[i].u.ref.desc=(ds); }while(0)
    #define FLT(i,v)        do{ cp[i].kind=CF_CP_FLOAT; cp[i].u.f=(v); }while(0)
    #define STR(i,str)      do{ cp[i].kind=CF_CP_STRING; cp[i].u.utf.s=(str); cp[i].u.utf.len=strlen(str); }while(0)

    CLS (1,  "fake/KerberosActor");
    MREF(2,  "fake/KerberosActor","<init>","()V");
    STR (3,  "vincent");
    MREF(4,  "fake/KerberosActor","load","(Ljava/lang/String;)I");
    CLS (5,  "fake/base/KerberosVector");
    MREF(6,  "fake/base/KerberosVector","<init>","(FFF)V");
    FLT (7,  10.0f);
    FLT (8,  20.0f);
    FLT (9,  30.0f);
    MREF(10, "fake/KerberosActor","setpos","(Lfake/base/KerberosVector;)V");
    CLS (11, "fake/KerberosCamera");
    MREF(12, "fake/KerberosCamera","<init>","()V");
    FLT (13, 1.0f);
    FLT (14, 2.0f);
    FLT (15, 3.0f);
    MREF(16, "fake/KerberosCamera","move","(FFF)I");
    MREF(17, "fake/KerberosLight","set_directionvector","(ILfake/base/KerberosVector;)V");
    FLT (18, 0.0f);
    FLT (19, -1.0f);
    CLS (20, "fake/KerberosZone");
    MREF(21, "fake/KerberosZone","<init>","()V");
    MREF(22, "fake/KerberosZone","setflag","(IB)V");
    MREF(23, "fake/KerberosZone","getflag","(I)B");
    MREF(24, "fake/KerberosEvent","setMainMode","(IZ)V");

    /* ---- bytecode for drive()I --------------------------------------- */
    static const uint8_t code[] = {
        /* actor = new KerberosActor() */
        0xbb,0x00,0x01,  0x59,  0xb7,0x00,0x02,  0x4b,          /* new;dup;<init>;astore_0 */
        /* actor.load("vincent"); pop */
        0x2a,  0x12,0x03,  0xb6,0x00,0x04,  0x57,               /* aload_0;ldc "vincent";load;pop */
        /* actor.setpos(new KerberosVector(10,20,30)) */
        0x2a,  0xbb,0x00,0x05,  0x59,  0x12,0x07, 0x12,0x08, 0x12,0x09,
        0xb7,0x00,0x06,  0xb6,0x00,0x0a,                        /* <init>(FFF);setpos */
        /* new KerberosCamera().move(1,2,3); pop */
        0xbb,0x00,0x0b,  0x59,  0xb7,0x00,0x0c,
        0x12,0x0d, 0x12,0x0e, 0x12,0x0f,  0xb6,0x00,0x10,  0x57,
        /* KerberosLight.set_directionvector(0, new KerberosVector(0,-1,0)) */
        0x03,  0xbb,0x00,0x05,  0x59,  0x12,0x12, 0x12,0x13, 0x12,0x12,
        0xb7,0x00,0x06,  0xb8,0x00,0x11,
        /* KerberosEvent.setMainMode(5,true) */
        0x08,  0x04,  0xb8,0x00,0x18,
        /* zone = new KerberosZone() */
        0xbb,0x00,0x14,  0x59,  0xb7,0x00,0x15,  0x4c,
        /* zone.setflag(7,3) */
        0x2b,  0x10,0x07,  0x06,  0xb6,0x00,0x16,
        /* return zone.getflag(7) */
        0x2b,  0x10,0x07,  0xb6,0x00,0x17,  0xac
    };

    static cf_class cls; static cf_method md;
    memset(&cls,0,sizeof cls); memset(&md,0,sizeof md);
    cls.cp=cp; cls.cp_count=25; cls.this_class="demo/Drive";
    md.name="drive"; md.desc="()I"; md.code=code; md.code_len=sizeof code;
    md.max_stack=8; md.max_locals=4;
    cls.methods=&md; cls.method_count=1;

    jvm vm; jvm_init(&vm,&cls);
    DirgeEngine eng; dirge_engine_init(&eng);

    /* --- baseline: the engine is at its documented defaults BEFORE the run - */
    printf("\nengine defaults before the script runs:\n");
    dvec3 eye0=doc_camera_get_position(&eng.camera);
    dvec3 dir0=eng.lights.lights[0].direction;
    printf("  DocCamera.eye   = (%.1f,%.1f,%.1f)   actors=%d   DocLight[0].dir=(%.1f,%.1f,%.1f)   mode_flags=0x%x\n",
           eye0.x,eye0.y,eye0.z, eng.actor_mgr.live_count, dir0.x,dir0.y,dir0.z, eng.estate.mode_flags);
    ok("baseline: no actors loaded",          eng.actor_mgr.live_count==0);
    ok("baseline: camera at origin",          feq(eye0.x,0)&&feq(eye0.y,0)&&feq(eye0.z,0));
    ok("baseline: light0 at engine default",  feq(dir0.x,0)&&feq(dir0.y,1.5f)&&feq(dir0.z,0.5f));
    ok("baseline: no mode flags set",         eng.estate.mode_flags==0u);
    ok("baseline: zone flag 7 clear",         eng.zone_flags[7]==0);

    bridge_install(&vm,&eng);
    vm.quiet=1;   /* silence the remaining <init> logging stubs */

    jval r=jvm_invoke(&vm,&md,NULL,0);

    printf("\nscript ran: %llu instructions, %llu native-bridge calls, error=%s\n",
           (unsigned long long)vm.insn_count,(unsigned long long)vm.native_calls,
           vm.error?vm.errmsg:"none");
    ok("script completed without error", !vm.error);

    /* --- assert the REAL engine state the script drove ---------------- */
    printf("\nnative engine state AFTER the script (asserted):\n");

    /* (a) DocActor created + positioned */
    DocActor *act=eng.actor_mgr.active_head;
    dvec3 ap = act?doc_actor_get_position(act):dvec3_make(-1,-1,-1);
    printf("  (a) DocActor: live=%d name=\"%s\" pos=(%.1f,%.1f,%.1f)\n",
           eng.actor_mgr.live_count, act&&act->name?act->name:"(null)", ap.x,ap.y,ap.z);
    ok("actor was loaded into the pool",      eng.actor_mgr.live_count==1 && act!=NULL);
    ok("actor name bound to \"vincent\"",     act && act->name && strcmp(act->name,"vincent")==0);
    ok("DocActor.position == setpos(10,20,30)", feq(ap.x,10)&&feq(ap.y,20)&&feq(ap.z,30));

    /* (b) DocCamera moved */
    dvec3 eye=doc_camera_get_position(&eng.camera);
    printf("  (b) DocCamera.eye = (%.1f,%.1f,%.1f)\n",eye.x,eye.y,eye.z);
    ok("DocCamera.eye == move(1,2,3) from origin", feq(eye.x,1)&&feq(eye.y,2)&&feq(eye.z,3));

    /* (c) DocLight direction set */
    dvec3 d=eng.lights.lights[0].direction;
    printf("  (c) DocLight[0].dir = (%.1f,%.1f,%.1f) enabled=%d type=%d\n",
           d.x,d.y,d.z, eng.lights.lights[0].enabled, eng.lights.lights[0].type);
    ok("DocLight[0].direction == set_directionvector(0,(0,-1,0))",
       feq(d.x,0)&&feq(d.y,-1)&&feq(d.z,0));
    ok("DocLight[0] enabled + DIRECTIONAL",
       eng.lights.lights[0].enabled==1 && eng.lights.lights[0].type==DOC_LIGHT_DIRECTIONAL);

    /* (d) EngineState mode-flag + zone flag round-trip */
    printf("  (d) EngineState.mode_flags=0x%08x  zone_flags[7]=%d  drive() returned=%d\n",
           eng.estate.mode_flags, eng.zone_flags[7], r.i);
    ok("EngineState mode-flag 5 set via setMainMode(5,true)",
       engine_test_mode_flag(&eng.estate,5)==1);
    ok("zone flag 7 stored == 3",             eng.zone_flags[7]==3);
    ok("script read the flag back: getflag(7) == 3", r.i==3);

    jvm_destroy(&vm);

    printf(fails? "\nBRIDGE-ENGINE TEST FAILED (%d)\n" : "\nBRIDGE-ENGINE TEST OK (all assertions passed)\n", fails);
    return fails?1:0;
}
